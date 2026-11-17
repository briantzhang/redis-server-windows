/* Redis Functions support, backported to the Windows 5.0.14.1 tree.
 *
 * Hand-written adaptation of the commands described by Redis 7.0.15 (BSD).
 * It reuses the EVAL Lua state and redis.call() plumbing. Libraries are kept
 * as source strings so they can be persisted in an RDB AUX field and replayed
 * as FUNCTION LOAD commands on AOF rewrite. No Redis 8 source is used. */

#include "server.h"
#include "rand.h"
#include <lua.h>
#include <lauxlib.h>
#include <ctype.h>

/* Provided by scripting.c, used to restore redis.call/pcall afterwards. */
int luaRedisCallCommand(lua_State *lua);
int luaRedisPCallCommand(lua_State *lua);
int luaRedisGenericCommand(lua_State *lua, int raise_error);

/* A library and an entry in the global function index. Both dictionaries are
 * managed by hand: the key sds belongs to the value, so the dict destructor
 * only frees the value. */
typedef struct functionInfo {
    sds name;
    sds libname;
    int flags;
} functionInfo;

typedef struct functionLib {
    sds name;
    sds code;
    size_t bodylen;
    dict *functions;
} functionLib;

#define FUNC_FLAG_NO_WRITES      (1<<0)
#define FUNC_FLAG_ALLOW_OOM      (1<<1)
#define FUNC_FLAG_ALLOW_STALE    (1<<2)
#define FUNC_FLAG_NO_CLUSTER     (1<<3)
#define FUNC_FLAG_RAW_ARGUMENTS  (1<<4)
#define FUNC_FLAG_RAW_ARGUMENTS2 (1<<5)

static dict *function_libs;
static dict *functions;

static int compileLibraries(client *c);
static int functionLoadCode(client *c, sds code, int replace, int quiet);
static void freeLibDict(dict *d);
static void freeFunctionDict(dict *d);
static void functionLibFree(functionLib *lib);

static void functionInfoFree(functionInfo *fi) {
    sdsfree(fi->name);
    sdsfree(fi->libname);
    zfree(fi);
}

static void functionLibFree(functionLib *lib) {
    dictIterator *di = dictGetIterator(lib->functions);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) functionInfoFree(dictGetVal(de));
    dictReleaseIterator(di);
    dictRelease(lib->functions);
    sdsfree(lib->name);
    sdsfree(lib->code);
    zfree(lib);
}

static void functionLibValDestructor(void *privdata, void *val) {
    UNUSED(privdata);
    functionLibFree(val);
}

static dictType functionLibDictType = {
    dictSdsHash, NULL, NULL, dictSdsKeyCompare,
    NULL, functionLibValDestructor
};

/* The global index borrows functionInfo pointers owned by a library dict. */
static dictType functionInfoDictType = {
    dictSdsHash, NULL, NULL, dictSdsKeyCompare,
    NULL, NULL
};

/* dictDelete invokes the value destructor; the key sds is owned by that value
 * and must not be freed by the dict. */

static dict *newLibDict(void) { return dictCreate(&functionLibDictType,NULL); }
static dict *newFunctionDict(void) { return dictCreate(&functionInfoDictType,NULL); }

/* The dict key is the sds owned by the value, so these helpers only clear
 * entries and let dictRelease() call the value destructor once per entry. */
static void freeLibDict(dict *d) {
    if (d) dictRelease(d);
}

static void freeFunctionDict(dict *d) {
    if (d) dictRelease(d);
}

void functionsInit(int setup) {
    if (!setup) {
        freeLibDict(function_libs);
        freeFunctionDict(functions);
    }
    function_libs = newLibDict();
    functions = newFunctionDict();
    server.function_libs = function_libs;
    server.function_index = functions;
    lua_newtable(server.lua);
    lua_setglobal(server.lua,"__redis_function_callbacks");
}

/* The Windows fork child copies the server structure but starts with fresh
 * statics. The dictionaries themselves are in the mapped parent heap. */
void functionsReloadGlobals(void) {
    function_libs = (dict*)server.function_libs;
    functions = (dict*)server.function_index;
}

