/* Client-side TLS for the Windows Redis 5 backport.
 *
 * OpenSSL uses memory BIOs. Encrypted bytes are read with the same read()
 * path as plaintext clients and produced ciphertext is flushed with write()
 * plus a writable event. This keeps the Windows IOCP readiness loop intact
 * without introducing a connection abstraction. */

#include "server.h"
#include "tls.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

static SSL_CTX *tls_ctx = NULL;
static SSL_CTX *tls_client_ctx = NULL;

static int tlsEnsureHandshake(client *c);
static int tlsFlush(client *c);

static void tlsLogError(const char *what) {
    unsigned long err = ERR_get_error();
    char buf[256];
    if (err) {
        ERR_error_string_n(err, buf, sizeof(buf));
        serverLog(LL_WARNING, "%s: %s", what, buf);
    } else {
        serverLog(LL_WARNING, "%s: unknown OpenSSL error", what);
    }
}

int tlsInit(void) {
    if (tls_ctx) return C_OK;
    if (server.tls_cert_file == NULL || server.tls_key_file == NULL) {
        serverLog(LL_WARNING, "TLS enabled but tls-cert-file or tls-key-file is missing");
        return C_ERR;
    }

    tls_ctx = SSL_CTX_new(TLS_server_method());
    if (!tls_ctx) {
        tlsLogError("SSL_CTX_new");
        return C_ERR;
    }
    SSL_CTX_set_min_proto_version(tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(tls_ctx,
        SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_session_cache_mode(tls_ctx, SSL_SESS_CACHE_SERVER);

    if (SSL_CTX_use_certificate_chain_file(tls_ctx, server.tls_cert_file) != 1) {
        tlsLogError("SSL_CTX_use_certificate_chain_file");
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
        return C_ERR;
    }
    if (SSL_CTX_use_PrivateKey_file(tls_ctx, server.tls_key_file,
                                    SSL_FILETYPE_PEM) != 1) {
        tlsLogError("SSL_CTX_use_PrivateKey_file");
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
        return C_ERR;
    }
    if (SSL_CTX_check_private_key(tls_ctx) != 1) {
        tlsLogError("SSL_CTX_check_private_key");
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
        return C_ERR;
    }

    if (server.tls_ca_cert_file || server.tls_ca_cert_dir) {
        if (SSL_CTX_load_verify_locations(tls_ctx,
                server.tls_ca_cert_file, server.tls_ca_cert_dir) != 1)
        {
            tlsLogError("SSL_CTX_load_verify_locations");
            SSL_CTX_free(tls_ctx);
            tls_ctx = NULL;
            return C_ERR;
        }
        SSL_CTX_set_verify(tls_ctx,
            server.tls_auth_clients ?
                SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT :
                SSL_VERIFY_NONE, NULL);
    } else if (server.tls_auth_clients) {
        serverLog(LL_WARNING,
            "tls-auth-clients yes requires tls-ca-cert-file or tls-ca-cert-dir");
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
        return C_ERR;
    }

    return C_OK;
}

void tlsCleanup(void) {
    if (tls_ctx) {
        SSL_CTX_free(tls_ctx);
        tls_ctx = NULL;
    }
    if (tls_client_ctx) {
        SSL_CTX_free(tls_client_ctx);
        tls_client_ctx = NULL;
    }
}

int tlsAttachClient(client *c) {
    SSL *ssl;
    BIO *rbio, *wbio;

    if (tls_ctx == NULL) return C_ERR;
    ssl = SSL_new(tls_ctx);
    if (!ssl) {
        tlsLogError("SSL_new");
        return C_ERR;
    }
    rbio = BIO_new(BIO_s_mem());
    wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        BIO_free(rbio);
        BIO_free(wbio);
        SSL_free(ssl);
        tlsLogError("BIO_new");
        return C_ERR;
    }
    SSL_set_bio(ssl, rbio, wbio);
    SSL_set_accept_state(ssl);
    c->tls_ssl = ssl;
    c->tls_handshaking = 1;
    return C_OK;
}

void tlsFreeClient(client *c) {
    if (c->tls_ssl) {
        SSL_free((SSL*)c->tls_ssl);
        c->tls_ssl = NULL;
    }
    if (c->tls_outbuf) {
        sdsfree(c->tls_outbuf);
        c->tls_outbuf = NULL;
    }
    c->tls_handshaking = 0;
}

static int tlsAppendOut(client *c, const char *buf, int len) {
    if (len <= 0) return C_OK;
    if (c->tls_outbuf == NULL) c->tls_outbuf = sdsempty();
    c->tls_outbuf = sdscatlen(c->tls_outbuf, buf, len);
    return C_OK;
}

static int tlsDrainWbio(client *c) {
    SSL *ssl = (SSL*)c->tls_ssl;
    BIO *wbio = SSL_get_wbio(ssl);
    char buf[16384];

    while (BIO_ctrl_pending(wbio) > 0) {
        int n = BIO_read(wbio, buf, sizeof(buf));
        if (n <= 0) break;
        tlsAppendOut(c, buf, n);
    }
    return tlsFlush(c);
}

/* Encrypted input is read from the socket and handed to the rbio.
 * Returns 1 for data, 0 for orderly EOF, 2 for "would block" and -1 on
 * error. EOF must stay distinct from EAGAIN: a fragmented handshake often
 * has no complete record yet while the connection is perfectly healthy. */
static int tlsReadRaw(client *c) {
    SSL *ssl = (SSL*)c->tls_ssl;
    BIO *rbio = SSL_get_rbio(ssl);
    char buf[16384];
    int n = (int)read(c->fd, buf, sizeof(buf));

    if (n > 0) {
        if (BIO_write(rbio, buf, n) != n) {
            errno = ECONNRESET;
            return -1;
        }
        return 1;
    }
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == WSAEWOULDBLOCK) return 2;
    return -1;
}

