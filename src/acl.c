/* Access control: named users, passwords, command categories, key patterns
 * and selectors. A user carries a root permission set plus any number of
 * selectors added with (...); a command is allowed when the root or any
 * selector allows it. Key permission flags (read/write per key) are absent:
 * a selector grants every key matching its patterns.
 *
 * Command permission is stored as one bit per command id. Key permission is
 * either all keys or a list of glob patterns. A user with no password set and
 * without "nopass" cannot authenticate. */

#include "server.h"
#include "sha1.h"

#define USER_ENABLED   (1<<0)
#define USER_ALLKEYS   (1<<1)
#define USER_ALLCMD    (1<<2)
#define USER_NOPASS    (1<<3)
#define USER_ALLCHANNELS (1<<4)

/* One permission set: command bits, key globs and channel globs. A user
 * has a root selector plus any number added with (...). A command is allowed
 * when the root or any selector allows it. */
typedef struct {
    int flags;
    list *patterns;
    list *channels;
    uint64_t commands[16]; /* 1024 command ids. */
} aclSelector;

typedef struct {
    sds name;
    int flags;         /* enabled, nopass */
    list *passwords;   /* SHA1 hex of each accepted password. */
    aclSelector root;
    list *selectors;   /* aclSelector*, added with (...). */
} user;

static dict *Users;
static user *DefaultUser;
static rax *CommandIds;
static list *ACLLog;

#define ACL_LOG_CTX_TOPLEVEL 0
#define ACL_LOG_CTX_LUA 1
#define ACL_LOG_CTX_MULTI 2
#define ACL_LOG_GROUPING_MAX_TIME_DELTA 60000

#define ACL_DENIED_CMD 0
#define ACL_DENIED_KEY 1
#define ACL_DENIED_AUTH 2
#define ACL_DENIED_CHANNEL 3

void *ACLDefaultUser(void) { return DefaultUser; }

const char *ACLUserName(void *u) { return ((user*)u)->name; }

/* Keys here are SDS, but lookups also arrive as plain C strings (AUTH passes
 * a literal username). dictSdsKeyCompare calls sdslen(), which reads the byte
 * before the pointer, so a C string would fault. Compare by strlen instead and
 * only replace the value destructor, which hashDictType would free as SDS. */
static uint64_t aclUserHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, (int)strlen((char*)key));
}

static int aclUserCompare(void *privdata, const void *key1, const void *key2) {
    DICT_NOTUSED(privdata);
    return strcmp((char*)key1, (char*)key2) == 0;
}

static dictType usersDictType;

static void aclFreeUser(void *privdata, void *ptr);
static void ACLFreeLogEntry(void *leptr);
static void ACLAddLogEntry(client *c, int reason, int argpos,
                           const char *username);

void ACLInit(void) {
    usersDictType = hashDictType;
    usersDictType.hashFunction = aclUserHash;
    usersDictType.keyCompare = aclUserCompare;
    usersDictType.valDestructor = aclFreeUser;
    Users = dictCreate(&usersDictType, NULL);
    CommandIds = raxNew();
    DefaultUser = NULL;
    ACLLog = listCreate();
    listSetFreeMethod(ACLLog,ACLFreeLogEntry);
}

/* privdata is the dict's private pointer. The destructor is called as
 * (privdata, value), so dropping the first argument would free the wrong
 * pointer and crash. */
static void aclFreeUser(void *privdata, void *ptr) {
    user *u = ptr;
    DICT_NOTUSED(privdata);
    sdsfree(u->name);
    listRelease(u->passwords);
    listRelease(u->root.patterns);
    listRelease(u->root.channels);
    listRelease(u->selectors);
    zfree(u);
}

static user *ACLGetUser(sds name) {
    dictEntry *de = dictFind(Users, name);
    return de ? dictGetVal(de) : NULL;
}

static void aclInitSelector(aclSelector *s) {
    s->flags = 0;
    s->patterns = listCreate();
    s->channels = listCreate();
    listSetFreeMethod(s->patterns, (void (*)(void*))sdsfree);
    listSetFreeMethod(s->channels, (void (*)(void*))sdsfree);
}

static void aclFreeSelector(aclSelector *s) {
    listRelease(s->patterns);
    listRelease(s->channels);
    zfree(s);
}

static user *ACLCreateUser(const char *name) {
    user *u = zcalloc(sizeof(*u));
    u->name = sdsnew(name);
    u->passwords = listCreate();
    listSetFreeMethod(u->passwords, (void (*)(void*))sdsfree);
    aclInitSelector(&u->root);
    u->selectors = listCreate();
    listSetFreeMethod(u->selectors, (void (*)(void*))aclFreeSelector);
    dictAdd(Users, sdsnew(name), u);
    return u;
}

static void aclAllowAll(aclSelector *s) {
    memset(s->commands, 0xff, sizeof(s->commands));
    s->flags |= USER_ALLCMD;
}

static void aclDenyAll(aclSelector *s) {
    memset(s->commands, 0, sizeof(s->commands));
    s->flags &= ~USER_ALLCMD;
}

static int aclCommandBit(aclSelector *s, unsigned id) {
    return (s->commands[id/64] >> (id%64)) & 1;
}

static void aclSetCommandBit(aclSelector *s, unsigned id, int on) {
    uint64_t mask = 1ULL << (id%64);
    if (on) s->commands[id/64] |= mask;
    else    s->commands[id/64] &= ~mask;
    s->flags &= ~USER_ALLCMD;
}

/* Number every command once, in table order, and keep the id on the command
 * so permission checks do not scan the table. */
void ACLAssignCommandIDs(void) {
    int id = 0;
    dictIterator *di = dictGetIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (id >= 1024) serverPanic("ACL command id space exhausted");
        cmd->id = id;
        raxInsert(CommandIds, (unsigned char*)cmd->name, strlen(cmd->name),
                  (void*)(size_t)(id+1), NULL);
        id++;
    }
    dictReleaseIterator(di);
}

static int aclCategoryMatch(struct redisCommand *cmd, const char *name) {
    int f = cmd->flags;
    if (!strcasecmp(name,"all")) return 1;
    if (!strcasecmp(name,"read")) return f & CMD_READONLY;
    if (!strcasecmp(name,"write")) return f & CMD_WRITE;
    if (!strcasecmp(name,"admin")) return f & CMD_ADMIN;
    if (!strcasecmp(name,"pubsub")) return f & CMD_PUBSUB;
    if (!strcasecmp(name,"keyspace"))
        return cmd->firstkey != 0 || cmd->getkeys_proc != NULL;
    if (!strcasecmp(name,"fast")) return f & CMD_FAST;
    if (!strcasecmp(name,"slow")) return !(f & CMD_FAST);
    return -1;
}