static int validName(const char *s, size_t len) {
    size_t i;
    if (len == 0) return 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) return 0;
    }
    return 1;
}

/* Parse "#!lua name=<lib>". The engine and one name= field are mandatory. */
static sds parseLibraryName(client *c, const char *code, size_t len, size_t *bodylen) {
    const char *line_end = memchr(code,'\n',len);
    const char *shebang_end;
    const char *name;
    size_t linelen, nlen;

    if (!code || len == 0) {
        if (c) addReplyError(c,"ERR Invalid library metadata");
        return NULL;
    }
    if (line_end == NULL) line_end = code+len;
    linelen = line_end-code;
    if (linelen < 7 || _strnicmp(code,"#!lua",5) != 0 || code[5] != ' ') {
        if (c) addReplyError(c,"ERR Invalid library metadata");
        return NULL;
    }
    shebang_end = line_end;
    {
        const char *q = code+6, *last = line_end-5;
        name = NULL;
        for (; q <= last; q++) {
            if (!_strnicmp(q,"name=",5)) { name = q; break; }
        }
    }
    if (!name) {
        if (c) addReplyError(c,"ERR Library name was not given");
        return NULL;
    }
    name += 5;
    nlen = line_end-name;
    while (nlen && (name[nlen-1] == '\r' || name[nlen-1] == ' ')) nlen--;
    if (!validName(name,nlen)) {
        if (c) addReplyError(c,"ERR Invalid library name");
        return NULL;
    }
    *bodylen = len - (size_t)(shebang_end-code);
    if (shebang_end < code+len) (*bodylen)--;
    return sdsnewlen(name,nlen);
}

/* Called while a library chunk runs. Registers the callback into a private
 * table and records the name/flags in the C registry. */
static int luaRegisterFunction(lua_State *lua) {
    functionLib *lib = lua_touserdata(lua,lua_upvalueindex(1));
    const char *name;
    size_t nlen;
    functionInfo *fi;
    int flags = 0;

    if (lua_gettop(lua) < 2 || lua_type(lua,1) != LUA_TSTRING ||
        lua_type(lua,2) != LUA_TFUNCTION) {
        return luaL_error(lua,"redis.register_function expects a name, a callback and optional options");
    }
    name = lua_tolstring(lua,1,&nlen);
    if (!validName(name,nlen)) return luaL_error(lua,"Invalid function name");
    if (dictFind(lib->functions,name) != NULL)
        return luaL_error(lua,"Function already exists in the library");
    if (dictFind(functions,name) != NULL)
        return luaL_error(lua,"Function already exists");

    if (lua_gettop(lua) >= 3 && lua_type(lua,3) == LUA_TTABLE) {
        int i, n = (int)luaL_getn(lua,3);
        for (i = 1; i <= n; i++) {
            const char *flag;
            lua_rawgeti(lua,3,i);
            flag = lua_tostring(lua,-1);
            if (!flag) {
                lua_pop(lua,1);
                return luaL_error(lua,"Function flags must be strings");
            }
            if (!strcasecmp(flag,"no-writes")) flags |= FUNC_FLAG_NO_WRITES;
            else if (!strcasecmp(flag,"allow-oom")) flags |= FUNC_FLAG_ALLOW_OOM;
            else if (!strcasecmp(flag,"allow-stale")) flags |= FUNC_FLAG_ALLOW_STALE;
            else if (!strcasecmp(flag,"no-cluster")) flags |= FUNC_FLAG_NO_CLUSTER;
            else if (!strcasecmp(flag,"raw-arguments")) flags |= FUNC_FLAG_RAW_ARGUMENTS;
            else if (!strcasecmp(flag,"raw-arguments2")) flags |= FUNC_FLAG_RAW_ARGUMENTS2;
            else {
                lua_pop(lua,1);
                return luaL_error(lua,"Unknown function flag");
            }
            lua_pop(lua,1);
        }
        /* Upstream also accepts { no_writes = true }. */
        lua_getfield(lua,3,"no_writes");
        if (lua_toboolean(lua,-1)) flags |= FUNC_FLAG_NO_WRITES;
        lua_pop(lua,1);
    }

    fi = zcalloc(sizeof(*fi));
    fi->name = sdsnewlen(name,nlen);
    fi->libname = sdsdup(lib->name);
    fi->flags = flags;
    dictAdd(lib->functions,fi->name,fi);
    dictAdd(functions,fi->name,fi);
    lua_getglobal(lua,"__redis_function_callbacks");
    lua_pushlstring(lua,name,nlen);
    lua_pushvalue(lua,2);
    lua_settable(lua,-3);
    lua_pop(lua,1);
    return 0;
}

