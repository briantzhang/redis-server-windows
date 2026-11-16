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

#endif
