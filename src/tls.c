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