static sds aclHashPassword(const char *pw, size_t len) {
    SHA1_CTX ctx;
    unsigned char digest[20];
    char hex[41];
    int i;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char*)pw, (uint32_t)len);
    SHA1Final(digest, &ctx);
    for (i = 0; i < 20; i++) sprintf(hex+i*2, "%02x", digest[i]);
    return sdsnewlen(hex, 40);
}

/* Apply one permission rule to a selector: command bits, key globs and
 * channel globs. Returns C_ERR for a rule that is not a permission. */
static int ACLSetSelector(aclSelector *s, const char *op) {
    if (!strcasecmp(op,"allkeys") || !strcmp(op,"~*")) {
        listEmpty(s->patterns);
        s->flags |= USER_ALLKEYS;
        return C_OK;
    }
    if (!strcasecmp(op,"resetkeys")) {
        listEmpty(s->patterns);
        s->flags &= ~USER_ALLKEYS;
        return C_OK;
    }
    if (!strcasecmp(op,"allchannels") || !strcmp(op,"&*")) {
        listEmpty(s->channels);
        s->flags |= USER_ALLCHANNELS;
        return C_OK;
    }
    if (!strcasecmp(op,"resetchannels")) {
        listEmpty(s->channels);
        s->flags &= ~USER_ALLCHANNELS;
        return C_OK;
    }
    if (op[0] == '&') {
        /* Redis refuses to add a pattern after allchannels: it would have no
         * effect until resetchannels is used. */
        if (s->flags & USER_ALLCHANNELS) return C_ERR;
        listAddNodeTail(s->channels,sdsnew(op+1));
        return C_OK;
    }
    if (!strcasecmp(op,"allcommands") || !strcasecmp(op,"+@all")) {
        aclAllowAll(s); return C_OK;
    }
    if (!strcasecmp(op,"nocommands") || !strcasecmp(op,"-@all")) {
        aclDenyAll(s); return C_OK;
    }
    if (op[0] == '~') {
        listAddNodeTail(s->patterns, sdsnew(op+1));
        s->flags &= ~USER_ALLKEYS;
        return C_OK;
    }
    if ((op[0] == '+' || op[0] == '-') && op[1] == '@') {
        int on = op[0] == '+';
        int known = 0;
        dictIterator *di = dictGetIterator(server.commands);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *cmd = dictGetVal(de);
            int match = aclCategoryMatch(cmd, op+2);
            if (match < 0) { known = -1; break; }
            if (match) { aclSetCommandBit(s, cmd->id, on); known = 1; }
        }
        dictReleaseIterator(di);
        return known < 0 ? C_ERR : C_OK;
    }
    if (op[0] == '+' || op[0] == '-') {
        void *found = raxFind(CommandIds, (unsigned char*)(op+1), strlen(op+1));
        int id = (found == raxNotFound) ? 0 : (int)(size_t)found;
        if (id == 0) return C_ERR;
        aclSetCommandBit(s, (unsigned)(id-1), op[0] == '+');
        return C_OK;
    }
    return C_ERR;
}

/* Apply one rule. op is a single token such as "on", "+@read", "~cache:*"
 * or ">secret", or a parenthesised selector. Returns C_ERR for a rule this
 * build does not understand. */
int ACLSetUser(user *u, const char *op) {
    size_t oplen = strlen(op);
    if (op[0] == '(' && oplen > 1 && op[oplen-1] == ')') {
        /* A selector is a space separated list of permission rules. */
        aclSelector *s = zcalloc(sizeof(*s));
        char *copy = zstrdup(op+1), *tok, *next;
        int ok = C_OK;
        copy[oplen-2] = '\0';
        aclInitSelector(s);
        for (tok = copy; tok && *tok; tok = next) {
            while (*tok == ' ') tok++;
            if (!*tok) break;
            next = strchr(tok, ' ');
            if (next) *next++ = '\0';
            if (ACLSetSelector(s, tok) != C_OK) { ok = C_ERR; break; }
        }
        zfree(copy);
        if (ok != C_OK) { aclFreeSelector(s); return C_ERR; }
        listAddNodeTail(u->selectors, s);
        return C_OK;
    }
    if (!strcasecmp(op,"on")) { u->flags |= USER_ENABLED; return C_OK; }
    if (!strcasecmp(op,"off")) { u->flags &= ~USER_ENABLED; return C_OK; }
    if (!strcasecmp(op,"nopass")) { u->flags |= USER_NOPASS; return C_OK; }
    if (!strcasecmp(op,"resetpass")) {
        listEmpty(u->passwords);
        u->flags &= ~USER_NOPASS;
        return C_OK;
    }
    if (!strcasecmp(op,"clearselectors")) {
        listEmpty(u->selectors);
        return C_OK;
    }
    if (!strcasecmp(op,"reset")) {
        aclDenyAll(&u->root);
        listEmpty(u->passwords);
        listEmpty(u->root.patterns);
        listEmpty(u->root.channels);
        listEmpty(u->selectors);
        u->flags = 0;
        u->root.flags = 0;
        return C_OK;
    }
    if (ACLSetSelector(&u->root, op) == C_OK) return C_OK;
    if (op[0] == '+' || op[0] == '-' || op[0] == '~' || op[0] == '&')
        return C_ERR;
    if (op[0] == '>' || op[0] == '#') {
        sds hash = op[0] == '>' ? aclHashPassword(op+1, strlen(op+1))
                                : sdsnew(op+1);
        listAddNodeTail(u->passwords, hash);
        u->flags &= ~USER_NOPASS;
        return C_OK;
    }
    return C_ERR;
}

/* Keep the default user in sync with CONFIG SET requirepass. */
void ACLUpdateDefaultUserPassword(const char *password) {
    user *u = DefaultUser;
    if (u == NULL) return;
    listEmpty(u->passwords);
    u->flags &= ~USER_NOPASS;
    if (password && password[0]) {
        sds rule = sdscatprintf(sdsempty(), ">%s", password);
        ACLSetUser(u, rule);
        sdsfree(rule);
    } else {
        u->flags |= USER_NOPASS;
    }
}

/* The default user mirrors the historical requirepass behaviour: everything
 * is allowed, and a configured password is the only way in. */
