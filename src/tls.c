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
static SSL_CTX *tls_cluster_ctx = NULL;
static char tls_config_error[256];

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

const char *tlsLastConfigError(void) {
    return tls_config_error[0] ? tls_config_error : "Invalid TLS configuration";
}

static void tlsSetConfigError(const char *what, const char *value) {
    if (value)
        snprintf(tls_config_error,sizeof(tls_config_error),"%s: %s",what,value);
    else
        snprintf(tls_config_error,sizeof(tls_config_error),"%s",what);
}

/* Apply tls-protocols, tls-ciphers, tls-ciphersuites and the related options
 * to a fresh SSL_CTX. Returns C_ERR with tls_config_error set when a value
 * cannot be applied, so CONFIG SET can reject it and keep the old one. */
static int tlsApplySettings(SSL_CTX *ctx, int is_server) {
    if (server.tls_protocols && server.tls_protocols[0]) {
        sds *tokens;
        int count, j, min = 0, max = 0;

        tokens = sdssplitlen(server.tls_protocols,
                             (int)strlen(server.tls_protocols)," ",1,&count);
        for (j = 0; j < count; j++) {
            int version;

            if (!strcasecmp(tokens[j],"TLSv1")) version = TLS1_VERSION;
            else if (!strcasecmp(tokens[j],"TLSv1.1")) version = TLS1_1_VERSION;
            else if (!strcasecmp(tokens[j],"TLSv1.2")) version = TLS1_2_VERSION;
            else if (!strcasecmp(tokens[j],"TLSv1.3")) version = TLS1_3_VERSION;
            else {
                tlsSetConfigError("Invalid tls-protocols value",tokens[j]);
                sdsfreesplitres(tokens,count);
                return C_ERR;
            }
            if (!min || version < min) min = version;
            if (version > max) max = version;
        }
        sdsfreesplitres(tokens,count);
        if (SSL_CTX_set_min_proto_version(ctx,min) != 1 ||
            SSL_CTX_set_max_proto_version(ctx,max) != 1)
        {
            tlsSetConfigError("Unsupported tls-protocols value",
                              server.tls_protocols);
            return C_ERR;
        }
    } else {
        /* Historical floor of this port: TLS 1.2 unless configured. */
        SSL_CTX_set_min_proto_version(ctx,TLS1_2_VERSION);
    }

    if (server.tls_ciphers && server.tls_ciphers[0] &&
        SSL_CTX_set_cipher_list(ctx,server.tls_ciphers) != 1)
    {
        tlsSetConfigError("Invalid tls-ciphers value",server.tls_ciphers);
        return C_ERR;
    }
    if (server.tls_ciphersuites && server.tls_ciphersuites[0] &&
        SSL_CTX_set_ciphersuites(ctx,server.tls_ciphersuites) != 1)
    {
        tlsSetConfigError("Invalid tls-ciphersuites value",
                          server.tls_ciphersuites);
        return C_ERR;
    }

    if (is_server && server.tls_prefer_server_ciphers)
        SSL_CTX_set_options(ctx,SSL_OP_CIPHER_SERVER_PREFERENCE);
    else
        SSL_CTX_clear_options(ctx,SSL_OP_CIPHER_SERVER_PREFERENCE);

    SSL_CTX_set_session_cache_mode(ctx,server.tls_session_caching ?
        (is_server ? SSL_SESS_CACHE_SERVER : SSL_SESS_CACHE_CLIENT) :
        SSL_SESS_CACHE_OFF);
    SSL_CTX_set_mode(ctx,
        SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return C_OK;
}

static SSL_CTX *tlsBuildServerCtx(void) {
    SSL_CTX *ctx;

    if (server.tls_cert_file == NULL || server.tls_key_file == NULL) {
        tlsSetConfigError("tls-cert-file and tls-key-file are required",NULL);
        return NULL;
    }
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        tlsLogError("SSL_CTX_new");
        tlsSetConfigError("Cannot create TLS context",NULL);
        return NULL;
    }
    if (tlsApplySettings(ctx,1) == C_ERR) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx,server.tls_cert_file) != 1) {
        tlsLogError("SSL_CTX_use_certificate_chain_file");
        tlsSetConfigError("Cannot load tls-cert-file",server.tls_cert_file);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx,server.tls_key_file,
                                    SSL_FILETYPE_PEM) != 1) {
        tlsLogError("SSL_CTX_use_PrivateKey_file");
        tlsSetConfigError("Cannot load tls-key-file",server.tls_key_file);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        tlsLogError("SSL_CTX_check_private_key");
        tlsSetConfigError("tls-key-file does not match tls-cert-file",NULL);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (server.tls_auth_clients != TLS_CLIENT_AUTH_NO &&
        !(server.tls_ca_cert_file || server.tls_ca_cert_dir))
    {
        tlsSetConfigError(
            "tls-auth-clients yes or optional requires "
            "tls-ca-cert-file or tls-ca-cert-dir",NULL);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (server.tls_ca_cert_file || server.tls_ca_cert_dir) {
        if (SSL_CTX_load_verify_locations(ctx,server.tls_ca_cert_file,
                server.tls_ca_cert_dir) != 1)
        {
            tlsLogError("SSL_CTX_load_verify_locations");
            tlsSetConfigError("Cannot load TLS CA certificate",NULL);
            SSL_CTX_free(ctx);
            return NULL;
        }
    }
    if (server.tls_auth_clients == TLS_CLIENT_AUTH_YES) {
        /* Every client must present a certificate signed by our CA. */
        SSL_CTX_set_verify(ctx,
            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,NULL);
    } else if (server.tls_auth_clients == TLS_CLIENT_AUTH_OPTIONAL) {
        /* Request a certificate: a bad one fails the handshake, while a client
         * without a certificate is still accepted. */
        SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER,NULL);
    } else {
        SSL_CTX_set_verify(ctx,SSL_VERIFY_NONE,NULL);
    }
    return ctx;
}

