/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

static int checkStringLength(client *c, PORT_LONGLONG size) {
    if (size > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)     /* Set if key not exists. */
#define OBJ_SET_XX (1<<1)     /* Set if key exists. */
#define OBJ_SET_EX (1<<2)
#define OBJ_SET_PX (1<<3)
#define OBJ_SET_KEEPTTL (1<<4)
#define OBJ_SET_GET (1<<5)
#define OBJ_SET_EXAT (1<<6)
#define OBJ_SET_PXAT (1<<7)
#define OBJ_SET_PERSIST (1<<8)

int getGenericCommand(client *c);

/* Backported expiration semantics from Redis 6.2/7.0. Always return an
 * absolute millisecond deadline, checking overflow before multiplication. */
static int stringExpireTime(client *c, robj *expire, int flags, int unit,
                            PORT_LONGLONG *when) {
    PORT_LONGLONG now = mstime();
    if (getLongLongFromObjectOrReply(c,expire,when,NULL) != C_OK) return C_ERR;
    if (*when <= 0 || (unit == UNIT_SECONDS && *when > LLONG_MAX/1000))
        goto invalid;
    if (unit == UNIT_SECONDS) *when *= 1000;
    if (!(flags & (OBJ_SET_EXAT|OBJ_SET_PXAT))) {
        if (*when > LLONG_MAX-now) goto invalid;
        *when += now;
    }
    return C_OK;
invalid:
    addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
    return C_ERR;
}

void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    PORT_LONGLONG when = -1;
    if (expire && stringExpireTime(c,expire,flags,unit,&when) != C_OK) return;
    if ((flags & OBJ_SET_GET) && getGenericCommand(c) == C_ERR) return;

    int found = 0;
    if (flags & (OBJ_SET_NX|OBJ_SET_XX|OBJ_SET_KEEPTTL))
        found = lookupKeyWrite(c->db,key) != NULL;
    if (((flags & OBJ_SET_NX) && found) || ((flags & OBJ_SET_XX) && !found)) {
        if (!(flags & OBJ_SET_GET))
            addReply(c,abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
    if ((flags & OBJ_SET_KEEPTTL) && found) when = getExpire(c->db,key);
    setKeyWithFlags(c->db,key,val,flags & OBJ_SET_KEEPTTL);
    if (expire) setExpire(c,c->db,key,when);
    server.dirty++;
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id);
    if (!(flags & OBJ_SET_GET)) addReply(c,ok_reply ? ok_reply : shared.ok);

    /* Leave the common SET hot path allocation-free, as in the base port. */
    if (!expire && !(flags & (OBJ_SET_GET|OBJ_SET_KEEPTTL))) return;

    /* Record the actual result rather than GET/NX/XX or relative time. The
     * legacy AOF writer expands PXAT to SET plus PEXPIREAT. Command names
     * must be the shared objects: the AOF path compares them by pointer. */
    if (when != -1) {
        robj *deadline = createStringObjectFromLongLong(when);
        rewriteClientCommandVector(c,5,shared.set,key,val,shared.pxat,deadline);
        decrRefCount(deadline);
    } else {
        rewriteClientCommandVector(c,3,shared.set,key,val);
    }
}

/* SET key value [NX|XX] [GET] [EX|PX|EXAT|PXAT time|KEEPTTL] */
void setCommand(client *c) {
    int flags = OBJ_SET_NO_FLAGS, unit = UNIT_SECONDS;
    robj *expire = NULL;
    for (int j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = j+1 < c->argc ? c->argv[j+1] : NULL;
        if (!strcasecmp(a,"nx") && !(flags & OBJ_SET_XX)) {
            flags |= OBJ_SET_NX;
        } else if (!strcasecmp(a,"xx") && !(flags & OBJ_SET_NX)) {
            flags |= OBJ_SET_XX;
        } else if (!strcasecmp(a,"get")) {
            flags |= OBJ_SET_GET;
        } else if (!strcasecmp(a,"keepttl") && !expire) {
            flags |= OBJ_SET_KEEPTTL;
        } else if ((!strcasecmp(a,"ex") || !strcasecmp(a,"px") ||
                    !strcasecmp(a,"exat") || !strcasecmp(a,"pxat")) &&
                   next && !expire && !(flags & OBJ_SET_KEEPTTL)) {
            if (!strcasecmp(a,"ex")) flags |= OBJ_SET_EX;
            else if (!strcasecmp(a,"px")) flags |= OBJ_SET_PX;
            else if (!strcasecmp(a,"exat")) flags |= OBJ_SET_EXAT;
            else flags |= OBJ_SET_PXAT;
            unit = (flags & (OBJ_SET_PX|OBJ_SET_PXAT)) ?
                   UNIT_MILLISECONDS : UNIT_SECONDS;
            expire = next;
            j++;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

int getGenericCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return C_OK;

    if (o->type != OBJ_STRING) {
        addReply(c,shared.wrongtypeerr);
        return C_ERR;
    } else {
        addReplyBulk(c,o);
        return C_OK;
    }
}

void getCommand(client *c) {
    getGenericCommand(c);
}

/* GETDEL and GETEX propagate existing 5.x mutation commands. */
void getdelCommand(client *c) {
    robj *key = c->argv[1];
    robj *o = lookupKeyWriteOrReply(c,key,shared.nullbulk);
    if (o == NULL || checkType(c,o,OBJ_STRING)) return;
    addReplyBulk(c,o);
    serverAssert(dbSyncDelete(c->db,key));
    signalModifiedKey(c->db,key);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
    server.dirty++;
    rewriteClientCommandVector(c,2,shared.del,key);
}

void getexCommand(client *c) {
    int flags = 0, unit = UNIT_SECONDS;
    robj *expire = NULL, *key = c->argv[1];
    PORT_LONGLONG when = 0;
    for (int j = 2; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        if (!strcasecmp(a,"persist") && !expire) {
            flags |= OBJ_SET_PERSIST;
        } else if ((!strcasecmp(a,"ex") || !strcasecmp(a,"px") ||
                    !strcasecmp(a,"exat") || !strcasecmp(a,"pxat")) &&
                   j+1 < c->argc && !expire && !(flags & OBJ_SET_PERSIST)) {
            if (!strcasecmp(a,"ex")) flags |= OBJ_SET_EX;
            else if (!strcasecmp(a,"px")) flags |= OBJ_SET_PX;
            else if (!strcasecmp(a,"exat")) flags |= OBJ_SET_EXAT;
            else flags |= OBJ_SET_PXAT;
            unit = (flags & (OBJ_SET_PX|OBJ_SET_PXAT)) ?
                   UNIT_MILLISECONDS : UNIT_SECONDS;
            expire = c->argv[++j];
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    robj *o = lookupKeyWriteOrReply(c,key,shared.nullbulk);
    if (o == NULL || checkType(c,o,OBJ_STRING)) return;
    if (expire && stringExpireTime(c,expire,flags,unit,&when) != C_OK) return;
    addReplyBulk(c,o);
    if (expire && checkAlreadyExpired(when)) {
        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db,key) :
                                                  dbSyncDelete(c->db,key);
        serverAssert(deleted);
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
        server.dirty++;
        rewriteClientCommandVector(c,2,server.lazyfree_lazy_expire ?
                                   shared.unlink : shared.del,key);
    } else if (expire) {
        setExpire(c,c->db,key,when);
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id);
        server.dirty++;
        robj *deadline = createStringObjectFromLongLong(when);
        rewriteClientCommandVector(c,3,shared.pexpireat,key,deadline);
        decrRefCount(deadline);
    } else if ((flags & OBJ_SET_PERSIST) && removeExpire(c->db,key)) {
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"persist",key,c->db->id);
        server.dirty++;
        rewriteClientCommandVector(c,2,shared.persist,key);
    }
}

void getsetCommand(client *c) {
    if (getGenericCommand(c) == C_ERR) return;
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setKey(c->db,c->argv[1],c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);
    server.dirty++;
}

void setrangeCommand(client *c) {
    robj *o;
    PORT_LONG offset;
    sds value = c->argv[3]->ptr;

    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;

    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
        dbAdd(c->db,c->argv[1],o);
    } else {
        size_t olen;

        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* Return existing string length when setting nothing */
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    if (sdslen(value) > 0) {
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        memcpy((char*)o->ptr+offset,value,sdslen(value));
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c,sdslen(o->ptr));
}

void getrangeCommand(client *c) {
    robj *o;
    PORT_LONGLONG start, end;
    char *str, llbuf[32];
    size_t strlen;

    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(PORT_LONG)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes */
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((PORT_ULONGLONG)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        addReply(c,shared.emptybulk);
    } else {
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

void mgetCommand(client *c) {
    int j;

    addReplyMultiBulkLen(c,c->argc-1);
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            if (o->type != OBJ_STRING) {
                addReply(c,shared.nullbulk);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

void msetGenericCommand(client *c, int nx) {
    int j;

    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key alerady exists. */
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }

    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c->db,c->argv[j],c->argv[j+1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

void incrDecrCommand(client *c, PORT_LONGLONG incr) {
    PORT_LONGLONG value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((PORT_LONG)value);
    } else {
        new = createStringObjectFromLongLongForValue(value);
        if (o) {
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            dbAdd(c->db,c->argv[1],new);
        }
    }
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}

void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

void incrbyCommand(client *c) {
    PORT_LONGLONG incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

void decrbyCommand(client *c) {
    PORT_LONGLONG incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}

void incrbyfloatCommand(client *c) {
    PORT_LONGDOUBLE incr, value;
    robj *o, *new, *aux;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2];
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        if (checkStringLength(c,totlen) != C_OK)
            return;

        /* Append the value */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
    addReplyLongLong(c,totlen);
}

void strlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}

/* LCS key1 key2 [LEN] [IDX] [MINMATCHLEN <len>] [WITHMATCHLEN]
 *
 * Longest common subsequence of the two string values. With IDX the reply
 * is the flat RESP2 form of the map {"matches": [...], "len": N}; each match
 * is [[start,end],[start,end]] optionally followed by its length. */
void lcsCommand(client *c) {
    uint32_t i, j;
    PORT_LONGLONG minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;
    uint32_t *lcs = NULL;
    sds result = NULL;

    obja = lookupKeyRead(c->db,c->argv[1]);
    objb = lookupKeyRead(c->db,c->argv[2]);
    if ((obja && obja->type != OBJ_STRING) ||
        (objb && objb->type != OBJ_STRING))
    {
        addReplyError(c,"The specified keys must contain string values");
        /* Don't cleanup the objects, we need to do that only after calling
         * getDecodedObject(). */
        obja = NULL;
        objb = NULL;
        goto cleanup;
    }
    obja = obja ? getDecodedObject(obja) : createStringObject("",0);
    objb = objb ? getDecodedObject(objb) : createStringObject("",0);
    a = obja->ptr;
    b = objb->ptr;

    for (j = 3; j < (uint32_t)c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1) - j;

        if (!strcasecmp(opt,"IDX")) {
            getidx = 1;
        } else if (!strcasecmp(opt,"LEN")) {
            getlen = 1;
        } else if (!strcasecmp(opt,"WITHMATCHLEN")) {
            withmatchlen = 1;
        } else if (!strcasecmp(opt,"MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&minmatchlen,NULL)
                != C_OK) goto cleanup;
            if (minmatchlen < 0) minmatchlen = 0;
            j++;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. */
    if (getlen && getidx) {
        addReplyError(c,
            "If you want both the length and indexes, please just use IDX.");
        goto cleanup;
    }

    /* Detect string truncation or later overflows. */
    if (sdslen(a) >= UINT32_MAX-1 || sdslen(b) >= UINT32_MAX-1) {
        addReplyError(c, "String too long for LCS");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. */
    uint32_t alen = sdslen(a);
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*i]. */
    #define LCS(A,B) lcs[(B)+((A)*(blen+1))]

    /* Try to allocate the LCS table, and abort on overflow or insufficient
     * memory. */
    unsigned long long lcssize = (unsigned long long)(alen+1)*(blen+1);
    unsigned long long lcsalloc = lcssize * sizeof(uint32_t);
    if (lcsalloc < SIZE_MAX && lcssize != 0 &&
        lcsalloc / lcssize == sizeof(uint32_t))
    {
        if (lcsalloc > (PORT_ULONGLONG)server.proto_max_bulk_len) {
            addReplyError(c,"Insufficient memory, transient memory for LCS "
                            "exceeds proto-max-bulk-len");
            goto cleanup;
        }
        lcs = ztrymalloc((size_t)lcsalloc);
    }
    if (!lcs) {
        addReplyError(c,"Insufficient memory, failed allocating transient "
                        "memory for LCS");
        goto cleanup;
    }

    /* Start building the LCS table. */
    for (uint32_t ti = 0; ti <= alen; ti++) {
        for (uint32_t tj = 0; tj <= blen; tj++) {
            if (ti == 0 || tj == 0) {
                LCS(ti,tj) = 0;
            } else if (a[ti-1] == b[tj-1]) {
                LCS(ti,tj) = LCS(ti-1,tj-1)+1;
            } else {
                uint32_t lcs1 = LCS(ti-1,tj);
                uint32_t lcs2 = LCS(ti,tj-1);
                LCS(ti,tj) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. */
    uint32_t idx = LCS(alen,blen);
    void *arraylenptr = NULL; /* Deferred length of the array for IDX. */
    uint32_t arange_start = alen, /* alen signals that values are not set. */
             arange_end = 0,
             brange_start = 0,
             brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. */
    int computelcs = getidx || !getlen;
    if (computelcs) result = sdsnewlen(SDS_NOINIT,idx);

    /* Start with a deferred array if we have to emit the ranges. */
    uint32_t arraylen = 0;  /* Number of ranges emitted in the array. */
    if (getidx) {
        addReplyMultiBulkLen(c,4);
        addReplyBulkCString(c,"matches");
        arraylenptr = addDeferredMultiBulkLength(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i-1] == b[j-1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. */
            result[idx-1] = a[i-1];

            /* Track the current range. */
            if (arange_start == alen) {
                arange_start = i-1;
                arange_end = i-1;
                brange_start = j-1;
                brange_end = j-1;
            } else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                } else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. */
            if (arange_start == 0 || brange_start == 0) emit_range = 1;
            idx--; i--; j--;
        } else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. */
            uint32_t lcs1 = LCS(i-1,j);
            uint32_t lcs2 = LCS(i,j-1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen) emit_range = 1;
        }

        /* Emit the current range if needed. */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyMultiBulkLen(c,2+withmatchlen);
                    addReplyMultiBulkLen(c,2);
                    addReplyLongLong(c,arange_start);
                    addReplyLongLong(c,arange_end);
                    addReplyMultiBulkLen(c,2);
                    addReplyLongLong(c,brange_start);
                    addReplyLongLong(c,brange_end);
                    if (withmatchlen) addReplyLongLong(c,match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. */
        }
    }

    /* Reply depending on the given options. */
    if (arraylenptr) {
        addReplyBulkCString(c,"len");
        addReplyLongLong(c,LCS(alen,blen));
        setDeferredMultiBulkLength(c,arraylenptr,arraylen);
    } else if (getlen) {
        addReplyLongLong(c,LCS(alen,blen));
    } else {
        addReplyBulkSds(c,result);
        result = NULL;
    }

cleanup:
    sdsfree(result);
    zfree(lcs);
    if (obja) decrRefCount(obja);
    if (objb) decrRefCount(objb);
}