void ACLInitDefaultUser(void) {
    user *u = ACLCreateUser("default");
    aclAllowAll(&u->root);
    u->flags |= USER_ENABLED | USER_NOPASS;
    u->root.flags |= USER_ALLKEYS | USER_ALLCHANNELS;
    if (server.requirepass) {
        sds rule = sdscatprintf(sdsempty(), ">%s", server.requirepass);
        ACLSetUser(u, rule);
        sdsfree(rule);
        u->flags &= ~USER_NOPASS;
    }
    DefaultUser = u;
    /* Existing connections were authenticated against requirepass before ACL
     * existed. Attach them to the default user so they keep working. */
}

int ACLCheckPassword(user *u, const char *pw, size_t len) {
    sds hash, given;
    listIter li;
    listNode *ln;
    int match = 0;
    if (u->flags & USER_NOPASS) return C_OK;
    given = aclHashPassword(pw, len);
    listRewind(u->passwords, &li);
    while ((ln = listNext(&li))) {
        hash = ln->value;
        if (time_independent_strcmp(given, hash) == 0) match = 1;
    }
    sdsfree(given);
    return match ? C_OK : C_ERR;
}

/* A selector allows a command when it grants the command and every key
 * the command touches. Returns 1 on success, 0 when the command is refused,
 * -1 when the command is granted but a key is not. */
static int aclSelectorAllows(aclSelector *s, client *c, int *denied_key) {
    int *keys, numkeys, i;
    if (!(s->flags & USER_ALLCMD) && !aclCommandBit(s, c->cmd->id))
        return 0;
    if (s->flags & USER_ALLKEYS) return 1;
    keys = getKeysFromCommand(c->cmd, c->argv, c->argc, &numkeys);
    for (i = 0; i < numkeys; i++) {
        sds key = c->argv[keys[i]]->ptr;
        listIter li;
        listNode *ln;
        int allowed = 0;
        listRewind(s->patterns, &li);
        while ((ln = listNext(&li))) {
            sds pat = ln->value;
            if (stringmatchlen(pat, (int)sdslen(pat), key, (int)sdslen(key), 0))
                allowed = 1;
        }
        if (!allowed) {
            *denied_key = keys[i];
            getKeysFreeResult(keys);
            return -1;
        }
    }
    getKeysFreeResult(keys);
    return 1;
}

/* Returns C_OK when the command may run. The root selector and every added
 * selector are tried; the first that allows the command wins. On refusal the
 * denial is logged and C_ERR is returned. Scripts use the quiet form because
 * the Lua pseudo-client has no socket. */
static int ACLCheckCommandInternal(client *c, int reply) {
    user *u = c->user;
    listIter li;
    listNode *ln;
    int denied_key = 0, saw_key = 0, result;
    if (u == NULL) return C_OK;
    if (!(u->flags & USER_ENABLED)) {
        if (reply) addReplyError(c,"user is disabled");
        return C_ERR;
    }
    /* ACL WHOAMI / HELP report the caller's own identity or usage, so Redis 6
     * allows them for every authenticated user regardless of the acl bit. */
    if (c->cmd->proc == aclCommand && c->argc == 2 &&
        (!strcasecmp(c->argv[1]->ptr,"whoami") ||
         !strcasecmp(c->argv[1]->ptr,"help"))) return C_OK;
    result = aclSelectorAllows(&u->root, c, &denied_key);
    if (result == 1) return C_OK;
    if (result < 0) saw_key = 1;
    listRewind(u->selectors, &li);
    while ((ln = listNext(&li))) {
        result = aclSelectorAllows(ln->value, c, &denied_key);
        if (result == 1) return C_OK;
        if (result < 0) saw_key = 1;
    }
    if (saw_key) {
        ACLAddLogEntry(c,ACL_DENIED_KEY,denied_key,NULL);
        if (reply) addReplyError(c,"key access denied");
    } else {
        ACLAddLogEntry(c,ACL_DENIED_CMD,0,NULL);
        if (reply)
            addReplyErrorFormat(c,"user has no permission to run '%s'",
                                c->cmd->name);
    }
    return C_ERR;
}

int ACLCheckCommand(client *c) {
    return ACLCheckCommandInternal(c,1);
}

int ACLCheckCommandQuiet(client *c) {
    return ACLCheckCommandInternal(c,0);
}

/* Channel permission check for SUBSCRIBE/PSUBSCRIBE/PUBLISH/SPUBLISH.
 * Literal channels are glob-matched against the user's channel patterns. A
 * pattern subscription must match an ACL pattern literally: accepting a
 * wider pattern would allow channels outside the ACL. */
/* A selector allows a channel when it grants all channels or one of its
 * patterns matches. A pattern subscription must match an ACL pattern
 * literally: accepting a wider pattern would allow channels outside the ACL. */
static int aclSelectorAllowsChannel(aclSelector *s, robj *channel,
                                    int is_pattern) {
    listIter li;
    listNode *ln;
    if (s->flags & USER_ALLCHANNELS) return 1;
    listRewind(s->channels,&li);
    while ((ln = listNext(&li)) != NULL) {
        sds pat = ln->value;
        if (is_pattern) {
            if (sdslen(pat) == sdslen(channel->ptr) &&
                memcmp(pat,channel->ptr,sdslen(pat)) == 0) return 1;
        } else if (stringmatchlen(pat,(int)sdslen(pat),
                                  channel->ptr,(int)sdslen(channel->ptr),0)) {
            return 1;
        }
    }
    return 0;
}

int ACLCheckPubsubPermission(client *c, robj *channel, int is_pattern) {
    user *u = c->user;
    listIter li;
    listNode *ln;

    if (u == NULL) return C_OK;
    if (aclSelectorAllowsChannel(&u->root, channel, is_pattern)) return C_OK;
    listRewind(u->selectors,&li);
    while ((ln = listNext(&li)) != NULL)
        if (aclSelectorAllowsChannel(ln->value, channel, is_pattern)) return C_OK;
    return C_ERR;
}

/* Check every channel argument of a Pub/Sub command before it runs, so a
 * multi-channel SUBSCRIBE is refused as a whole. */