/* Returns 0 when the handshake is complete, 1 when more I/O is needed, and
 * -1 on a fatal protocol or socket error. */
static int tlsEnsureHandshake(client *c) {
    SSL *ssl = (SSL*)c->tls_ssl;
    int ret, err;

    if (!ssl) return -1;
    if (!c->tls_handshaking) return 0;

    ERR_clear_error();
    ret = SSL_do_handshake(ssl);
    if (tlsDrainWbio(c) == C_ERR) return -1;
    if (ret == 1) {
        c->tls_handshaking = 0;
        return 0;
    }
    err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 1;
    tlsLogError("TLS handshake");
    return -1;
}

static int tlsFlush(client *c) {
    while (c->tls_outbuf && sdslen(c->tls_outbuf) > 0) {
        int n = (int)write(c->fd, c->tls_outbuf, sdslen(c->tls_outbuf));
        if (n > 0) {
            sdsrange(c->tls_outbuf, n, -1);
            if (sdslen(c->tls_outbuf) == 0) {
                sdsfree(c->tls_outbuf);
                c->tls_outbuf = NULL;
            }
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == WSAEWOULDBLOCK)) {
            if (aeCreateFileEvent(server.el,c->fd,AE_WRITABLE,
                                  sendReplyToClient,c) == AE_ERR)
                return C_ERR;
            return C_OK;
        }
        return C_ERR;
    }
    return C_OK;
}

int tlsFlushPending(client *c) {
    if (c->tls_outbuf == NULL) return C_OK;
    return tlsFlush(c);
}

/* Bytes that can be returned without another socket read. This includes
 * processed plaintext and unprocessed records still in the read BIO. */
size_t tlsPendingBytes(client *c) {
    SSL *ssl = (SSL*)c->tls_ssl;
    if (!ssl) return 0;
    return (size_t)SSL_pending(ssl) + BIO_ctrl_pending(SSL_get_rbio(ssl));
}

int tlsRead(client *c, void *buf, int len) {
    SSL *ssl = (SSL*)c->tls_ssl;
    int ret, err, raw, handshake, total = 0;

    if (!ssl) {
        errno = ECONNRESET;
        return -1;
    }

    raw = tlsReadRaw(c);
    if (raw < 0) {
        tlsLogError("TLS read");
        errno = ECONNRESET;
        return -1;
    }

    handshake = c->tls_handshaking ? tlsEnsureHandshake(c) : 0;
    if (handshake < 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (handshake > 0) {
        /* There may still be a complete record buffered even if the socket
         * is drained, so fall through to SSL_read before asking for more. */
        if (raw == 0) return 0;
        errno = EAGAIN;
        return -1;
    }

    /* Consume every complete record currently buffered. The caller sizes the
     * buffer with tlsPendingBytes() plus one socket read, so this normally
     * empties the BIO and never leaves plaintext stranded without an IOCP
     * readable completion to trigger another call. */
    for (;;) {
        ERR_clear_error();
        ret = SSL_read(ssl, (char*)buf + total, len - total);
        if (tlsDrainWbio(c) == C_ERR) {
            if (total > 0) return total;
            errno = ECONNRESET;
            return -1;
        }
        if (ret > 0) {
            total += ret;
            if (total == len) break;
            continue;
        }
        err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (total > 0) return total;
            if (raw == 0) return 0;
            errno = EAGAIN;
            return -1;
        }
        if (err == SSL_ERROR_ZERO_RETURN) return total;
        if (err == SSL_ERROR_SYSCALL && raw == 0) return total;
        tlsLogError("SSL_read");
        errno = ECONNRESET;
        return -1;
    }
    return total;
}

