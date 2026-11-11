/* tracking.c - Client side caching: keys tracking and invalidation
 *
 * Adapted for the Windows Redis 5.0.14.1 backport (bp13). This build is
 * RESP2-only: invalidation is delivered as Pub/Sub messages on the
 * __redis__:invalidate channel (requires REDIRECT to a CLIENT_PUBSUB peer).
 *
 * Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* The tracking table is constituted by a radix tree of keys, each pointing
 * to a radix tree of client IDs, used to track the clients that may have
 * certain keys in their local, client side, cache. */
rax *TrackingTable = NULL;
rax *PrefixTable = NULL;
uint64_t TrackingTableTotalItems = 0;
robj *TrackingChannelName;

/* PrefixTable value: keys modified this event-loop cycle + clients listening. */
typedef struct bcastState {
    rax *keys;
    rax *clients;
} bcastState;

/* Remove the tracking state from the client 'c'. */
void disableTracking(client *c) {
    if (c->flags & CLIENT_TRACKING_BCAST) {
        raxIterator ri;
        raxStart(&ri,c->client_tracking_prefixes);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            bcastState *bs = raxFind(PrefixTable,ri.key,ri.key_len);
            serverAssert(bs != raxNotFound);
            raxRemove(bs->clients,(unsigned char*)&c,sizeof(c),NULL);
            if (raxSize(bs->clients) == 0) {
                raxFree(bs->clients);
                raxFree(bs->keys);
                zfree(bs);
                raxRemove(PrefixTable,ri.key,ri.key_len,NULL);
            }
        }
        raxStop(&ri);
        raxFree(c->client_tracking_prefixes);
        c->client_tracking_prefixes = NULL;
    }

    if (c->flags & CLIENT_TRACKING) {
        server.tracking_clients--;
        c->flags &= ~(CLIENT_TRACKING|CLIENT_TRACKING_BROKEN_REDIR|
                      CLIENT_TRACKING_BCAST|CLIENT_TRACKING_OPTIN|
                      CLIENT_TRACKING_OPTOUT|CLIENT_TRACKING_CACHING|
                      CLIENT_TRACKING_NOLOOP);
        c->client_tracking_redirection = 0;
    }
}

static int stringCheckPrefix(unsigned char *s1, size_t s1_len,
                             unsigned char *s2, size_t s2_len)
{
    size_t min_length = s1_len < s2_len ? s1_len : s2_len;
    return memcmp(s1,s2,min_length) == 0;
}

/* Check that provided prefixes do not collide with each other or with the
 * prefixes already registered for this client. */
int checkPrefixCollisionsOrReply(client *c, robj **prefixes, size_t numprefix) {
    size_t i, j;

    for (i = 0; i < numprefix; i++) {
        if (c->client_tracking_prefixes) {
            raxIterator ri;
            raxStart(&ri,c->client_tracking_prefixes);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                if (stringCheckPrefix(ri.key,ri.key_len,
                    prefixes[i]->ptr,sdslen(prefixes[i]->ptr)))
                {
                    sds collision = sdsnewlen(ri.key,ri.key_len);
                    addReplyErrorFormat(c,
                        "Prefix '%s' overlaps with an existing one: '%s'. "
                        "Prefixes for a single client should not overlap.",
                        (unsigned char *)prefixes[i]->ptr,
                        (unsigned char *)collision);
                    sdsfree(collision);
                    raxStop(&ri);
                    return 0;
                }
            }
            raxStop(&ri);
        }
        for (j = 0; j < i; j++) {
            if (stringCheckPrefix(prefixes[i]->ptr,sdslen(prefixes[i]->ptr),
                                  prefixes[j]->ptr,sdslen(prefixes[j]->ptr)))
            {
                addReplyErrorFormat(c,
                    "Prefix '%s' overlaps with another given for this command: "
                    "'%s'. Prefixes for a single client should not overlap.",
                    (unsigned char *)prefixes[i]->ptr,
                    (unsigned char *)prefixes[j]->ptr);
                return 0;
            }
        }
    }
    return 1;
}