int tlsInit(void) {
    if (tls_ctx) return C_OK;
    tls_ctx = tlsBuildServerCtx();
    if (!tls_ctx) {
        serverLog(LL_WARNING,"TLS configuration failed: %s",
                  tlsLastConfigError());
        return C_ERR;
    }
    return C_OK;
}

/* Rebuild the contexts after CONFIG SET changed a TLS option. Existing SSL
 * objects hold their own reference to the old context, so live connections
 * keep working and only new ones pick up the new settings. */
int tlsApplyConfig(void) {
    SSL_CTX *new_ctx = NULL;

    tls_config_error[0] = '\0';
    if (tls_ctx) {
        new_ctx = tlsBuildServerCtx();
        if (!new_ctx) return C_ERR;
    } else {
        /* TLS is not listening: still validate the values on a throwaway
         * context so a typo is reported now and not at the next restart. */
        SSL_CTX *probe = SSL_CTX_new(TLS_server_method());
        int ok = probe != NULL && tlsApplySettings(probe,1) == C_OK;

        SSL_CTX_free(probe);
        if (!ok) {
            if (tls_config_error[0] == '\0')
                tlsSetConfigError("Cannot create TLS context",NULL);
            return C_ERR;
        }
    }
    if (new_ctx) {
        SSL_CTX *old = tls_ctx;
        tls_ctx = new_ctx;
        SSL_CTX_free(old);
    }
    if (tls_client_ctx) {
        SSL_CTX *old = tls_client_ctx;
        tls_client_ctx = NULL;   /* Rebuilt lazily with the new settings. */
        SSL_CTX_free(old);
    }
    if (tls_cluster_ctx) {
        SSL_CTX *old = tls_cluster_ctx;
        tls_cluster_ctx = NULL;  /* Rebuilt lazily with the new settings. */
        SSL_CTX_free(old);
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
    if (tls_cluster_ctx) {
        SSL_CTX_free(tls_cluster_ctx);
        tls_cluster_ctx = NULL;
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
    if (tlsApplySettings(ctx,0) == C_ERR) {
        serverLog(LL_WARNING,"TLS client configuration failed: %s",
                  tlsLastConfigError());
        SSL_CTX_free(ctx);
        return NULL;
    }

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

/* -----------------------------------------------------------------------------
 * Cluster bus TLS (tls-cluster yes)
 *
 * Cluster links live in the event loop, so unlike the client and replication
 * paths every operation here is non-blocking. Ciphertext produced by OpenSSL
 * is queued in st->outbuf and flushed with write(); the cluster code registers
 * a writable event when the socket is not ready.
 * -------------------------------------------------------------------------- */

static void tlsClusterAppendOut(tlsLinkState *st, const char *buf, int len) {
    if (len <= 0) return;
    if (st->outbuf == NULL) st->outbuf = sdsempty();
    st->outbuf = sdscatlen(st->outbuf, buf, len);
}

static int tlsClusterDrainWbio(tlsLinkState *st) {
    char buf[16384];

    while (BIO_ctrl_pending((BIO*)st->wbio) > 0) {
        int n = BIO_read((BIO*)st->wbio, buf, sizeof(buf));
        if (n <= 0) break;
        tlsClusterAppendOut(st, buf, n);
    }
    return C_OK;
}

/* Every cluster node must present a certificate signed by the shared CA, so
 * the cluster bus uses its own server context regardless of tls-auth-clients:
 * that option describes regular clients, not other cluster nodes. */
static SSL_CTX *tlsGetClusterServerCtx(void) {
    SSL_CTX *ctx;

    if (tls_cluster_ctx) return tls_cluster_ctx;
    if (!server.tls_cert_file || !server.tls_key_file) {
        serverLog(LL_WARNING,
            "tls-cluster requires tls-cert-file and tls-key-file");
        return NULL;
    }
    if (!server.tls_ca_cert_file && !server.tls_ca_cert_dir) {
        serverLog(LL_WARNING,
            "tls-cluster requires tls-ca-cert-file or tls-ca-cert-dir");
        return NULL;
    }
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        tlsLogError("SSL_CTX_new cluster");
        return NULL;
    }
    if (tlsApplySettings(ctx,1) == C_ERR ||
        SSL_CTX_use_certificate_chain_file(ctx,server.tls_cert_file) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx,server.tls_key_file,
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1 ||
        SSL_CTX_load_verify_locations(ctx,server.tls_ca_cert_file,
                                      server.tls_ca_cert_dir) != 1)
    {
        tlsLogError("TLS cluster context");
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,NULL);
    tls_cluster_ctx = ctx;
    return ctx;
}

int tlsClusterLinkInit(tlsLinkState *st, int is_server) {
    SSL_CTX *ctx = is_server ? tlsGetClusterServerCtx() : tlsGetClientCtx();
    SSL *ssl;
    BIO *rbio, *wbio;

    memset(st, 0, sizeof(*st));
    if (!ctx) return C_ERR;
    ssl = SSL_new(ctx);
    if (!ssl) {
        tlsLogError("SSL_new cluster link");
        return C_ERR;
    }
    rbio = BIO_new(BIO_s_mem());
    wbio = BIO_new(BIO_s_mem());
    if (!rbio || !wbio) {
        BIO_free(rbio);
        BIO_free(wbio);
        SSL_free(ssl);
        tlsLogError("BIO_new cluster link");
        return C_ERR;
    }
    BIO_set_mem_eof_return(rbio, -1);
    SSL_set_bio(ssl, rbio, wbio);
    if (is_server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);
    st->ssl = ssl;
    st->rbio = rbio;
    st->wbio = wbio;
    st->handshaking = 1;
    return C_OK;
}

void tlsClusterLinkFree(tlsLinkState *st) {
    if (st->ssl) {
        SSL_free((SSL*)st->ssl);  /* Frees both memory BIOs. */
        st->ssl = NULL;
    }
    if (st->outbuf) {
        sdsfree(st->outbuf);
        st->outbuf = NULL;
    }
    st->rbio = st->wbio = NULL;
    st->handshaking = 0;
}

int tlsClusterPendingOut(tlsLinkState *st) {
    if (st->outbuf == NULL) return 0;
    return (int)sdslen(st->outbuf);
}

/* Write queued ciphertext. Returns 1 when everything reached the socket, 0
 * when the socket would block (a writable event must be registered) and -1 on
 * a fatal error. */
int tlsClusterFlush(tlsLinkState *st, int fd) {
    while (st->outbuf && sdslen(st->outbuf) > 0) {
        int n = (int)write(fd, st->outbuf, sdslen(st->outbuf));
        if (n > 0) {
            sdsrange(st->outbuf, n, -1);
            if (sdslen(st->outbuf) == 0) {
                sdsfree(st->outbuf);
                st->outbuf = NULL;
            }
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == WSAEWOULDBLOCK)) return 0;
        return -1;
    }
    return 1;
}

/* Feed encrypted bytes from the socket into the read BIO.
 * Returns 1 for data, 0 for EOF, 2 for "would block" and -1 on error. */
static int tlsClusterReadRaw(tlsLinkState *st, int fd) {
    char buf[16384];
    int n = (int)read(fd, buf, sizeof(buf));

    if (n > 0) {
        if (BIO_write((BIO*)st->rbio, buf, n) != n) {
            errno = ECONNRESET;
            return -1;
        }
        return 1;
    }
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == WSAEWOULDBLOCK) return 2;
    return -1;
}