int tlsWrite(client *c, const void *buf, int len) {
    SSL *ssl = (SSL*)c->tls_ssl;
    int ret, err;

    if (!ssl) {
        errno = ECONNRESET;
        return -1;
    }
    if (c->tls_handshaking) {
        ret = tlsEnsureHandshake(c);
        if (ret != 0) {
            errno = EAGAIN;
            return -1;
        }
    }

    /* Apply backpressure: don't let application buffers accumulate while
     * ciphertext from an earlier record is still queued for the socket. */
    if (c->tls_outbuf) {
        if (tlsFlush(c) == C_ERR) {
            errno = ECONNRESET;
            return -1;
        }
        if (c->tls_outbuf) {
            errno = EAGAIN;
            return -1;
        }
    }

    ERR_clear_error();
    ret = SSL_write(ssl, buf, len);
    if (tlsDrainWbio(c) == C_ERR) {
        errno = ECONNRESET;
        return -1;
    }
    if (ret > 0) return ret;
    err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    tlsLogError("SSL_write");
    errno = ECONNRESET;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Replica -> master link (tls-replication yes)
 *
 * The replica talks to the master's TLS port. The handshake and the RDB
 * transfer are synchronous, so this path drives the memory BIOs with aeWait()
 * instead of the client event handlers. Once the dataset is loaded the SSL
 * object is handed to the master client and tlsRead()/tlsWrite() take over.
 * ------------------------------------------------------------------------- */

static SSL *repl_ssl = NULL;
static sds repl_outbuf = NULL;
static int repl_fd = -1;

static void tlsReplDrainWbio(void) {
    BIO *wbio;
    char buf[16384];

    if (!repl_ssl) return;
    wbio = SSL_get_wbio(repl_ssl);
    while (BIO_ctrl_pending(wbio) > 0) {
        int n = BIO_read(wbio, buf, sizeof(buf));
        if (n <= 0) break;
        if (repl_outbuf == NULL) repl_outbuf = sdsempty();
        repl_outbuf = sdscatlen(repl_outbuf, buf, n);
    }
}

static void tlsReplFlushBestEffort(int fd) {
    while (repl_outbuf && sdslen(repl_outbuf) > 0) {
        int n = (int)write(fd, repl_outbuf, sdslen(repl_outbuf));
        if (n <= 0) break;
        sdsrange(repl_outbuf, n, -1);
        if (sdslen(repl_outbuf) == 0) {
            sdsfree(repl_outbuf);
            repl_outbuf = NULL;
        }
    }
}

static int tlsReplWait(int fd, int mask, PORT_LONGLONG deadline) {
    PORT_LONGLONG remaining = deadline - mstime();
    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return C_ERR;
    }
    if (aeWait(fd, mask, remaining) <= 0) {
        errno = ETIMEDOUT;
        return C_ERR;
    }
    return C_OK;
}

static int tlsReplFlush(int fd, PORT_LONGLONG deadline) {
    while (repl_outbuf && sdslen(repl_outbuf) > 0) {
        int n = (int)write(fd, repl_outbuf, sdslen(repl_outbuf));
        if (n > 0) {
            sdsrange(repl_outbuf, n, -1);
            if (sdslen(repl_outbuf) == 0) {
                sdsfree(repl_outbuf);
                repl_outbuf = NULL;
            }
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == WSAEWOULDBLOCK)) {
            if (tlsReplWait(fd, AE_WRITABLE, deadline) == C_ERR) return C_ERR;
            continue;
        }
        return C_ERR;
    }
    return C_OK;
}

/* One nonblocking socket read into the read BIO. Returns 1 for data, 0 for
 * EOF, 2 for "would block" and -1 on error. */