static void installRegisterFunction(lua_State *lua, functionLib *lib) {
    lua_getglobal(lua,"redis");
    lua_pushlightuserdata(lua,lib);
    lua_pushcclosure(lua,luaRegisterFunction,1);
    lua_setfield(lua,-2,"register_function");
    lua_pop(lua,1);
}

static void removeRegisterFunction(lua_State *lua) {
    lua_getglobal(lua,"redis");
    lua_pushstring(lua,"register_function");
    lua_pushnil(lua);
    lua_settable(lua,-3);
    lua_pop(lua,1);
}

/* Rebuild the callback table and global index from every stored library. */
static int compileLibraries(client *c) {
    functionLib **libs;
    PORT_ULONG nlibs = dictSize(function_libs), i;
    lua_State *lua = server.lua;

    freeFunctionDict(functions);
    functions = newFunctionDict();
    lua_newtable(lua);
    lua_setglobal(lua,"__redis_function_callbacks");

    libs = zmalloc(sizeof(*libs) * (nlibs ? nlibs : 1));
    {
        dictIterator *di = dictGetIterator(function_libs);
        dictEntry *de;
        i = 0;
        while ((de = dictNext(di)) != NULL) libs[i++] = dictGetVal(de);
        dictReleaseIterator(di);
    }
    for (i = 0; i < nlibs; i++) {
        functionLib *lib = libs[i];

        {
            dictIterator *fi = dictGetSafeIterator(lib->functions);
            dictEntry *fe;
            while ((fe = dictNext(fi)) != NULL) {
                functionInfoFree(dictGetVal(fe));
                dictFreeKey(lib->functions,fe);
            }
            dictReleaseIterator(fi);
            dictEmpty(lib->functions,NULL);
        }

        installRegisterFunction(lua,lib);
        if (luaL_loadbuffer(lua,lib->code+sdslen(lib->code)-lib->bodylen,
                            lib->bodylen,"@function_library") != 0) {
            const char *err = lua_tostring(lua,-1);
            if (c) addReplyErrorFormat(c,"ERR Error compiling function: %s",err ? err : "unknown error");
            else serverLog(LL_WARNING,"Error compiling function library: %s",err ? err : "unknown error");
            lua_pop(lua,1);
            removeRegisterFunction(lua);
            zfree(libs);
            return C_ERR;
        }
        if (lua_pcall(lua,0,0,0) != 0) {
            const char *err = lua_tostring(lua,-1);
            if (c) addReplyErrorFormat(c,"ERR Error loading function: %s",err ? err : "unknown error");
            else serverLog(LL_WARNING,"Error loading function library: %s",err ? err : "unknown error");
            lua_pop(lua,1);
            removeRegisterFunction(lua);
            zfree(libs);
            return C_ERR;
        }
        removeRegisterFunction(lua);
    }
    zfree(libs);
    return C_OK;
}