static int ACLCheckChannelPermissionsInternal(client *c, int reply) {
    int j, last = c->argc, is_pattern = 0;

    if (c->user == NULL) return C_OK;
    if (c->cmd->proc == subscribeCommand ||
        c->cmd->proc == ssubscribeCommand) {
        /* Every argument is a literal channel. */
    } else if (c->cmd->proc == psubscribeCommand) {
        is_pattern = 1;
    } else if (c->cmd->proc == publishCommand ||
               c->cmd->proc == spublishCommand) {
        last = 2; /* Only the channel argument. */
    } else {
        return C_OK;
    }
    for (j = 1; j < last; j++) {
        if (ACLCheckPubsubPermission(c,c->argv[j],is_pattern) == C_ERR) {
            ACLAddLogEntry(c,ACL_DENIED_CHANNEL,j,NULL);
            if (reply)
                addReplyError(c,"-NOPERM this user has no permissions to "
                                "access one of the channels used as "
                                "arguments");
            return C_ERR;
        }
    }
    return C_OK;
}

int ACLCheckChannelPermissions(client *c) {
    return ACLCheckChannelPermissionsInternal(c,1);
}

int ACLCheckChannelPermissionsQuiet(client *c) {
    return ACLCheckChannelPermissionsInternal(c,0);
}

/* Authenticate the client as username/password. On success attaches the user
 * and marks the connection authenticated. On failure clears auth state, logs
 * the denial, and returns C_ERR without sending a reply (caller chooses the
 * error text so AUTH and HELLO can keep their own messages). */
int ACLAuthenticateUser(client *c, robj *username, robj *password) {
    const char *userstr = username->ptr;
    user *u = ACLGetUser((sds)userstr);
    size_t pwlen = sdslen(password->ptr);

    if (u == NULL || !(u->flags & USER_ENABLED) ||
        ACLCheckPassword(u, password->ptr, pwlen) != C_OK) {
        ACLAddLogEntry(c,ACL_DENIED_AUTH,0,userstr);
        c->authenticated = 0;
        c->user = NULL;
        return C_ERR;
    }
    c->authenticated = 1;
    c->user = u;
    return C_OK;
}

void authCommand(client *c) {
    robj *username, *password;
    robj *default_user = NULL;
    if (c->argc > 3) {
        addReplyError(c,"wrong number of arguments for 'auth' command");
        return;
    }
    if (c->argc == 2) {
        default_user = createStringObject("default",7);
        username = default_user;
        password = c->argv[1];
    } else {
        username = c->argv[1];
        password = c->argv[2];
    }
    if (ACLAuthenticateUser(c, username, password) != C_OK) {
        addReplyError(c,"invalid username-password pair or user is disabled");
        if (default_user) decrRefCount(default_user);
        return;
    }
    if (default_user) decrRefCount(default_user);
    addReply(c, shared.ok);
}

/* Describe the command rules of one selector in the same syntax accepted by
 * ACL SETUSER, for ACL GETUSER and ACL SAVE. */
static sds ACLDescribeUserCommandRules(aclSelector *s) {
    sds rules = sdsempty();
    raxIterator ri;

    if (s->flags & USER_ALLCMD) return sdscat(rules,"+@all");
    rules = sdscat(rules,"-@all");
    raxStart(&ri,CommandIds);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        unsigned id = (unsigned)(size_t)ri.data - 1;
        if (aclCommandBit(s,id)) {
            rules = sdscat(rules," +");
            rules = sdscatlen(rules,ri.key,ri.key_len);
        }
    }
    raxStop(&ri);
    return rules;
}

static int aclSelectorAllowsCommand(aclSelector *s, struct redisCommand *cmd,
                                    robj **argv, int argc);

/* Check whether a user may run the given command with the given arguments,
 * without executing it (ACL DRYRUN). On refusal *err is set to an sds
 * describing the reason and the caller must free it. */
static int aclCheckUserPerm(user *u, struct redisCommand *cmd,
                            robj **argv, int argc, sds *err)
{
    if (!(u->flags & USER_ENABLED)) {
        *err = sdsnew("User is disabled");
        return C_ERR;
    }
    if (aclSelectorAllowsCommand(&u->root, cmd, argv, argc)) return C_OK;
    {
        listIter li;
        listNode *ln;
        listRewind(u->selectors,&li);
        while ((ln = listNext(&li)))
            if (aclSelectorAllowsCommand(ln->value, cmd, argv, argc)) return C_OK;
    }
    *err = sdscatprintf(sdsempty(),
        "This user has no permissions to run the '%s' command or "
        "to access one of the keys used as arguments", cmd->name);
    return C_ERR;
}

/* Whether one selector grants a command and every key it touches. Used by
 * DRYRUN, which has a command and arguments but no client. */
static int aclSelectorAllowsCommand(aclSelector *s, struct redisCommand *cmd,
                                    robj **argv, int argc) {
    int *keys, numkeys, i;
    if (!(s->flags & USER_ALLCMD) && !aclCommandBit(s, cmd->id)) return 0;
    if (s->flags & USER_ALLKEYS) return 1;
    keys = getKeysFromCommand(cmd, argv, argc, &numkeys);
    for (i = 0; i < numkeys; i++) {
        sds key = argv[keys[i]]->ptr;
        listIter li;
        listNode *ln;
        int allowed = 0;
        listRewind(s->patterns,&li);
        while ((ln = listNext(&li))) {
            sds pat = ln->value;
            if (stringmatchlen(pat,(int)sdslen(pat),key,(int)sdslen(key),0))
                allowed = 1;
        }
        if (!allowed) { if (keys) getKeysFreeResult(keys); return 0; }
    }
    if (keys) getKeysFreeResult(keys);
    return 1;
}

/* =============================================================================
 * ACL log
 * ==========================================================================*/

typedef struct ACLLogEntry {
    uint64_t count;
    int reason;
    int context;
    sds object;
    sds username;
    mstime_t ctime;
    sds cinfo;
} ACLLogEntry;

static void ACLFreeLogEntry(void *leptr) {
    ACLLogEntry *le = leptr;
    sdsfree(le->object);
    sdsfree(le->username);
    sdsfree(le->cinfo);
    zfree(le);
}

static int ACLLogMatchEntry(ACLLogEntry *a, ACLLogEntry *b) {
    if (a->reason != b->reason) return 0;
    if (a->context != b->context) return 0;
    mstime_t delta = a->ctime - b->ctime;
    if (delta < 0) delta = -delta;
    if (delta > ACL_LOG_GROUPING_MAX_TIME_DELTA) return 0;
    if (sdscmp(a->object,b->object) != 0) return 0;
    if (sdscmp(a->username,b->username) != 0) return 0;
    return 1;
}

