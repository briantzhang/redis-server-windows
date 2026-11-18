#ifndef __REDIS_TLS_H
#define __REDIS_TLS_H

int tlsInit(void);
int tlsApplyConfig(void);
const char *tlsLastConfigError(void);
void tlsCleanup(void);
int tlsAttachClient(struct client *c);
void tlsFreeClient(struct client *c);
int tlsRead(struct client *c, void *buf, int len);
int tlsWrite(struct client *c, const void *buf, int len);
size_t tlsPendingBytes(struct client *c);
int tlsFlushPending(struct client *c);

/* Replica -> master link (tls-replication yes). One link exists at a time. */
int tlsReplConnect(int fd, PORT_LONGLONG timeout);
int tlsReplActive(int fd);
ssize_t tlsReplWrite(int fd, const char *buf, size_t len, PORT_LONGLONG timeout);
ssize_t tlsReplRead(int fd, char *buf, size_t len, PORT_LONGLONG timeout);
int tlsReplReadSome(int fd, char *buf, int len);
size_t tlsReplPending(int fd);
int tlsReplWriteNewline(int fd);
int tlsReplAdoptClient(struct client *c);
void tlsReplCleanup(void);

/* Cluster bus TLS (tls-cluster yes). Cluster links are driven by the event
 * loop instead of the blocking helpers used for clients and replication, so
 * the state below is embedded in every clusterLink. */
typedef struct tlsLinkState {
    void *ssl;          /* SSL object, NULL when this link is plaintext */
    void *rbio;         /* Memory BIO with ciphertext read from the socket */
    void *wbio;         /* Memory BIO with ciphertext to write to the socket */
    sds outbuf;         /* Ciphertext not yet flushed to the socket */
    int handshaking;    /* TLS handshake still in progress */
} tlsLinkState;

int tlsClusterLinkInit(tlsLinkState *st, int is_server);
void tlsClusterLinkFree(tlsLinkState *st);
int tlsClusterHandshake(tlsLinkState *st, int fd);
int tlsClusterFlush(tlsLinkState *st, int fd);
int tlsClusterPendingOut(tlsLinkState *st);
int tlsClusterWrite(tlsLinkState *st, int fd, const void *buf, size_t len);
ssize_t tlsClusterRead(tlsLinkState *st, int fd, void *buf, size_t len);

#endif