static int tlsReplRawReadOnce(int fd) {
    char buf[16384];
    int n = (int)read(fd, buf, sizeof(buf));

    if (n > 0) {
        if (BIO_write(SSL_get_rbio(repl_ssl), buf, n) != n) return -1;
        return 1;
    }
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == WSAEWOULDBLOCK) return 2;
    return -1;
}

static int tlsReplHandshake(int fd, PORT_LONGLONG deadline) {
    for (;;) {
        int ret, err, raw;
        ERR_clear_error();
        ret = SSL_connect(repl_ssl);
        tlsReplDrainWbio();
        if (tlsReplFlush(fd, deadline) == C_ERR) return C_ERR;
        if (ret == 1) return C_OK;
        err = SSL_get_error(repl_ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            do {
                raw = tlsReplRawReadOnce(fd);
                if (raw == 2 &&
                    tlsReplWait(fd, AE_READABLE, deadline) == C_ERR)
                    return C_ERR;
            } while (raw == 2);
            if (raw != 1) return C_ERR;
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (tlsReplWait(fd, AE_WRITABLE, deadline) == C_ERR) return C_ERR;
            continue;
        }
        tlsLogError("TLS replication handshake");
        return C_ERR;
    }
}

/* The client side context. The certificate is optional (a master may allow
 * clients without one) and the CA decides whether the master is verified. */
static SSL_CTX *tlsGetClientCtx(void) {
    SSL_CTX *ctx;

    if (tls_client_ctx) return tls_client_ctx;
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        tlsLogError("SSL_CTX_new client");
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx,
        SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (server.tls_cert_file || server.tls_key_file) {
        if (!server.tls_cert_file || !server.tls_key_file) {
            serverLog(LL_WARNING,
                "tls-replication needs both tls-cert-file and tls-key-file");
            SSL_CTX_free(ctx);
            return NULL;
        }
        if (SSL_CTX_use_certificate_chain_file(ctx,
                server.tls_cert_file) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, server.tls_key_file,
                SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1)
        {
            tlsLogError("TLS replication client certificate");
            SSL_CTX_free(ctx);
            return NULL;
        }
    }

    if (server.tls_ca_cert_file || server.tls_ca_cert_dir) {
        if (SSL_CTX_load_verify_locations(ctx, server.tls_ca_cert_file,
                server.tls_ca_cert_dir) != 1)
        {
            tlsLogError("TLS replication CA");
            SSL_CTX_free(ctx);
            return NULL;
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    }

    tls_client_ctx = ctx;
    return ctx;
}

int tlsReplConnect(int fd, PORT_LONGLONG timeout) {
    SSL_CTX *ctx = tlsGetClientCtx();
    SSL *ssl;
    BIO *rbio, *wbio;

    if (!ctx) return C_ERR;
    ssl = SSL_new(ctx);
    if (!ssl) {
        tlsLogError("SSL_new replication");
        return C_ERR;
    }
    rbio = BIO_new(BIO_s_mem());
    wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        BIO_free(rbio);
        BIO_free(wbio);
        SSL_free(ssl);
        tlsLogError("BIO_new replication");
        return C_ERR;
    }
    SSL_set_bio(ssl, rbio, wbio);
    SSL_set_connect_state(ssl);

    repl_ssl = ssl;
    repl_outbuf = NULL;
    repl_fd = fd;
    if (tlsReplHandshake(fd, mstime() + timeout) != C_OK) {
        tlsReplCleanup();
        return C_ERR;
    }
    return C_OK;
}

int tlsReplActive(int fd) {
    return repl_ssl != NULL && fd == repl_fd;
}

ssize_t tlsReplWrite(int fd, const char *buf, size_t len,
                     PORT_LONGLONG timeout) {
    PORT_LONGLONG deadline = mstime() + timeout;
    size_t off = 0;

    if (!tlsReplActive(fd)) {
        errno = ENOTCONN;
        return -1;
    }
    if (tlsReplFlush(fd, deadline) == C_ERR) return -1;

    while (off < len) {
        int n, err;
        ERR_clear_error();
        n = SSL_write(repl_ssl, buf + off, (int)(len - off));
        tlsReplDrainWbio();
        if (tlsReplFlush(fd, deadline) == C_ERR) return -1;
        if (n > 0) {
            off += n;
            continue;
        }
        err = SSL_get_error(repl_ssl, n);
        if (err == SSL_ERROR_WANT_READ) {
            int raw;
            do {
                raw = tlsReplRawReadOnce(fd);
                if (raw == 2 &&
                    tlsReplWait(fd, AE_READABLE, deadline) == C_ERR)
                    return -1;
            } while (raw == 2);
            if (raw != 1) return -1;
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (tlsReplWait(fd, AE_WRITABLE, deadline) == C_ERR) return -1;
            continue;
        }
        tlsLogError("TLS replication write");
        return -1;
    }
    return (ssize_t)len;
}