/* Drive the handshake. Returns 1 when complete, 0 when more I/O is needed
 * (check tlsClusterPendingOut() and register a writable event if non-zero),
 * and -1 on a fatal error. */
int tlsClusterHandshake(tlsLinkState *st, int fd) {
    SSL *ssl = (SSL*)st->ssl;
    int ret, err;

    if (!ssl) return -1;
    if (!st->handshaking) return 1;
    for (;;) {
        ERR_clear_error();
        ret = SSL_do_handshake(ssl);
        tlsClusterDrainWbio(st);
        if (tlsClusterFlush(st, fd) < 0) return -1;
        if (ret == 1) {
            st->handshaking = 0;
            return 1;
        }
        err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            int raw = tlsClusterReadRaw(st, fd);
            if (raw == 1) continue;
            if (raw == 2) return 0;
            return -1;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (tlsClusterFlush(st, fd) == 0) return 0;
            continue;
        }
        tlsLogError("TLS cluster handshake");
        return -1;
    }
}

/* Encrypt and write plaintext. Returns the number of plaintext bytes consumed
 * (which may be less than len, or zero when the socket is not writable yet) or
 * -1 on a fatal error. */
int tlsClusterWrite(tlsLinkState *st, int fd, const void *buf, size_t len) {
    SSL *ssl = (SSL*)st->ssl;
    int ret, err;

    if (!ssl) {
        errno = ECONNRESET;
        return -1;
    }
    if (st->handshaking) {
        int hs = tlsClusterHandshake(st, fd);
        if (hs < 0) return -1;
        if (hs == 0) return 0;
    }
    if (tlsClusterPendingOut(st) && tlsClusterFlush(st, fd) != 1)
        return (errno == EAGAIN || errno == WSAEWOULDBLOCK) ? 0 : -1;
    if (len == 0) return 0;

    for (;;) {
        ERR_clear_error();
        ret = SSL_write(ssl, buf, len);
        tlsClusterDrainWbio(st);
        if (tlsClusterFlush(st, fd) < 0) return -1;
        if (ret > 0) return ret;
        err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_WRITE) return 0;
        if (err == SSL_ERROR_WANT_READ) {
            int raw = tlsClusterReadRaw(st, fd);
            if (raw == 1) continue;
            if (raw == 2) return 0;
        }
        tlsLogError("SSL_write cluster link");
        return -1;
    }
}