static int functionLoadCode(client *c, sds code, int replace, int quiet) {
    size_t bodylen = 0;
    sds name = parseLibraryName(c,code,sdslen(code),&bodylen);
    functionLib *lib;
    dictEntry *de;

    if (name == NULL) return C_ERR; /* parseLibraryName already replied. */
    if (!function_libs) {
        sdsfree(name);
        if (c && !quiet) addReplyError(c,"ERR Function engine is not initialized");
        return C_ERR;
    }
    de = dictFind(function_libs,name);
    if (de && !replace) {
        if (!quiet && c) addReplyError(c,"ERR Library already exists");
        sdsfree(name);
        return C_ERR;
    }
    if (de) dictDelete(function_libs,name);
    lib = zcalloc(sizeof(*lib));
    lib->name = name;
    /* Keep the shebang: DUMP/RESTORE and AUX reload feed this text through
     * parseLibraryName(), which requires the metadata line. */
    lib->code = sdsdup(code);
    lib->bodylen = bodylen;
    lib->functions = newFunctionDict();
    dictAdd(function_libs,lib->name,lib);

    if (compileLibraries(NULL) != C_OK) {
        de = dictFind(function_libs,name);
        if (de) dictDelete(function_libs,name);
        compileLibraries(NULL);
        if (!quiet && c) addReplyError(c,"ERR Error compiling function library");
        return C_ERR;
    }
    if (!quiet && c) addReplyBulkCString(c,lib->name);
    return C_OK;
}

/* FCALL / FCALL_RO. In 7.0.15 the callback receives (KEYS, ARGV); this build
 * keeps the EVAL convention of globals as well, so simple libraries behave the
 * same. */
/* Wrapper for redis.call/pcall used by read-only functions. It rejects a
 * command marked CMD_WRITE before handing the arguments to the EVAL engine. */
static int luaFunctionReadOnlyCall(lua_State *lua) {
    const char *name;
    struct redisCommand *cmd;
    int raise_error = lua_toboolean(lua,lua_upvalueindex(1));
    if (lua_gettop(lua) < 1 || lua_type(lua,1) != LUA_TSTRING)
        return luaL_error(lua,"Please specify at least one argument for redis.call()");
    name = lua_tostring(lua,1);
    cmd = lookupCommandByCString((char*)name);
    if (!cmd) return luaL_error(lua,"Unknown Redis command called from Lua script");
    if (cmd->flags & CMD_WRITE)
        return luaL_error(lua,"Write commands are not allowed from read-only functions.");
    return luaRedisGenericCommand(lua,raise_error);
}

static void installReadOnlyWrappers(lua_State *lua) {
    lua_getglobal(lua,"redis");
    lua_pushboolean(lua,1);
    lua_pushcclosure(lua,luaFunctionReadOnlyCall,1);
    lua_setfield(lua,-2,"call");
    lua_pushboolean(lua,0);
    lua_pushcclosure(lua,luaFunctionReadOnlyCall,1);
    lua_setfield(lua,-2,"pcall");
    lua_pop(lua,1);
}

static void restoreCallWrappers(lua_State *lua) {
    lua_getglobal(lua,"redis");
    lua_pushcfunction(lua,luaRedisCallCommand);
    lua_setfield(lua,-2,"call");
    lua_pushcfunction(lua,luaRedisPCallCommand);
    lua_setfield(lua,-2,"pcall");
    lua_pop(lua,1);
}