ssize_t tlsReplRead(int fd, char *buf, size_t len, PORT_LONGLONG timeout) {
    PORT_LONGLONG deadline = mstime() + timeout;
    size_t got = 0;

    if (!tlsReplActive(fd)) {
        errno = ENOTCONN;
        return -1;
    }

    while (got < len) {
        int n, err;
        ERR_clear_error();
        n = SSL_read(repl_ssl, buf + got, (int)(len - got));
        if (n > 0) {
            got += n;
            continue;
        }
        err = SSL_get_error(repl_ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            tlsReplDrainWbio();
            if (tlsReplFlush(fd, deadline) == C_ERR) return -1;
            if (tlsReplWait(fd, AE_WRITABLE, deadline) == C_ERR) return -1;
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            int raw;
            do {
                raw = tlsReplRawReadOnce(fd);
                if (raw == 2 &&
                    tlsReplWait(fd, AE_READABLE, deadline) == C_ERR)
                    return -1;
            } while (raw == 2);
            if (raw != 1) return -1;
            continue;
        }
        return -1;
    }
    return (ssize_t)got;
}

/* Nonblocking read used by the RDB bulk transfer. Returns plaintext bytes,
 * 0 on EOF, -1 on error and -2 when the caller must wait for a new readable
 * event (which is queued here). */
int tlsReplReadSome(int fd, char *buf, int len) {
    int total = 0, raw;

    if (!tlsReplActive(fd)) {
        errno = ENOTCONN;
        return -1;
    }

    raw = tlsReplRawReadOnce(fd);
    if (raw < 0) return -1;

    while (total < len) {
        int n, err;
        ERR_clear_error();
        n = SSL_read(repl_ssl, buf + total, len - total);
        if (n > 0) {
            total += n;
            continue;
        }
        err = SSL_get_error(repl_ssl, n);
        if (err == SSL_ERROR_WANT_READ) break;
        if (err == SSL_ERROR_WANT_WRITE) {
            tlsReplDrainWbio();
            tlsReplFlushBestEffort(fd);
            break;
        }
        if (total > 0) break;
        return -1;
    }

    if (total > 0) return total;
    if (raw == 0) return 0;      /* orderly EOF */
#ifdef _WIN32
    WSIOCP_QueueNextRead(fd);
#endif
    return -2;
}

size_t tlsReplPending(int fd) {
    if (!tlsReplActive(fd)) return 0;
    return (size_t)SSL_pending(repl_ssl) +
           BIO_ctrl_pending(SSL_get_rbio(repl_ssl));
}

/* Best-effort newline used to keep the master from timing the replica out
 * while the received RDB is loaded. */
int tlsReplWriteNewline(int fd) {
    int n, err;

    if (!tlsReplActive(fd)) return -1;
    ERR_clear_error();
    n = SSL_write(repl_ssl, "\n", 1);
    tlsReplDrainWbio();
    tlsReplFlushBestEffort(fd);
    if (n == 1) return 0;
    err = SSL_get_error(repl_ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
    return -1;
}

/* Hand the established TLS session to the master client. A cached master may
 * still hold the session of the previous connection; drop that first. */
int tlsReplAdoptClient(client *c) {
    if (!repl_ssl) return C_ERR;
    tlsFreeClient(c);
    c->tls_ssl = repl_ssl;
    c->tls_handshaking = 0;
    c->tls_outbuf = repl_outbuf;
    c->flags |= CLIENT_TLS;
    repl_ssl = NULL;
    repl_outbuf = NULL;
    repl_fd = -1;
    return C_OK;
}

void tlsReplCleanup(void) {
    if (repl_ssl) {
        SSL_free(repl_ssl);
        repl_ssl = NULL;
    }
    if (repl_outbuf) {
        sdsfree(repl_outbuf);
        repl_outbuf = NULL;
    }
    repl_fd = -1;
}