void enableBcastTrackingForPrefix(client *c, char *prefix, size_t plen) {
    bcastState *bs = raxFind(PrefixTable,(unsigned char*)prefix,plen);
    if (bs == raxNotFound) {
        bs = zmalloc(sizeof(*bs));
        bs->keys = raxNew();
        bs->clients = raxNew();
        raxInsert(PrefixTable,(unsigned char*)prefix,plen,bs,NULL);
    }
    if (raxTryInsert(bs->clients,(unsigned char*)&c,sizeof(c),NULL,NULL)) {
        if (c->client_tracking_prefixes == NULL)
            c->client_tracking_prefixes = raxNew();
        raxInsert(c->client_tracking_prefixes,
                  (unsigned char*)prefix,plen,NULL,NULL);
    }
}

void enableTracking(client *c, uint64_t redirect_to, uint64_t options,
                    robj **prefix, size_t numprefix)
{
    if (!(c->flags & CLIENT_TRACKING)) server.tracking_clients++;
    c->flags |= CLIENT_TRACKING;
    c->flags &= ~(CLIENT_TRACKING_BROKEN_REDIR|CLIENT_TRACKING_BCAST|
                  CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT|
                  CLIENT_TRACKING_NOLOOP|CLIENT_TRACKING_CACHING);
    c->client_tracking_redirection = redirect_to;

    if (TrackingTable == NULL) {
        TrackingTable = raxNew();
        PrefixTable = raxNew();
        TrackingChannelName = createStringObject("__redis__:invalidate",20);
    }

    if (options & CLIENT_TRACKING_BCAST) {
        c->flags |= CLIENT_TRACKING_BCAST;
        if (numprefix == 0) enableBcastTrackingForPrefix(c,"",0);
        for (size_t j = 0; j < numprefix; j++) {
            sds sdsprefix = prefix[j]->ptr;
            enableBcastTrackingForPrefix(c,sdsprefix,sdslen(sdsprefix));
        }
    }

    c->flags |= options & (CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT|
                           CLIENT_TRACKING_NOLOOP);
}

/* After a readonly command, remember the keys this client may have cached. */
void trackingRememberKeys(client *tracking) {
    uint64_t optin = tracking->flags & CLIENT_TRACKING_OPTIN;
    uint64_t optout = tracking->flags & CLIENT_TRACKING_OPTOUT;
    uint64_t caching_given = tracking->flags & CLIENT_TRACKING_CACHING;
    if ((optin && !caching_given) || (optout && caching_given)) return;

    if (tracking->cmd->flags & CMD_PUBSUB) return;

    int numkeys = 0;
    int *keyindex = getKeysFromCommand(tracking->cmd,tracking->argv,
                                       tracking->argc,&numkeys);
    if (!numkeys) {
        getKeysFreeResult(keyindex);
        return;
    }

    if (TrackingTable == NULL) {
        TrackingTable = raxNew();
        PrefixTable = raxNew();
        TrackingChannelName = createStringObject("__redis__:invalidate",20);
    }

    for (int j = 0; j < numkeys; j++) {
        int idx = keyindex[j];
        sds sdskey = tracking->argv[idx]->ptr;
        rax *ids = raxFind(TrackingTable,(unsigned char*)sdskey,sdslen(sdskey));
        if (ids == raxNotFound) {
            ids = raxNew();
            raxInsert(TrackingTable,(unsigned char*)sdskey,
                      sdslen(sdskey),ids,NULL);
        }
        if (raxTryInsert(ids,(unsigned char*)&tracking->id,
                         sizeof(tracking->id),NULL,NULL))
            TrackingTableTotalItems++;
    }
    getKeysFreeResult(keyindex);
}

/* Deliver a Pub/Sub invalidation message to 'c' (must be CLIENT_PUBSUB).
 * 'proto' means keyname/keylen already contain a RESP array (or null bulk). */