/* Read plaintext. Returns bytes read, 0 on EOF, or -1 with errno set to
 * EAGAIN when no complete plaintext is available yet. */
ssize_t tlsClusterRead(tlsLinkState *st, int fd, void *buf, size_t len) {
    SSL *ssl = (SSL*)st->ssl;
    int ret, err, raw = 0;

    if (!ssl) {
        errno = ECONNRESET;
        return -1;
    }
    if (st->handshaking) {
        int hs = tlsClusterHandshake(st, fd);
        if (hs < 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (hs == 0) {
            errno = EAGAIN;
            return -1;
        }
    }
    if (len == 0) return 0;

    for (;;) {
        ERR_clear_error();
        ret = SSL_read(ssl, buf, len);
        tlsClusterDrainWbio(st);
        if (tlsClusterFlush(st, fd) < 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (ret > 0) return ret;
        err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            if (raw) {
                errno = EAGAIN;
                return -1;
            }
            raw = tlsClusterReadRaw(st, fd);
            if (raw == 1) continue;
            if (raw == 2) {
                errno = EAGAIN;
                return -1;
            }
            return 0; /* Orderly shutdown. */
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (tlsClusterFlush(st, fd) == 0) {
                errno = EAGAIN;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        tlsLogError("SSL_read cluster link");
        errno = ECONNRESET;
        return -1;
    }
}
