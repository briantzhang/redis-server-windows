/* Access control at the Redis 6 level: named users, passwords, command
 * categories and key patterns. Redis 7 selectors (several permission sets per
 * user) and the key-permission flags are intentionally absent.
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

typedef struct {
    sds name;
    int flags;
    list *passwords;   /* SHA1 hex of each accepted password. */
    list *patterns;    /* Key globs. Empty unless USER_ALLKEYS is off. */
    uint64_t commands[16]; /* 1024 command ids. */
} user;

static dict *Users;
static user *DefaultUser;
static rax *CommandIds;

void *ACLDefaultUser(void) { return DefaultUser; }

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

void ACLInit(void) {
    usersDictType = hashDictType;
    usersDictType.hashFunction = aclUserHash;
    usersDictType.keyCompare = aclUserCompare;
    usersDictType.valDestructor = aclFreeUser;
    Users = dictCreate(&usersDictType, NULL);
    CommandIds = raxNew();
    DefaultUser = NULL;
}

/* privdata is the dict's private pointer. The destructor is called as
 * (privdata, value), so dropping the first argument would free the wrong
 * pointer and crash. */
static void aclFreeUser(void *privdata, void *ptr) {
    user *u = ptr;
    DICT_NOTUSED(privdata);
    sdsfree(u->name);
    listRelease(u->passwords);
    listRelease(u->patterns);
    zfree(u);
}

static user *ACLGetUser(sds name) {
    dictEntry *de = dictFind(Users, name);
    return de ? dictGetVal(de) : NULL;
}

static user *ACLCreateUser(const char *name) {
    user *u = zcalloc(sizeof(*u));
    u->name = sdsnew(name);
    u->passwords = listCreate();
    u->patterns = listCreate();
    listSetFreeMethod(u->passwords, (void (*)(void*))sdsfree);
    listSetFreeMethod(u->patterns, (void (*)(void*))sdsfree);
    dictAdd(Users, sdsnew(name), u);
    return u;
}

static void aclAllowAll(user *u) {
    memset(u->commands, 0xff, sizeof(u->commands));
    u->flags |= USER_ALLCMD;
}

static void aclDenyAll(user *u) {
    memset(u->commands, 0, sizeof(u->commands));
    u->flags &= ~USER_ALLCMD;
}

static int aclCommandBit(user *u, unsigned id) {
    return (u->commands[id/64] >> (id%64)) & 1;
}

static void aclSetCommandBit(user *u, unsigned id, int on) {
    uint64_t mask = 1ULL << (id%64);
    if (on) u->commands[id/64] |= mask;
    else    u->commands[id/64] &= ~mask;
    u->flags &= ~USER_ALLCMD;
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

/* Apply one rule. op is a single token such as "on", "+@read", "~cache:*"
 * or ">secret". Returns C_ERR for a rule this build does not understand. */
int ACLSetUser(user *u, const char *op) {
    if (!strcasecmp(op,"on")) { u->flags |= USER_ENABLED; return C_OK; }
    if (!strcasecmp(op,"off")) { u->flags &= ~USER_ENABLED; return C_OK; }
    if (!strcasecmp(op,"nopass")) { u->flags |= USER_NOPASS; return C_OK; }
    if (!strcasecmp(op,"resetpass")) {
        listEmpty(u->passwords);
        u->flags &= ~USER_NOPASS;
        return C_OK;
    }
    if (!strcasecmp(op,"allkeys") || !strcmp(op,"~*")) {
        listEmpty(u->patterns);
        u->flags |= USER_ALLKEYS;
        return C_OK;
    }
    if (!strcasecmp(op,"resetkeys")) {
        listEmpty(u->patterns);
        u->flags &= ~USER_ALLKEYS;
        return C_OK;
    }
    if (!strcasecmp(op,"allcommands") || !strcasecmp(op,"+@all")) {
        aclAllowAll(u); return C_OK;
    }
    if (!strcasecmp(op,"nocommands") || !strcasecmp(op,"-@all")) {
        aclDenyAll(u); return C_OK;
    }
    if (!strcasecmp(op,"reset")) {
        aclDenyAll(u);
        listEmpty(u->passwords);
        listEmpty(u->patterns);
        u->flags = 0;
        return C_OK;
    }
    if (op[0] == '>' || op[0] == '#') {
        sds hash = op[0] == '>' ? aclHashPassword(op+1, strlen(op+1))
                                : sdsnew(op+1);
        listAddNodeTail(u->passwords, hash);
        u->flags &= ~USER_NOPASS;
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
            if (match) { aclSetCommandBit(u, cmd->id, on); known = 1; }
        }
        dictReleaseIterator(di);
        return known < 0 ? C_ERR : C_OK;
    }
    if (op[0] == '+' || op[0] == '-') {
        void *found = raxFind(CommandIds, (unsigned char*)(op+1), strlen(op+1));
        int id = (found == raxNotFound) ? 0 : (int)(size_t)found;
        if (id == 0) return C_ERR;
        aclSetCommandBit(u, (unsigned)(id-1), op[0] == '+');
        return C_OK;
    }
    if (op[0] == '~') {
        listAddNodeTail(u->patterns, sdsnew(op+1));
        u->flags &= ~USER_ALLKEYS;
        return C_OK;
    }
    return C_ERR;
}

