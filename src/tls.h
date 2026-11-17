#ifndef __REDIS_TLS_H
#define __REDIS_TLS_H

int tlsInit(void);
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

#endif
