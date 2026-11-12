/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* IOCP-based ae.c module  */

#include "win32_Interop/win32fixes.h"
#include "adlist.h"
#include "win32_Interop/win32_wsiocp.h"

#define MAX_COMPLETE_PER_POLL   100
#define MAX_SOCKET_LOOKUP       65535

/* Extra threads blocked on the same completion port. Completions are
 * delivered to exactly one waiter, so each thread owns the socket state of
 * the operations it dequeues. Zero keeps the original single-threaded loop. */
#define IOCP_IO_THREADS         3

/* Use GetQueuedCompletionStatusEx if possible.
 * Try to load the function pointer dynamically.
 * If it is not available, use GetQueuedCompletionStatus */
typedef BOOL(WINAPI *sGetQueuedCompletionStatusEx)
            (HANDLE CompletionPort,
            LPOVERLAPPED_ENTRY lpCompletionPortEntries,
            ULONG ulCount,
            PULONG ulNumEntriesRemoved,
            DWORD dwMilliseconds,
            BOOL fAlertable);
sGetQueuedCompletionStatusEx pGetQueuedCompletionStatusEx;

typedef struct aeApiState aeApiState;
static void aeIocpStartWorkers(aeEventLoop *eventLoop, aeApiState *state);

/* One batch of completions dequeued by a single thread. */
typedef struct aeIocpBatch {
    OVERLAPPED_ENTRY entries[MAX_COMPLETE_PER_POLL];
    aeFiredEvent fired[MAX_COMPLETE_PER_POLL];
    int numevents;
    int error;
} aeIocpBatch;

/* Structure that keeps state of sockets and Completion port handle */
typedef struct aeApiState {
    HANDLE iocp;
    int setsize;
    aeIocpBatch mainBatch;
    HANDLE ioThreads[IOCP_IO_THREADS];
    aeIocpBatch *ioBatches[IOCP_IO_THREADS];
    LONG ioBatchLock[IOCP_IO_THREADS];
    volatile LONG ioStop;
} aeApiState;

/* Find matching value in list and remove. If found return TRUE */
BOOL removeMatchFromList(list *requestlist, void *value) {
    listNode *node;
    if (requestlist == NULL) {
        return FALSE;
    }
    node = listFirst(requestlist);
    while (node != NULL) {
        if (listNodeValue(node) == value) {
            listDelNode(requestlist, node);
            return TRUE;
        }
        node = listNextNode(node);
    }
    return FALSE;
}

/* Called by ae to initialize state */
static int aeApiCreate(aeEventLoop *eventLoop) {
    HMODULE kernel32_module;
    aeApiState *state = (aeApiState *) CallocMemoryNoCOW(sizeof(aeApiState));

    if (!state) return -1;

    // Share one IOCP between the event loop and the IO threads.
    // A concurrency of 0 lets the system run one active thread per core;
    // the previous value of 1 throttled every completion onto one thread.
    state->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                         NULL,
                                         0,
                                         0);
    if (state->iocp == NULL) {
        FreeMemoryNoCOW(state);
        return -1;
    }

    pGetQueuedCompletionStatusEx = NULL;
    kernel32_module = GetModuleHandleA("kernel32.dll");
    if (kernel32_module != NULL) {
        pGetQueuedCompletionStatusEx = (sGetQueuedCompletionStatusEx) GetProcAddress(
                                        kernel32_module,
                                        "GetQueuedCompletionStatusEx");
    }

    state->setsize = eventLoop->setsize;
    eventLoop->apidata = state;
    WSIOCP_Init(state->iocp);
    aeIocpStartWorkers(eventLoop, state);
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    ((aeApiState *) (eventLoop->apidata))->setsize = setsize;
    return 0;
}