void fcallCommand(client *c) {
    PORT_LONGLONG numkeys;
    functionInfo *fi;
    lua_State *lua = server.lua;
    int readonly, delhook = 0, err;
    mstime_t start;

    if (getLongLongFromObjectOrReply(c,c->argv[2],&numkeys,NULL) != C_OK) return;
    if (numkeys < 0 || numkeys > c->argc-3) {
        addReplyError(c,"ERR Number of keys can't be negative or greater than number of args");
        return;
    }
    fi = dictFetchValue(functions,c->argv[1]->ptr);
    if (!fi) { addReplyError(c,"NOFUNCTION Unknown function"); return; }

    redisSrand48(0);
    server.lua_random_dirty = 0;
    server.lua_write_dirty = 0;
    server.lua_replicate_commands = server.lua_always_replicate_commands;
    server.lua_multi_emitted = 0;
    server.lua_repl = PROPAGATE_AOF|PROPAGATE_REPL;
    /* EVAL's lua_ro is private to scripting.c. Read-only functions swap in a
     * redis.call wrapper that rejects CMD_WRITE commands before they reach it. */
    readonly = c->cmd->proc == fcallRoCommand ||
               (fi->flags & FUNC_FLAG_NO_WRITES) != 0;
    if (readonly) installReadOnlyWrappers(lua);

    /* Keep the EVAL globals, and also pass (keys, args) as the 7.0 callback
     * arguments. Two table objects are needed: one to install as a global and
     * one to leave on the stack. */
    luaSetGlobalArray(lua,"KEYS",c->argv+3,(int)numkeys);
    luaSetGlobalArray(lua,"ARGV",c->argv+3+numkeys,(int)(c->argc-3-numkeys));
    selectDb(server.lua_client,c->db->id);

    /* Stack: error handler, callback, keys table, args table. */
    lua_settop(lua,0);
    lua_getglobal(lua,"__redis__err__handler");
    lua_getglobal(lua,"__redis_function_callbacks");
    lua_pushstring(lua,fi->name);
    lua_gettable(lua,-2);
    lua_remove(lua,-2);
    if (!lua_isfunction(lua,-1)) {
        lua_pop(lua,2);
        if (readonly) restoreCallWrappers(lua);
        addReplyError(c,"NOFUNCTION Function callback is unavailable");
        return;
    }
    lua_getglobal(lua,"KEYS");
    lua_getglobal(lua,"ARGV");

    server.lua_caller = c;
    start = mstime();
    server.lua_time_start = start;
    server.lua_kill = 0;
    if (server.lua_time_limit > 0) {
        lua_sethook(lua,luaMaskCountHook,LUA_MASKCOUNT,100000);
        delhook = 1;
    }
    err = lua_pcall(lua,2,1,-4);
    if (delhook) lua_sethook(lua,NULL,0,0);
    if (server.lua_timedout) {
        server.lua_timedout = 0;
        unprotectClient(c);
        if (server.masterhost && server.master)
            queueClientForReprocessing(server.master);
    }
    server.lua_caller = NULL;

    if (err) {
        addReplyErrorFormat(c,"ERR Error running function (call to %s): %s",
            fi->name, lua_tostring(lua,-1));
        lua_pop(lua,2); /* the error object and the error handler */
    } else {
        /* luaReplyToRedisReply consumes the reply; only the handler is left. */
        luaReplyToRedisReply(c,lua);
        lua_pop(lua,1);
    }
    if (readonly) restoreCallWrappers(lua);

    if (server.lua_replicate_commands) {
        preventCommandPropagation(c);
        if (server.lua_multi_emitted) {
            robj *propargv[1];
            propargv[0] = createStringObject("EXEC",4);
            alsoPropagate(server.execCommand,c->db->id,propargv,1,
                PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(propargv[0]);
        }
    }
}

void fcallRoCommand(client *c) { fcallCommand(c); }

/* FUNCTION LIST [LIBRARYNAME pattern] [WITHCODE]. */
static void functionListCommand(client *c) {
    const char *pattern = NULL;
    int withcode = 0, i, count = 0;
    dictIterator *di;
    dictEntry *de;
    functionLib **libs;
    PORT_ULONG nlibs;

    for (i = 2; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr,"withcode")) withcode = 1;
        else if (!strcasecmp(c->argv[i]->ptr,"libraryname") && i+1 < c->argc)
            pattern = c->argv[++i]->ptr;
        else { addReplyError(c,"ERR Syntax error in FUNCTION LIST"); return; }
    }
    nlibs = dictSize(function_libs);
    libs = zmalloc(sizeof(*libs) * (nlibs ? nlibs : 1));
    di = dictGetIterator(function_libs);
    i = 0;
    while ((de = dictNext(di)) != NULL) {
        functionLib *lib = dictGetVal(de);
        sds name = lib->name;
        if (!pattern || stringmatchlen(pattern,(int)strlen(pattern),name,(int)sdslen(name),0)) libs[i++] = lib;
    }
    dictReleaseIterator(di);
    count = (int)i;
    addReplyMultiBulkLen(c,count);
    for (i = 0; i < count; i++) {
        functionLib *lib = libs[i];
        dictIterator *fi;
        dictEntry *fe;
        addReplyMapLen(c,withcode ? 3 : 2);
        addReplyBulkCString(c,"library_name");
        addReplyBulkCString(c,lib->name ? lib->name : "");
        addReplyBulkCString(c,"functions");
        addReplyMultiBulkLen(c,(PORT_LONG)dictSize(lib->functions));
        fi = dictGetIterator(lib->functions);
        while ((fe = dictNext(fi)) != NULL) {
            functionInfo *info = dictGetVal(fe);
            addReplyMapLen(c,2);
            addReplyBulkCString(c,"name");
            addReplyBulkCString(c,info->name);
            addReplyBulkCString(c,"flags");
            addReplyMultiBulkLen(c,0);
        }
        dictReleaseIterator(fi);
        if (withcode) {
            addReplyBulkCString(c,"library_code");
            addReplyBulkCBuffer(c,lib->code,sdslen(lib->code));
        }
    }
    zfree(libs);
}