/* The default user mirrors the historical requirepass behaviour: everything
 * is allowed, and a configured password is the only way in. */
void ACLInitDefaultUser(void) {
    user *u = ACLCreateUser("default");
    aclAllowAll(u);
    u->flags |= USER_ENABLED | USER_ALLKEYS | USER_NOPASS;
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

/* Returns C_OK when the command may run. On refusal a reply is written and
 * C_ERR is returned; the caller must not execute the command. */
int ACLCheckCommand(client *c) {
    user *u = c->user;
    int *keys, numkeys, i;
    if (u == NULL) return C_OK;
    if (!(u->flags & USER_ENABLED)) {
        addReplyError(c,"user is disabled");
        return C_ERR;
    }
    /* ACL WHOAMI reports the caller's own identity, so Redis 6 allows it for
     * every authenticated user regardless of the acl command bit. */
    if (c->cmd->proc == aclCommand && c->argc == 2 &&
        !strcasecmp(c->argv[1]->ptr,"whoami")) return C_OK;
    if (!(u->flags & USER_ALLCMD) && !aclCommandBit(u, c->cmd->id)) {
        addReplyErrorFormat(c,"user has no permission to run '%s'", c->cmd->name);
        return C_ERR;
    }
    if (u->flags & USER_ALLKEYS) return C_OK;
    keys = getKeysFromCommand(c->cmd, c->argv, c->argc, &numkeys);
    for (i = 0; i < numkeys; i++) {
        sds key = c->argv[keys[i]]->ptr;
        listIter li;
        listNode *ln;
        int allowed = 0;
        listRewind(u->patterns, &li);
        while ((ln = listNext(&li))) {
            sds pat = ln->value;
            if (stringmatchlen(pat, (int)sdslen(pat), key, (int)sdslen(key), 0))
                allowed = 1;
        }
        if (!allowed) {
            getKeysFreeResult(keys);
            addReplyError(c,"key access denied");
            return C_ERR;
        }
    }
    getKeysFreeResult(keys);
    return C_OK;
}

void authCommand(client *c) {
    user *u;
    const char *username, *password;
    if (c->argc > 3) {
        addReplyError(c,"wrong number of arguments for 'auth' command");
        return;
    }
    if (c->argc == 2) {
        username = "default";
        password = c->argv[1]->ptr;
    } else {
        username = c->argv[1]->ptr;
        password = c->argv[2]->ptr;
    }
    u = ACLGetUser((sds)username);
    if (u == NULL || !(u->flags & USER_ENABLED) ||
        ACLCheckPassword(u, password, strlen(password)) != C_OK) {
        c->authenticated = 0;
        c->user = NULL;
        addReplyError(c,"invalid username-password pair or user is disabled");
        return;
    }
    c->authenticated = 1;
    c->user = u;
    addReply(c, shared.ok);
}

void aclCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"list")) {
        dictIterator *di = dictGetIterator(Users);
        dictEntry *de;
        addReplyMultiBulkLen(c, dictSize(Users));
        while ((de = dictNext(di)) != NULL) {
            user *u = dictGetVal(de);
            sds line = sdscatprintf(sdsempty(), "user %s %s",
                u->name, (u->flags & USER_ENABLED) ? "on" : "off");
            line = sdscat(line, (u->flags & USER_NOPASS) ? " nopass" : "");
            line = sdscat(line, (u->flags & USER_ALLKEYS) ? " ~*" : "");
            line = sdscat(line, (u->flags & USER_ALLCMD) ? " +@all" : "");
            addReplyBulkCBuffer(c, line, sdslen(line));
            sdsfree(line);
        }
        dictReleaseIterator(di);
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
    if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr,"deluser")) {
        if (!strcasecmp(c->argv[2]->ptr,"default")) {
            addReplyError(c,"the default user cannot be removed");
            return;
        }
        addReplyLongLong(c, dictDelete(Users, c->argv[2]->ptr) == DICT_OK);
        return;
    }
    addReplyError(c,"unknown ACL subcommand or wrong number of arguments");
}