/* Record a denied operation. argpos is the argument index of the key that was
 * refused; username is only used for failed authentication. */
static void ACLAddLogEntry(client *c, int reason, int argpos,
                           const char *username)
{
    ACLLogEntry *le = zmalloc(sizeof(*le));
    le->count = 1;
    le->reason = reason;
    le->username = sdsnew(reason == ACL_DENIED_AUTH ? username :
                          ((user*)c->user)->name);
    le->ctime = mstime();

    switch(reason) {
    case ACL_DENIED_CMD: le->object = sdsnew(c->cmd->name); break;
    case ACL_DENIED_KEY: le->object = sdsdup(c->argv[argpos]->ptr); break;
    case ACL_DENIED_AUTH: le->object = sdsnew(username); break;
    case ACL_DENIED_CHANNEL: le->object = sdsdup(c->argv[argpos]->ptr); break;
    default: le->object = sdsempty();
    }

    client *realclient = c;
    if (realclient->flags & CLIENT_LUA && server.lua_caller)
        realclient = server.lua_caller;
    le->cinfo = catClientInfoString(sdsempty(),realclient);
    if (c->flags & CLIENT_MULTI) {
        le->context = ACL_LOG_CTX_MULTI;
    } else if (c->flags & CLIENT_LUA) {
        le->context = ACL_LOG_CTX_LUA;
    } else {
        le->context = ACL_LOG_CTX_TOPLEVEL;
    }

    if (server.acllog_max_len == 0) {
        ACLFreeLogEntry(le);
        return;
    }

    /* Only a few recent entries are scanned for a duplicate. */
    long toscan = 10;
    listIter li;
    listNode *ln;
    listRewind(ACLLog,&li);
    while (toscan-- && (ln = listNext(&li)) != NULL) {
        ACLLogEntry *current = listNodeValue(ln);
        if (ACLLogMatchEntry(current,le)) {
            current->count++;
            current->ctime = le->ctime;
            sdsfree(current->cinfo);
            current->cinfo = le->cinfo;
            le->cinfo = NULL;
            /* Move the entry to the head without freeing its value: the
             * list has a free method, which listDelNode() would invoke. */
            void (*freefn)(void*) = ACLLog->free;
            ACLLog->free = NULL;
            listDelNode(ACLLog,ln);
            ACLLog->free = freefn;
            listAddNodeHead(ACLLog,current);
            ACLFreeLogEntry(le);
            return;
        }
    }

    listAddNodeHead(ACLLog,le);
    while (listLength(ACLLog) > server.acllog_max_len) {
        listNode *tail = listLast(ACLLog);
        listDelNode(ACLLog,tail);
    }
}

static const char *aclLogReasonName(int reason) {
    switch(reason) {
    case ACL_DENIED_CMD: return "command";
    case ACL_DENIED_CHANNEL: return "channel";
    case ACL_DENIED_KEY: return "key";
    case ACL_DENIED_AUTH: return "auth";
    default: return "unknown";
    }
}

static const char *aclLogContextName(int context) {
    switch(context) {
    case ACL_LOG_CTX_TOPLEVEL: return "toplevel";
    case ACL_LOG_CTX_LUA: return "lua";
    case ACL_LOG_CTX_MULTI: return "multi";
    default: return "unknown";
    }
}

/* =============================================================================
 * ACL file persistence
 * ==========================================================================*/

static user *ACLCreateUserIn(dict *users, const char *name) {
    user *u = zcalloc(sizeof(*u));
    u->name = sdsnew(name);
    u->passwords = listCreate();
    listSetFreeMethod(u->passwords, (void (*)(void*))sdsfree);
    aclInitSelector(&u->root);
    u->selectors = listCreate();
    listSetFreeMethod(u->selectors, (void (*)(void*))aclFreeSelector);
    dictAdd(users, sdsnew(name), u);
    return u;
}

static list *aclCopySdsList(list *src) {
    list *dst = listCreate();
    listIter li;
    listNode *ln;
    listSetFreeMethod(dst, (void (*)(void*))sdsfree);
    listRewind(src,&li);
    while ((ln = listNext(&li))) listAddNodeTail(dst,sdsdup(ln->value));
    return dst;
}

/* Copy every rule of src into dst, keeping dst's name and address. */
static aclSelector *aclCloneSelector(aclSelector *src) {
    aclSelector *s = zcalloc(sizeof(*s));
    s->flags = src->flags;
    s->patterns = aclCopySdsList(src->patterns);
    s->channels = aclCopySdsList(src->channels);
    memcpy(s->commands, src->commands, sizeof(s->commands));
    return s;
}

static void ACLCopyUser(user *dst, user *src) {
    listIter li;
    listNode *ln;
    listRelease(dst->passwords);
    listRelease(dst->root.patterns);
    listRelease(dst->root.channels);
    listEmpty(dst->selectors);
    dst->passwords = aclCopySdsList(src->passwords);
    dst->root.patterns = aclCopySdsList(src->root.patterns);
    dst->root.channels = aclCopySdsList(src->root.channels);
    dst->flags = src->flags;
    dst->root.flags = src->root.flags;
    memcpy(dst->root.commands, src->root.commands, sizeof(dst->root.commands));
    listRewind(src->selectors,&li);
    while ((ln = listNext(&li)))
        listAddNodeTail(dst->selectors, aclCloneSelector(ln->value));
}

/* Delete a user and disconnect every client authenticated as it. Clients keep
 * a pointer to their user, so the user cannot be freed underneath them. */
static void ACLFreeUserAndKillClients(user *u) {
    listIter li;
    listNode *ln;
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (c->user == u) {
            c->user = DefaultUser;
            c->authenticated = 0;
            if (c == server.current_client)
                c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            else
                freeClientAsync(c);
        }
    }
    aclFreeUser(NULL,u);
}

static sds aclAppendSelector(sds d, aclSelector *s);

/* Describe all rules of a user in the syntax accepted by ACL SETUSER. */
static sds ACLDescribeUser(user *u) {
    sds d = sdsempty();
    listIter li;
    listNode *ln;

    d = sdscat(d,(u->flags & USER_ENABLED) ? "on" : "off");
    if (u->flags & USER_NOPASS) d = sdscat(d," nopass");
    listRewind(u->passwords,&li);
    while ((ln = listNext(&li)))
        d = sdscatprintf(d," #%s",(char*)ln->value);
    d = aclAppendSelector(d, &u->root);
    listRewind(u->selectors,&li);
    while ((ln = listNext(&li))) {
        sds body = aclAppendSelector(sdsempty(), ln->value);
        d = sdscat(d," (");
        d = sdscatsds(d, body);
        d = sdscat(d,")");
        sdsfree(body);
    }
    return d;
}