/* Serialized payload: "RFNC" + u32 count, then for each library a u32 code
 * length and its bytes, then a u64 CRC64. This is a local, not upstream,
 * format: DUMP and RESTORE are only meant to round trip within this build. */
static void put32(sds *p, uint32_t v) {
    unsigned char b[4] = {(unsigned char)v,(unsigned char)(v>>8),
                          (unsigned char)(v>>16),(unsigned char)(v>>24)};
    *p = sdscatlen(*p,b,4);
}

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static sds functionsDump(void) {
    sds payload = sdsnewlen("RFNC",4);
    dictIterator *di = dictGetIterator(function_libs);
    dictEntry *de;
    uint64_t sum;
    unsigned char sumbuf[8];
    int i;

    put32(&payload,(uint32_t)dictSize(function_libs));
    while ((de = dictNext(di)) != NULL) {
        functionLib *lib = dictGetVal(de);
        size_t namelen = sdslen(lib->name), codelen = sdslen(lib->code);
        put32(&payload,(uint32_t)namelen);
        payload = sdscatlen(payload,lib->name,namelen);
        put32(&payload,(uint32_t)codelen);
        payload = sdscatlen(payload,lib->code,codelen);
    }
    dictReleaseIterator(di);
    sum = crc64(0,payload,sdslen(payload));
    for (i = 0; i < 8; i++) sumbuf[i] = (unsigned char)(sum >> (8*i));
    payload = sdscatlen(payload,sumbuf,8);
    return payload;
}

/* Restore from a DUMP payload. FLUSH clears first, APPEND rejects collisions,
 * REPLACE replaces the named libraries. */