/* Termination */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    int slot;

    state->ioStop = 1;
    for (slot = 0; slot < IOCP_IO_THREADS; slot++) {
        if (state->ioThreads[slot] != NULL)
            PostQueuedCompletionStatus(state->iocp, 0, 0, NULL);
    }
    for (slot = 0; slot < IOCP_IO_THREADS; slot++) {
        if (state->ioThreads[slot] != NULL) {
            WaitForSingleObject(state->ioThreads[slot], 2000);
            CloseHandle(state->ioThreads[slot]);
        }
        if (state->ioBatches[slot] != NULL)
            FreeMemoryNoCOW(state->ioBatches[slot]);
    }
    CloseHandle(state->iocp);
    FreeMemoryNoCOW(state);
    WSIOCP_Cleanup();
}

/* Monitor state changes for a socket */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    iocpSockState *sockstate = WSIOCP_GetSocketState(fd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    if (mask & AE_READABLE) {
        sockstate->masks |= AE_READABLE;
        if ((sockstate->masks & CONNECT_PENDING) == 0) {
            if (sockstate->masks & LISTEN_SOCK) {
                // Actually a listen. Do not treat as read
            } else {
                if ((sockstate->masks & READ_QUEUED) == 0) {
                    // Queue up a 0 byte read
                    WSIOCP_QueueNextRead(fd);
                }
            }
        }
    }
    if (mask & AE_WRITABLE) {
        sockstate->masks |= AE_WRITABLE;
        if ((sockstate->masks & CONNECT_PENDING) == 0) {
            // If no write active, then need to queue write ready
            if (sockstate->wreqs == 0) {
                asendreq *areq = (asendreq *) CallocMemoryNoCOW(sizeof(asendreq));
                aeApiState *state = (aeApiState *) eventLoop->apidata;
                if (PostQueuedCompletionStatus(state->iocp,
                                               0,
                                               fd,
                                               &areq->ov) == 0) {
                    errno = GetLastError();
                    FreeMemoryNoCOW(areq);
                    return -1;
                }
                sockstate->wreqs++;
                listAddNodeTail(&sockstate->wreqlist, areq);
            }
        }
    }
    return 0;
}

/* Stop monitoring state changes for a socket */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    iocpSockState *sockstate = WSIOCP_GetExistingSocketState(fd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return;
    }

    if (mask & AE_READABLE) sockstate->masks &= ~AE_READABLE;
    if (mask & AE_WRITABLE) sockstate->masks &= ~AE_WRITABLE;
}

/* Apply one thread's batch of completions. Each completion belongs to
 * exactly one waiter, and a socket has only one read and a serial chain of
 * writes outstanding, so the socket state touched here is not shared with
 * another thread. Ready sockets are recorded in the batch, not in the event
 * loop, because several threads finish batches at once. */