/* Append one selector's key, channel and command rules. */
static sds aclAppendSelector(sds d, aclSelector *s) {
    listIter li;
    listNode *ln;
    if (s->flags & USER_ALLKEYS) d = sdscat(d," ~*");
    listRewind(s->patterns,&li);
    while ((ln = listNext(&li)))
        d = sdscatprintf(d," ~%s",(char*)ln->value);
    if (s->flags & USER_ALLCHANNELS) {
        d = sdscat(d," allchannels");
    } else {
        d = sdscat(d," resetchannels");
        listRewind(s->channels,&li);
        while ((ln = listNext(&li)))
            d = sdscatprintf(d," &%s",(char*)ln->value);
    }
    sds rules = ACLDescribeUserCommandRules(s);
    d = sdscat(d," ");
    d = sdscatsds(d,rules);
    sdsfree(rules);
    return d;
}

int ACLSaveToFile(const char *filename) {
    sds acl = sdsempty();
    dictIterator *di = dictGetIterator(Users);
    dictEntry *de;

    while ((de = dictNext(di)) != NULL) {
        user *u = dictGetVal(de);
        sds descr = ACLDescribeUser(u);
        acl = sdscatprintf(acl,"user %s %s\n",u->name,descr);
        sdsfree(descr);
    }
    dictReleaseIterator(di);

    sds tmp = sdscatprintf(sdsempty(),"%s.tmp-%d-%lld",filename,
                           (int)getpid(),(long long)mstime());
    FILE *fp = fopen(tmp,"wb");
    if (fp == NULL) {
        serverLog(LL_WARNING,"Opening temp ACL file for ACL SAVE: %s",
                  strerror(errno));
        sdsfree(tmp);
        sdsfree(acl);
        return C_ERR;
    }
    size_t acllen = sdslen(acl);
    size_t written = fwrite(acl,1,acllen,fp);
    int closeerr = fclose(fp);
    sdsfree(acl);
    if (written != acllen || closeerr != 0) {
        serverLog(LL_WARNING,"Writing ACL file for ACL SAVE failed");
        unlink(tmp);
        sdsfree(tmp);
        return C_ERR;
    }
    if (rename(tmp,filename) == -1) {
#ifdef _WIN32
        /* MSVC rename() does not replace an existing destination. */
        unlink(filename);
        if (rename(tmp,filename) == -1) {
#endif
            serverLog(LL_WARNING,"Renaming ACL file for ACL SAVE: %s",
                      strerror(errno));
            unlink(tmp);
            sdsfree(tmp);
            return C_ERR;
#ifdef _WIN32
        }
#endif
    }
    sdsfree(tmp);
    return C_OK;
}

/* Load users from an ACL file. Returns NULL on success, or an error message
 * the caller must free. Existing users keep their addresses so authenticated
 * clients stay valid; users missing from the file are disconnected. */
sds ACLLoadFromFile(const char *filename) {
    FILE *fp = fopen(filename,"r");
    if (fp == NULL) {
        serverLog(LL_WARNING,"Error loading ACLs, opening file '%s': %s",
                  filename,strerror(errno));
        return C_ERR;
    }
    sds acls = sdsempty();
    char buf[1024];
    while (fgets(buf,sizeof(buf),fp) != NULL) acls = sdscat(acls,buf);
    fclose(fp);

    int totlines;
    sds *lines = sdssplitlen(acls,sdslen(acls),"\n",1,&totlines);
    sdsfree(acls);

    dict *newusers = dictCreate(&usersDictType,NULL);
    user *newdefault = ACLCreateUserIn(newusers,"default");
    aclAllowAll(&newdefault->root);
    newdefault->flags |= USER_ENABLED | USER_NOPASS;
    newdefault->root.flags |= USER_ALLKEYS | USER_ALLCHANNELS;
    if (server.requirepass) {
        sds rule = sdscatprintf(sdsempty(), ">%s", server.requirepass);
        ACLSetUser(newdefault, rule);
        sdsfree(rule);
        newdefault->flags &= ~USER_NOPASS;
    }

    sds errors = sdsempty();
    int sawdefault = 0;
    for (int i = 0; i < totlines; i++) {
        sds line = sdstrim(lines[i]," \t\r\n");
        if (line[0] == '\0') continue;

        int argc;
        sds *argv = sdssplitlen(line,sdslen(line)," ",1,&argc);
        if (argv == NULL) {
            errors = sdscatprintf(errors,"%s:%d: unbalanced quotes. ",
                                  filename,i+1);
            continue;
        }
        if (argc == 0 || strcasecmp(argv[0],"user") != 0) {
            errors = sdscatprintf(errors,
                "%s:%d: line should start with 'user'. ",filename,i+1);
            sdsfreesplitres(argv,argc);
            continue;
        }
        if (argc < 2) {
            errors = sdscatprintf(errors,"%s:%d: missing user name. ",
                                  filename,i+1);
            sdsfreesplitres(argv,argc);
            continue;
        }
        user *u = dictFetchValue(newusers,argv[1]);
        if (u == NULL) u = ACLCreateUserIn(newusers,argv[1]);
        if (!strcmp(argv[1],"default")) sawdefault = 1;
        for (int j = 2; j < argc; j++) {
            if (argv[j][0] == '\0') continue;
            if (ACLSetUser(u,argv[j]) != C_OK) {
                errors = sdscatprintf(errors,"%s:%d: invalid rule '%s'. ",
                                      filename,i+1,argv[j]);
            }
        }
        sdsfreesplitres(argv,argc);
    }

    if (sdslen(errors) == 0 && !sawdefault) {
        errors = sdscatprintf(errors,
            "%s: ACL file must contain a 'user default' line. ",filename);
    }

    if (sdslen(errors) != 0) {
        dictRelease(newusers);
        sdsfreesplitres(lines,totlines);
        return errors;
    }

    /* Copy rules into the live users, creating the ones that are new. */
    dictIterator *di = dictGetIterator(newusers);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        const char *name = dictGetKey(de);
        user *nu = dictGetVal(de);
        if (!strcmp(name,"default")) continue;
        user *old = ACLGetUser((sds)name);
        if (old) {
            ACLCopyUser(old,nu);
        } else {
            user *created = ACLCreateUser(name);
            ACLCopyUser(created,nu);
        }
    }
    dictReleaseIterator(di);
    ACLCopyUser(DefaultUser,newdefault);

    /* Remove users that no longer appear in the file. */
    di = dictGetSafeIterator(Users);
    while ((de = dictNext(di)) != NULL) {
        sds name = dictGetKey(de);
        if (!strcmp(name,"default")) continue;
        if (dictFind(newusers,name) != NULL) continue;
        user *u = dictGetVal(de);
        dictEntry *he = dictUnlink(Users,name);
        if (he == NULL) continue;
        ACLFreeUserAndKillClients(u);
        sdsfree(dictGetKey(he));
        zfree(he);
    }
    dictReleaseIterator(di);
    dictRelease(newusers);

    sdsfreesplitres(lines,totlines);
    return NULL;
}

void aclCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"list")) {
        dictIterator *di = dictGetIterator(Users);
        dictEntry *de;
        addReplyMultiBulkLen(c, dictSize(Users));
        while ((de = dictNext(di)) != NULL) {
            user *u = dictGetVal(de);
            sds descr = ACLDescribeUser(u);
            sds line = sdscatprintf(sdsempty(), "user %s %s", u->name, descr);
            addReplyBulkCBuffer(c, line, sdslen(line));
            sdsfree(line);
            sdsfree(descr);
        }
        dictReleaseIterator(di);
        return;
    }
    if (!strcasecmp(c->argv[1]->ptr,"log") &&
        (c->argc == 2 || c->argc == 3))
    {
        long count = 10;
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"reset")) {
                listEmpty(ACLLog);
                addReply(c,shared.ok);
                return;
            }
            if (getLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK)
                return;
            if (count < 0) {
                addReplyError(c,"count must be positive");
                return;
            }
        }
        PORT_LONG n = (PORT_LONG)listLength(ACLLog);
        if (count < n) n = count;
        addReplyMultiBulkLen(c,n);
        listIter li;
        listNode *ln;
        listRewind(ACLLog,&li);
        while (n-- && (ln = listNext(&li)) != NULL) {
            ACLLogEntry *le = listNodeValue(ln);
            addReplyMultiBulkLen(c,14);
            addReplyBulkCString(c,"count");
            addReplyLongLong(c,le->count);
            addReplyBulkCString(c,"reason");
            addReplyBulkCString(c,aclLogReasonName(le->reason));
            addReplyBulkCString(c,"context");
            addReplyBulkCString(c,aclLogContextName(le->context));
            addReplyBulkCString(c,"object");
            addReplyBulkCBuffer(c,le->object,sdslen(le->object));
            addReplyBulkCString(c,"username");
            addReplyBulkCBuffer(c,le->username,sdslen(le->username));
            addReplyBulkCString(c,"age-seconds");
            addReplyDouble(c,(double)(mstime()-le->ctime)/1000);
            addReplyBulkCString(c,"client-info");
            addReplyBulkCBuffer(c,le->cinfo,sdslen(le->cinfo));
        }
        return;
    }
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"save")) {
        if (server.acl_filename == NULL) {
            addReplyError(c,"This Redis instance is not configured to use an "
                            "ACL file. You can only use ACL SAVE if you have "
                            "configured an aclfile in the Redis "
                            "configuration.");
            return;
        }
        if (ACLSaveToFile(server.acl_filename) == C_OK) {
            addReply(c,shared.ok);
        } else {
            addReplyError(c,"There was an error trying to save the ACLs. "
                            "Please check the server logs for more "
                            "information");
        }
        return;
    }
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"load")) {
        if (server.acl_filename == NULL) {
            addReplyError(c,"This Redis instance is not configured to use an "
                            "ACL file. You can only use ACL LOAD if you have "
                            "configured an aclfile in the Redis "
                            "configuration.");
            return;
        }
        sds err = ACLLoadFromFile(server.acl_filename);
        if (err) {
            addReplyErrorFormat(c,"Error loading ACL file %s, %s",
                                server.acl_filename, err);
            sdsfree(err);
        } else {
            addReply(c,shared.ok);
        }
        return;
    }
    if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"whoami")) {
        addReply(c, shared.syntaxerr);
        return;
    }
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"whoami")) {
        sds name = c->user ? ((user*)c->user)->name : sdsnew("default");
        addReplyBulkCBuffer(c, name, sdslen(name));
        if (!c->user) sdsfree(name);
        return;
    }
    if (c->argc >= 3 && !strcasecmp(c->argv[1]->ptr,"setuser")) {
        sds name = c->argv[2]->ptr;
        user *u = ACLGetUser(name);
        int j;
        if (u == NULL) u = ACLCreateUser(name);
        for (j = 3; j < c->argc; j++) {
            if (ACLSetUser(u, c->argv[j]->ptr) != C_OK) {
                addReplyErrorFormat(c,"unknown ACL rule '%s'",
                    (char*)c->argv[j]->ptr);
                return;
            }
        }
        addReply(c, shared.ok);
        return;
    }
    if (c->argc >= 3 && !strcasecmp(c->argv[1]->ptr,"deluser")) {
        int deleted = 0, j;
        for (j = 2; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr,"default")) {
                addReplyError(c,"the default user cannot be removed");
                return;
            }
            user *u = ACLGetUser(c->argv[j]->ptr);
            if (u == NULL) continue;
            dictEntry *he = dictUnlink(Users,c->argv[j]->ptr);
            if (he == NULL) continue;
            ACLFreeUserAndKillClients(u);
            sdsfree(dictGetKey(he));
            zfree(he);
            deleted++;
        }
        addReplyLongLong(c,deleted);
        return;
    }
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"users")) {
        dictIterator *di = dictGetIterator(Users);
        dictEntry *de;
        addReplyMultiBulkLen(c, dictSize(Users));
        while ((de = dictNext(di)) != NULL) {
            user *u = dictGetVal(de);
            addReplyBulkCBuffer(c, u->name, sdslen(u->name));
        }
        dictReleaseIterator(di);
        return;
    }
    if (!strcasecmp(c->argv[1]->ptr,"cat") && (c->argc == 2 || c->argc == 3)) {
        static const char *categories[] = {
            "keyspace", "read", "write", "admin", "pubsub", "fast", "slow",
            NULL
        };
        int i;
        if (c->argc == 2) {
            for (i = 0; categories[i]; i++);
            addReplyMultiBulkLen(c, i);
            for (i = 0; categories[i]; i++)
                addReplyBulkCString(c, categories[i]);
            return;
        }
        for (i = 0; categories[i]; i++) {
            if (!strcasecmp(categories[i], c->argv[2]->ptr)) break;
        }
        if (categories[i] == NULL) {
            addReplyErrorFormat(c,"Unknown ACL cat '%s'",(char*)c->argv[2]->ptr);
            return;
        }
        /* Count the matching commands first, then list them. */
        dictIterator *di = dictGetIterator(server.commands);
        dictEntry *de;
        PORT_LONG count = 0;
        while ((de = dictNext(di)) != NULL) {
            if (aclCategoryMatch(dictGetVal(de), c->argv[2]->ptr) != 0) count++;
        }
        dictReleaseIterator(di);
        addReplyMultiBulkLen(c, count);
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *cmd = dictGetVal(de);
            if (aclCategoryMatch(cmd, c->argv[2]->ptr) != 0)
                addReplyBulkCString(c, cmd->name);
        }
        dictReleaseIterator(di);
        return;
    }
    if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"getuser")) {
        user *u = ACLGetUser(c->argv[2]->ptr);
        if (u == NULL) {
            addReply(c,shared.nullmultibulk);
            return;
        }
        addReplyMultiBulkLen(c,12);

        int numflags = 1;
        if (u->root.flags & USER_ALLKEYS) numflags++;
        if (u->root.flags & USER_ALLCMD) numflags++;
        if (u->flags & USER_NOPASS) numflags++;
        addReplyBulkCString(c,"flags");
        addReplyMultiBulkLen(c,numflags);
        addReplyBulkCString(c,(u->flags & USER_ENABLED) ? "on" : "off");
        if (u->root.flags & USER_ALLKEYS) addReplyBulkCString(c,"allkeys");
        if (u->root.flags & USER_ALLCMD) addReplyBulkCString(c,"allcommands");
        if (u->flags & USER_NOPASS) addReplyBulkCString(c,"nopass");

        addReplyBulkCString(c,"passwords");
        addReplyMultiBulkLen(c, listLength(u->passwords));
        listIter li;
        listNode *ln;
        listRewind(u->passwords,&li);
        while ((ln = listNext(&li))) {
            sds pw = ln->value;
            addReplyBulkCBuffer(c, pw, sdslen(pw));
        }

        addReplyBulkCString(c,"commands");
        addReplyBulkSds(c, ACLDescribeUserCommandRules(&u->root));

        addReplyBulkCString(c,"keys");
        if (u->root.flags & USER_ALLKEYS) {
            addReplyMultiBulkLen(c,1);
            addReplyBulkCString(c,"~*");
        } else {
            addReplyMultiBulkLen(c, listLength(u->root.patterns));
            listRewind(u->root.patterns,&li);
            while ((ln = listNext(&li))) {
                sds pat = ln->value;
                addReplyBulkSds(c, sdscatprintf(sdsempty(),"~%s",pat));
            }
        }

        addReplyBulkCString(c,"channels");
        if (u->root.flags & USER_ALLCHANNELS) {
            addReplyMultiBulkLen(c,1);
            addReplyBulkCString(c,"&*");
        } else {
            addReplyMultiBulkLen(c,listLength(u->root.channels));
            listRewind(u->root.channels,&li);
            while ((ln = listNext(&li))) {
                sds pat = ln->value;
                addReplyBulkSds(c, sdscatprintf(sdsempty(),"&%s",pat));
            }
        }

        addReplyBulkCString(c,"selectors");
        addReplyMultiBulkLen(c, listLength(u->selectors));
        listRewind(u->selectors,&li);
        while ((ln = listNext(&li)))
            addReplyBulkSds(c, aclAppendSelector(sdsempty(), ln->value));
        return;
    }
    if (!strcasecmp(c->argv[1]->ptr,"genpass") &&
        (c->argc == 2 || c->argc == 3))
    {
        #define ACL_GENPASS_MAX_BITS 4096
        char pass[ACL_GENPASS_MAX_BITS/8*2];
        PORT_LONGLONG bits = 256;
        if (c->argc == 3) {
            if (getLongLongFromObjectOrReply(c,c->argv[2],&bits,NULL) != C_OK)
                return;
            if (bits <= 0 || bits > ACL_GENPASS_MAX_BITS) {
                addReplyError(c,
                    "ACL GENPASS argument must be the number of bits for "
                    "the output password, a positive number up to 4096");
                return;
            }
        }
        PORT_LONG chars = (PORT_LONG)((bits+3)/4);
        getRandomHexChars(pass,chars);
        addReplyBulkCBuffer(c,pass,chars);
        return;
    }
    if (c->argc >= 4 && !strcasecmp(c->argv[1]->ptr,"dryrun")) {
        user *u = ACLGetUser(c->argv[2]->ptr);
        struct redisCommand *cmd;
        sds err = NULL;
        if (u == NULL) {
            addReplyErrorFormat(c,"User '%s' not found",(char*)c->argv[2]->ptr);
            return;
        }
        cmd = lookupCommand(c->argv[3]->ptr);
        if (cmd == NULL) {
            addReplyErrorFormat(c,"Command '%s' does not exist",
                (char*)c->argv[3]->ptr);
            return;
        }
        if (aclCheckUserPerm(u,cmd,c->argv+3,c->argc-3,&err) == C_OK) {
            addReply(c,shared.ok);
        } else {
            addReplyError(c,err);
            sdsfree(err);
        }
        return;
    }
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"CAT [<category>]",
"    List the ACL categories, or the commands inside a category.",
"DELUSER <username> [<username> ...]",
"    Delete one or more users.",
"DRYRUN <username> <command> [<arg> ...]",
"    Check whether a user can run a command without executing it.",
"GENPASS [<bits>]",
"    Generate a secure random password string.",
"GETUSER <username>",
"    Return the rules describing a user.",
"LIST",
"    Show user configurations as ACL rule strings.",
"LOAD",
"    Reload users from the configured ACL file.",
"LOG [<count>|RESET]",
"    Show or clear recent ACL security events.",
"SAVE",
"    Save the current ACL rules to the configured ACL file.",
"SETUSER <username> [<rule> ...]",
"    Create or modify a user.",
"USERS",
"    List all registered usernames.",
"WHOAMI",
"    Return the username of the current connection.",
NULL
        };
        addReplyHelp(c, help);
        return;
    }
    addReplyError(c,"unknown ACL subcommand or wrong number of arguments");
}