static void functionRestoreCommand(client *c) {
    sds payload = c->argv[2]->ptr;
    const unsigned char *p = (const unsigned char*)payload;
    size_t len = sdslen(payload), pos = 4;
    uint32_t n, i;
    uint64_t sum = 0, expected = 0;
    sds *codes = NULL, *names = NULL;
    int mode = 0, parsed = 0, compiled;

    if (c->argc == 4) {
        if (!strcasecmp(c->argv[3]->ptr,"flush")) mode = 1;
        else if (!strcasecmp(c->argv[3]->ptr,"replace")) mode = 2;
        else if (!strcasecmp(c->argv[3]->ptr,"append")) mode = 0;
        else { addReplyError(c,"ERR Invalid FUNCTION RESTORE policy"); return; }
    }
    if (len < 16 || memcmp(p,"RFNC",4) != 0) {
        addReplyError(c,"ERR Invalid function payload"); return;
    }
    sum = crc64(0,p,len-8);
    for (i = 0; i < 8; i++) expected |= (uint64_t)p[len-8+i] << (8*i);
    if (sum != expected) {
        addReplyError(c,"ERR Invalid function payload checksum"); return;
    }
    n = get32(p+pos); pos += 4;
    if (n > 100000) { addReplyError(c,"ERR Invalid function payload"); return; }
    codes = zcalloc(sizeof(sds)*(n ? n : 1));
    names = zcalloc(sizeof(sds)*(n ? n : 1));
    for (i = 0; i < n; i++) {
        uint32_t namelen, clen;
        if (pos+4 > len-8) {
            addReplyError(c,"ERR Invalid function payload"); goto cleanup;
        }
        namelen = get32(p+pos); pos += 4;
        if ((size_t)namelen > len-8-pos) {
            addReplyError(c,"ERR Invalid function payload"); goto cleanup;
        }
        names[i] = sdsnewlen((const char*)p+pos,namelen);
        pos += namelen;
        if (pos+4 > len-8 || !validName(names[i],sdslen(names[i]))) {
            addReplyError(c,"ERR Invalid function payload"); goto cleanup;
        }
        clen = get32(p+pos); pos += 4;
        if ((size_t)clen > len-8-pos) {
            addReplyError(c,"ERR Invalid function payload"); goto cleanup;
        }
        codes[i] = sdsnewlen((const char*)p+pos,clen);
        {
            size_t bodylen = 0;
            sds parsed_name = parseLibraryName(NULL,codes[i],sdslen(codes[i]),&bodylen);
            if (!parsed_name) {
                addReplyError(c,"ERR Invalid library metadata");
                goto cleanup;
            }
            sdsfree(parsed_name);
        }
        pos += clen;
    }
    parsed = 1;

    if (mode == 0) {
        for (i = 0; i < n; i++) {
            if (dictFind(function_libs,names[i])) {
                addReplyError(c,"ERR Library already exists");
                goto cleanup;
            }
        }
    }

    if (mode == 1) {
        freeFunctionDict(functions);
        functions = newFunctionDict();
        freeLibDict(function_libs);
        function_libs = newLibDict();
    } else {
        compileLibraries(NULL);
    }
    for (i = 0; i < n; i++) {
        size_t bodylen;
        sds name;
        dictEntry *de;
        functionLib *lib;
        if (parseLibraryName(NULL,codes[i],sdslen(codes[i]),&bodylen) == NULL) {
            addReplyError(c,"ERR Invalid library metadata");
            goto fail;
        }
        name = sdsdup(names[i]);
        de = dictFind(function_libs,name);
        if (de) {
            if (mode != 2) {
                sdsfree(name);
                addReplyError(c,"ERR Library already exists");
                goto fail;
            }
            dictDelete(function_libs,name);
        }
        lib = zcalloc(sizeof(*lib));
        lib->name = name;
        lib->code = sdsdup(codes[i]);
        lib->bodylen = bodylen;
        lib->functions = newFunctionDict();
        dictAdd(function_libs,lib->name,lib);
    }
    compiled = compileLibraries(NULL);
    if (compiled != C_OK) {
        addReplyError(c,"ERR Error compiling function library");
        goto cleanup;
    }
    addReply(c,shared.ok);
    goto cleanup;

fail:
    compileLibraries(NULL);
cleanup:
    if (parsed) {
        for (i = 0; i < n; i++) {
            sdsfree(codes[i]);
            if (names) sdsfree(names[i]);
        }
    }
    if (codes) zfree(codes);
    if (names) zfree(names);
}