static void aeIocpApplyCompletions(aeEventLoop *eventLoop, aeApiState *state,
                                   aeIocpBatch *batch, ULONG numComplete) {
    ULONG j;
    iocpSockState *sockstate;
    int numevents = 0;
    LPOVERLAPPED_ENTRY entry = batch->entries;

    for (j = 0; j < numComplete && numevents < MAX_COMPLETE_PER_POLL; j++, entry++) {
            // The competion key is the rfd identifying the socket 
            int rfd = (int) entry->lpCompletionKey;
            sockstate = WSIOCP_GetExistingSocketState(rfd);
            if (sockstate == NULL) {
                continue;
            }

            if ((sockstate->masks & CLOSE_PENDING) == FALSE) {
                if ((sockstate->masks & LISTEN_SOCK) && entry->lpOverlapped != NULL) {
                    // Need to set event for listening
                    aacceptreq *areq = (aacceptreq *) entry->lpOverlapped;
                    areq->next = sockstate->reqs;
                    sockstate->reqs = areq;
                    sockstate->masks &= ~ACCEPT_PENDING;
                    if (sockstate->masks & AE_READABLE) {
                        batch->fired[numevents].fd = rfd;
                        batch->fired[numevents].mask = AE_READABLE;
                        numevents++;
                    }
                } else if (sockstate->masks & CONNECT_PENDING) {
                    // Check if connect complete
                    if (entry->lpOverlapped == &sockstate->ov_read) {
                        sockstate->masks &= ~CONNECT_PENDING;
                        // Enable read and write events for this connection
                        aeApiAddEvent(eventLoop, rfd, sockstate->masks);
                    }
                } else {
                    BOOL matched = FALSE;
                    // Check if event is read complete (may be 0 length read)
                    if (entry->lpOverlapped == &sockstate->ov_read) {
                        matched = TRUE;
                        sockstate->masks &= ~READ_QUEUED;
                        if (sockstate->masks & AE_READABLE) {
                            batch->fired[numevents].fd = rfd;
                            batch->fired[numevents].mask = AE_READABLE;
                            numevents++;
                        }
                    } else if (sockstate->wreqs > 0 && entry->lpOverlapped != NULL) {
                        // Should be write complete. Get results
                        asendreq *areq = (asendreq *) entry->lpOverlapped;
                        matched = removeMatchFromList(&sockstate->wreqlist, areq);
                        if (matched == TRUE) {
                            // Call write complete callback so buffers can be freed
                            if (areq->proc != NULL) {
                                DWORD written = 0;
                                DWORD flags;
                                FDAPI_WSAGetOverlappedResult(rfd, &areq->ov, &written, FALSE, &flags);
                                areq->proc(areq->eventLoop, rfd, &areq->req, (int) written);
                            }
                            sockstate->wreqs--;
                            FreeMemoryNoCOW(areq);
                            // If no active write requests, set ready to write
                            if (sockstate->wreqs == 0 && sockstate->masks & AE_WRITABLE) {
                                batch->fired[numevents].fd = rfd;
                                batch->fired[numevents].mask = AE_WRITABLE;
                                numevents++;
                            }
                        }
                    }
                    if (matched == 0 && sockstate->unknownComplete == 0) {
                        sockstate->unknownComplete = 1;
                        close(rfd);
                    }
                }
            } else {
                if (sockstate->masks & CONNECT_PENDING) {
                    // Check if connect complete
                    if (entry->lpOverlapped == &sockstate->ov_read) {
                        sockstate->masks &= ~CONNECT_PENDING;
                    }
                } else if (entry->lpOverlapped == &sockstate->ov_read) {
                    // Read complete
                    sockstate->masks &= ~READ_QUEUED;
                } else {
                    // Check pending writes
                    asendreq *areq = (asendreq *) entry->lpOverlapped;
                    if (removeMatchFromList(&sockstate->wreqlist, areq)) {
                        sockstate->wreqs--;
                        FreeMemoryNoCOW(areq);
                    }
                }
                if (sockstate->wreqs == 0 &&
                    (sockstate->masks & (CONNECT_PENDING | READ_QUEUED | SOCKET_ATTACHED)) == 0) {
                    sockstate->masks &= ~(CLOSE_PENDING);
                    if (WSIOCP_CloseSocketState(sockstate)) {
                        FDAPI_ClearSocketInfo(rfd);
                    }
                }
            }
        }
    batch->numevents = numevents;
    (void) eventLoop;
    (void) state;
}

/* Dequeue one batch. A zero wait returns whatever is already queued. */
static ULONG aeIocpDequeue(aeApiState *state, aeIocpBatch *batch, int mswait) {
    ULONG numComplete = 0;
    BOOL rc;

    if (pGetQueuedCompletionStatusEx != NULL) {
        rc = pGetQueuedCompletionStatusEx(state->iocp,
                                          batch->entries,
                                          MAX_COMPLETE_PER_POLL,
                                          &numComplete,
                                          mswait,
                                          FALSE);
        return rc ? numComplete : 0;
    }

    rc = GetQueuedCompletionStatus(state->iocp,
                                   &batch->entries[0].dwNumberOfBytesTransferred,
                                   &batch->entries[0].lpCompletionKey,
                                   &batch->entries[0].lpOverlapped,
                                   mswait);
    if (!rc && batch->entries[0].lpOverlapped == NULL) return 0;

    numComplete = 1;
    while (numComplete < MAX_COMPLETE_PER_POLL) {
        BOOL lrc = GetQueuedCompletionStatus(state->iocp,
                        &batch->entries[numComplete].dwNumberOfBytesTransferred,
                        &batch->entries[numComplete].lpCompletionKey,
                        &batch->entries[numComplete].lpOverlapped,
                        0);
        if (!lrc && batch->entries[numComplete].lpOverlapped == NULL) break;
        numComplete++;
    }
    return numComplete;
}