static void sendTrackingMessage(client *c, char *keyname, size_t keylen, int proto) {
    int using_redirection = 0;
    if (c->client_tracking_redirection) {
        client *redir = lookupClientByID(c->client_tracking_redirection);
        if (!redir) {
            c->flags |= CLIENT_TRACKING_BROKEN_REDIR;
            return;
        }
        c = redir;
        using_redirection = 1;
    }

    /* This RESP2-only build can only push invalidations via Pub/Sub. */
    if (!(using_redirection && (c->flags & CLIENT_PUBSUB))) return;

    /* Pub/Sub envelope: ["message", "__redis__:invalidate", <payload>] */
    addReply(c,shared.mbulkhdr[3]);
    addReply(c,shared.messagebulk);
    addReplyBulk(c,TrackingChannelName);
    if (proto) {
        addReplyString(c,keyname,keylen);
    } else {
        addReplyMultiBulkLen(c,1);
        addReplyBulkCBuffer(c,keyname,keylen);
    }
}

void trackingRememberKeyToBroadcast(client *c, char *keyname, size_t keylen) {
    raxIterator ri;
    raxStart(&ri,PrefixTable);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        if (ri.key_len > keylen) continue;
        if (ri.key_len != 0 && memcmp(ri.key,keyname,ri.key_len) != 0)
            continue;
        bcastState *bs = ri.data;
        raxInsert(bs->keys,(unsigned char*)keyname,keylen,c,NULL);
    }
    raxStop(&ri);
}

void trackingInvalidateKey(client *c, robj *keyobj, int bcast) {
    if (TrackingTable == NULL) return;

    unsigned char *key = (unsigned char*)keyobj->ptr;
    size_t keylen = sdslen(keyobj->ptr);

    if (bcast && PrefixTable && raxSize(PrefixTable) > 0)
        trackingRememberKeyToBroadcast(c,(char *)key,keylen);

    rax *ids = raxFind(TrackingTable,key,keylen);
    if (ids == raxNotFound) return;

    raxIterator ri;
    raxStart(&ri,ids);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        uint64_t id;
        memcpy(&id,ri.key,sizeof(id));
        client *target = lookupClientByID(id);
        if (target == NULL ||
            !(target->flags & CLIENT_TRACKING) ||
            (target->flags & CLIENT_TRACKING_BCAST))
        {
            continue;
        }

        if ((target->flags & CLIENT_TRACKING_NOLOOP) &&
            target == server.current_client)
        {
            continue;
        }

        sendTrackingMessage(target,(char *)keyobj->ptr,sdslen(keyobj->ptr),0);
    }
    raxStop(&ri);

    TrackingTableTotalItems -= raxSize(ids);
    raxFree(ids);
    raxRemove(TrackingTable,(unsigned char*)key,keylen,NULL);
}

void freeTrackingRadixTreeCallback(void *rt) {
    raxFree(rt);
}

void freeTrackingRadixTree(rax *rt) {
    raxFreeWithCallback(rt,freeTrackingRadixTreeCallback);
}

/* FLUSHDB/FLUSHALL: tell trackers that everything is gone (null bulk payload). */
void trackingInvalidateKeysOnFlush(int async) {
    UNUSED(async);
    if (server.tracking_clients) {
        listNode *ln;
        listIter li;
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            if (c->flags & CLIENT_TRACKING) {
                /* RESP null bulk means "all keys". */
                sendTrackingMessage(c,shared.nullbulk->ptr,
                                    sdslen(shared.nullbulk->ptr),1);
            }
        }
    }

    if (TrackingTable) {
        freeTrackingRadixTree(TrackingTable);
        TrackingTable = raxNew();
        TrackingTableTotalItems = 0;
    }
}