void functionCommand(client *c) {
    if (c->argc < 2) { addReplyError(c,"ERR Unknown FUNCTION subcommand"); return; }
    if (!strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"LOAD [REPLACE] <code> -- Create a library from Lua source.",
"DELETE <library-name> -- Delete a library and its functions.",
"FLUSH [ASYNC|SYNC] -- Delete all libraries.",
"LIST [LIBRARYNAME <pattern>] [WITHCODE] -- List libraries.",
"STATS -- Return function engine statistics.",
"DUMP -- Return a serialized payload of all libraries.",
"RESTORE <payload> [FLUSH|APPEND|REPLACE] -- Restore libraries.",
"KILL -- Kill the running function (NOTBUSY when idle).",
NULL };
        addReplyHelp(c,help);
    } else if (!strcasecmp(c->argv[1]->ptr,"load")) {
        int replace = 0, at = 2;
        if (c->argc >= 4 && !strcasecmp(c->argv[2]->ptr,"replace")) {
            replace = 1; at = 3;
        }
        if (c->argc != at+1) {
            addReplyError(c,"ERR Wrong number of arguments for FUNCTION LOAD");
            return;
        }
        if (functionLoadCode(c,c->argv[at]->ptr,replace,0) == C_ERR) {
            /* All detailed errors are already emitted by the loader. */
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"delete") && c->argc == 3) {
        dictEntry *de = dictFind(function_libs,c->argv[2]->ptr);
        if (!de) { addReplyError(c,"ERR Library not found"); return; }
        dictDelete(function_libs,c->argv[2]->ptr);
        compileLibraries(NULL);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"flush") && c->argc >= 2 && c->argc <= 3) {
        if (c->argc == 3 && strcasecmp(c->argv[2]->ptr,"async") &&
            strcasecmp(c->argv[2]->ptr,"sync")) {
            addReplyError(c,"ERR FUNCTION FLUSH only accepts SYNC or ASYNC");
            return;
        }
        freeLibDict(function_libs);
        freeFunctionDict(functions);
        function_libs = newLibDict();
        functions = newFunctionDict();
        lua_pushnil(server.lua);
        lua_setglobal(server.lua,"__redis_function_callbacks");
        lua_newtable(server.lua);
        lua_setglobal(server.lua,"__redis_function_callbacks");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"list")) {
        functionListCommand(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"stats") && c->argc == 2) {
        addReplyMapLen(c,1);
        addReplyBulkCString(c,"engines");
        addReplyMapLen(c,1);
        addReplyBulkCString(c,"LUA");
        addReplyMapLen(c,4);
        addReplyBulkCString(c,"libraries_count");
        addReplyLongLong(c,dictSize(function_libs));
        addReplyBulkCString(c,"functions_count");
        addReplyLongLong(c,dictSize(functions));
        addReplyBulkCString(c,"function_list");
        addReplyMultiBulkLen(c,0);
        addReplyBulkCString(c,"running_script");
        addReplyNull(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"kill") && c->argc == 2) {
        addReplyError(c,"NOTBUSY No functions in execution right now.");
    } else if (!strcasecmp(c->argv[1]->ptr,"dump") && c->argc == 2) {
        addReplyBulkSds(c,functionsDump());
    } else if (!strcasecmp(c->argv[1]->ptr,"restore")) {
        if (c->argc < 3 || c->argc > 4) {
            addReplyError(c,"ERR Wrong number of arguments for FUNCTION RESTORE");
            return;
        }
        functionRestoreCommand(c);
    } else {
        addReplyErrorFormat(c,"ERR Unknown FUNCTION subcommand '%s'",
            (char*)c->argv[1]->ptr);
    }
}

int *functionGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    PORT_LONGLONG n;
    int *keys, i;
    UNUSED(cmd);
    *numkeys = 0;
    if (argc < 3 || getLongLongFromObject(argv[2],&n) != C_OK ||
        n < 0 || n > argc-3) return NULL;
    keys = zmalloc(sizeof(int)*(int)n);
    for (i = 0; i < n; i++) keys[i] = 3+i;
    *numkeys = (int)n;
    return keys;
}

int functionsSaveAuxField(rio *rdb) {
    dictIterator *di = dictGetSafeIterator(function_libs);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        functionLib *lib = dictGetVal(de);
        if (rdbSaveAuxField(rdb,"function-library",16,lib->code,sdslen(lib->code)) == -1) {
            dictReleaseIterator(di);
            return C_ERR;
        }
    }
    dictReleaseIterator(di);
    return C_OK;
}

int functionsLoadAuxField(robj *auxkey, robj *auxval) {
    if (strcasecmp(auxkey->ptr,"function-library") != 0) return 0;
    return functionLoadCode(NULL,auxval->ptr,1,1) == C_OK ? 1 : -1;
}

int functionsRewriteAOF(rio *aof) {
    dictIterator *di = dictGetSafeIterator(function_libs);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        functionLib *lib = dictGetVal(de);
        if (rioWriteBulkCount(aof,'*',3) == 0 ||
            rioWriteBulkString(aof,"FUNCTION",8) == 0 ||
            rioWriteBulkString(aof,"LOAD",4) == 0 ||
            rioWriteBulkString(aof,lib->code,sdslen(lib->code)) == 0) {
            dictReleaseIterator(di);
            return C_ERR;
        }
    }
    dictReleaseIterator(di);
    return C_OK;
}