/* Hand a worker batch to the event loop. The worker blocks until the loop
 * has copied it out, so the batch storage is never reused early. */
static void aeIocpPublishBatch(aeApiState *state, int slot) {
    aeIocpBatch *batch = state->ioBatches[slot];
    if (batch->numevents == 0 && batch->error == 0) return;
    while (InterlockedCompareExchange(&state->ioBatchLock[slot], 1, 0) != 0) {
        if (state->ioStop) return;
        Sleep(1);
    }
}

static DWORD WINAPI aeIocpWorker(LPVOID arg);

static void aeIocpStartWorkers(aeEventLoop *eventLoop, aeApiState *state) {
    int slot;
    for (slot = 0; slot < IOCP_IO_THREADS; slot++) {
        state->ioBatches[slot] = (aeIocpBatch *) CallocMemoryNoCOW(sizeof(aeIocpBatch));
        if (state->ioBatches[slot] == NULL) {
            state->ioStop = 1;
            return;
        }
        state->ioThreads[slot] = CreateThread(NULL, 0, aeIocpWorker,
                                              eventLoop, 0, NULL);
        if (state->ioThreads[slot] == NULL) {
            state->ioStop = 1;
            return;
        }
    }
}

static DWORD WINAPI aeIocpWorker(LPVOID arg) {
    aeEventLoop *eventLoop = (aeEventLoop *) arg;
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    int slot;

    for (slot = 0; slot < IOCP_IO_THREADS; slot++) {
        if (state->ioThreads[slot] == GetCurrentThread()) break;
    }
    if (slot == IOCP_IO_THREADS) return 0;

    while (!state->ioStop) {
        aeIocpBatch *batch = state->ioBatches[slot];
        ULONG numComplete = aeIocpDequeue(state, batch, 100);
        if (numComplete == 0) continue;
        batch->numevents = 0;
        batch->error = 0;
        aeIocpApplyCompletions(eventLoop, state, batch, numComplete);
        aeIocpPublishBatch(state, slot);
    }
    return 0;
}

/* Return array of sockets that are ready for read or write
 * depending on the mask for each socket */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = (aeApiState *) eventLoop->apidata;
    int numevents = 0;
    int mswait = (tvp == NULL) ? 100 : (tvp->tv_sec * 1000) + (tvp->tv_usec / 1000);
    ULONG numComplete;
    int slot;

    /* Take the completions the worker threads finished since last poll. */
    for (slot = 0; slot < IOCP_IO_THREADS && numevents < state->setsize; slot++) {
        aeIocpBatch *batch = state->ioBatches[slot];
        int i;
        if (batch == NULL) continue;
        if (InterlockedCompareExchange(&state->ioBatchLock[slot], 0, 1) != 1)
            continue;
        for (i = 0; i < batch->numevents && numevents < state->setsize; i++) {
            eventLoop->fired[numevents].fd = batch->fired[i].fd;
            eventLoop->fired[numevents].mask = batch->fired[i].mask;
            numevents++;
        }
        batch->numevents = 0;
    }

    /* A full batch from the workers needs no extra wait. */
    if (numevents > 0) mswait = 0;

    numComplete = aeIocpDequeue(state, &state->mainBatch, mswait);
    if (numComplete > 0) {
        int i;
        state->mainBatch.numevents = 0;
        aeIocpApplyCompletions(eventLoop, state, &state->mainBatch, numComplete);
        for (i = 0; i < state->mainBatch.numevents && numevents < state->setsize; i++) {
            eventLoop->fired[numevents].fd = state->mainBatch.fired[i].fd;
            eventLoop->fired[numevents].mask = state->mainBatch.fired[i].mask;
            numevents++;
        }
    }
    return numevents;
}

/* Name of this event handler */
static char *aeApiName(void) {
    return "WinSock_IOCP";
}