void trackingLimitUsedSlots(void) {
    static unsigned int timeout_counter = 0;
    if (TrackingTable == NULL) return;
    if (server.tracking_table_max_keys == 0) return;
    size_t max_keys = server.tracking_table_max_keys;
    if (raxSize(TrackingTable) <= max_keys) {
        timeout_counter = 0;
        return;
    }

    int effort = 100 * (timeout_counter+1);
    raxIterator ri;
    raxStart(&ri,TrackingTable);
    while(effort > 0) {
        effort--;
        raxSeek(&ri,"^",NULL,0);
        raxRandomWalk(&ri,0);
        if (raxEOF(&ri)) break;
        robj *keyobj = createStringObject((char*)ri.key,ri.key_len);
        trackingInvalidateKey(NULL,keyobj,0);
        decrRefCount(keyobj);
        if (raxSize(TrackingTable) <= max_keys) {
            timeout_counter = 0;
            raxStop(&ri);
            return;
        }
    }
    raxStop(&ri);
    timeout_counter++;
}

sds trackingBuildBroadcastReply(client *c, rax *keys) {
    raxIterator ri;
    uint64_t count;

    if (c == NULL) {
        count = raxSize(keys);
    } else {
        count = 0;
        raxStart(&ri,keys);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            if (ri.data != c) count++;
        }
        raxStop(&ri);
        if (count == 0) return NULL;
    }

    char buf[32];
    size_t len = ll2string(buf,sizeof(buf),count);
    sds proto = sdsempty();
    proto = sdsMakeRoomFor(proto,count*15);
    proto = sdscatlen(proto,"*",1);
    proto = sdscatlen(proto,buf,len);
    proto = sdscatlen(proto,"\r\n",2);
    raxStart(&ri,keys);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        if (c && ri.data == c) continue;
        len = ll2string(buf,sizeof(buf),ri.key_len);
        proto = sdscatlen(proto,"$",1);
        proto = sdscatlen(proto,buf,len);
        proto = sdscatlen(proto,"\r\n",2);
        proto = sdscatlen(proto,ri.key,ri.key_len);
        proto = sdscatlen(proto,"\r\n",2);
    }
    raxStop(&ri);
    return proto;
}

void trackingBroadcastInvalidationMessages(void) {
    raxIterator ri, ri2;

    if (TrackingTable == NULL || !PrefixTable || raxSize(PrefixTable) == 0) return;

    raxStart(&ri,PrefixTable);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        bcastState *bs = ri.data;

        if (raxSize(bs->keys)) {
            /* Default (with NOLOOP) reply and one without the NOLOOP filter. */
            sds proto = NULL;
            sds proto_noloop = NULL;

            raxStart(&ri2,bs->clients);
            raxSeek(&ri2,"^",NULL,0);
            while(raxNext(&ri2)) {
                client *c;
                memcpy(&c,ri2.key,sizeof(c));
                if (c->flags & CLIENT_TRACKING_NOLOOP) {
                    if (proto_noloop == NULL)
                        proto_noloop = trackingBuildBroadcastReply(c,bs->keys);
                    if (proto_noloop)
                        sendTrackingMessage(c,proto_noloop,sdslen(proto_noloop),1);
                } else {
                    if (proto == NULL)
                        proto = trackingBuildBroadcastReply(NULL,bs->keys);
                    if (proto)
                        sendTrackingMessage(c,proto,sdslen(proto),1);
                }
            }
            raxStop(&ri2);
            if (proto) sdsfree(proto);
            if (proto_noloop) sdsfree(proto_noloop);
            raxFree(bs->keys);
            bs->keys = raxNew();
        }
    }
    raxStop(&ri);
}

uint64_t trackingGetTotalItems(void) {
    return TrackingTableTotalItems;
}

uint64_t trackingGetTotalKeys(void) {
    if (TrackingTable == NULL) return 0;
    return raxSize(TrackingTable);
}

uint64_t trackingGetTotalPrefixes(void) {
    if (PrefixTable == NULL) return 0;
    return raxSize(PrefixTable);
}
