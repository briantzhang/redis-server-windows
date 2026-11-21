/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
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

#include "fmacros.h"
#include <string.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <assert.h>
#include <errno.h>
#include <ctype.h>

#include "hiredis.h"
#include "net.h"
#include "sds.h"

#ifdef _WIN32
#include "win32_hiredis.h"
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>

static redisReply *createReplyObject(int type);
static void *createStringObject(const redisReadTask *task, char *str, size_t len);
static void *createArrayObject(const redisReadTask *task, int elements);
static void *createIntegerObject(const redisReadTask *task, PORT_LONGLONG value);
static void *createNilObject(const redisReadTask *task);

/* Default set of functions to build the reply. Keep in mind that such a
 * function returning NULL is interpreted as OOM. */
static redisReplyObjectFunctions defaultFunctions = {
	createStringObject,
	createArrayObject,
	createIntegerObject,
	createNilObject,
	freeReplyObject
};

/* Create a reply object */
static redisReply *createReplyObject(int type) {
	redisReply *r = calloc(1, sizeof(*r));

	if (r == NULL)
		return NULL;

	r->type = type;
	return r;
}

/* Free a reply object */
void freeReplyObject(void *reply) {
	redisReply *r = reply;
	size_t j;

	if (r == NULL)
		return;

	switch (r->type) {
	case REDIS_REPLY_INTEGER:
		break; /* Nothing to free */
	case REDIS_REPLY_ARRAY:
		if (r->element != NULL) {
			for (j = 0; j < r->elements; j++)
				if (r->element[j] != NULL)
					freeReplyObject(r->element[j]);
			free(r->element);
		}
		break;
	case REDIS_REPLY_ERROR:
	case REDIS_REPLY_STATUS:
	case REDIS_REPLY_STRING:
		if (r->str != NULL)
			free(r->str);
		break;
	}
	free(r);
}

static void *createStringObject(const redisReadTask *task, char *str, size_t len) {
	redisReply *r, *parent;
	char *buf;

	r = createReplyObject(task->type);
	if (r == NULL)
		return NULL;

	buf = malloc(len + 1);
	if (buf == NULL) {
		freeReplyObject(r);
		return NULL;
	}

	assert(task->type == REDIS_REPLY_ERROR ||
		task->type == REDIS_REPLY_STATUS ||
		task->type == REDIS_REPLY_STRING);

	/* Copy string value */
	memcpy(buf, str, len);
	buf[len] = '\0';
	r->str = buf;
	r->len = (int)len;

	if (task->parent) {
		parent = task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}
	return r;
}

static void *createArrayObject(const redisReadTask *task, int elements) {
	redisReply *r, *parent;

	r = createReplyObject(REDIS_REPLY_ARRAY);
	if (r == NULL)
		return NULL;

	if (elements > 0) {
        if (SIZE_MAX / sizeof(redisReply*) < elements) return NULL;  /* Don't overflow */
		r->element = calloc(elements, sizeof(redisReply*));
		if (r->element == NULL) {
			freeReplyObject(r);
			return NULL;
		}
	}

	r->elements = elements;

	if (task->parent) {
		parent = task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}
	return r;
}

static void *createIntegerObject(const redisReadTask *task, PORT_LONGLONG value) {
	redisReply *r, *parent;

	r = createReplyObject(REDIS_REPLY_INTEGER);
	if (r == NULL)
		return NULL;

	r->integer = value;

	if (task->parent) {
		parent = task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}
	return r;
}

static void *createNilObject(const redisReadTask *task) {
	redisReply *r, *parent;

	r = createReplyObject(REDIS_REPLY_NIL);
	if (r == NULL)
		return NULL;

	if (task->parent) {
		parent = task->parent->obj;
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element[task->idx] = r;
	}
	return r;
}

/* Return the number of digits of 'v' when converted to string in radix 10.
 * Implementation borrowed from link in redis/src/util.c:string2ll(). */
static uint32_t countDigits(uint64_t v) {
	uint32_t result = 1;
	for (;;) {
		if (v < 10) return result;
		if (v < 100) return result + 1;
		if (v < 1000) return result + 2;
		if (v < 10000) return result + 3;
		v /= 10000U;
		result += 4;
	}
}

/* Helper that calculates the bulk length given a certain string length. */
static size_t bulklen(size_t len) {
	return (size_t)(1 + countDigits(len) + 2 + len + 2);
}

int redisvFormatCommand(char **target, const char *format, va_list ap) {
	const char *c = format;
	char *cmd = NULL; /* final command */
	int pos; /* position in final command */
	sds curarg, newarg; /* current argument */
	int touched = 0; /* was the current argument touched? */
	char **curargv = NULL, **newargv = NULL;
	int argc = 0;
	int totlen = 0;
	int error_type = 0; /* 0 = no error; -1 = memory error; -2 = format error */
	int j;

	/* Abort if there is not target to set */
	if (target == NULL)
		return -1;

	/* Build the command string accordingly to protocol */
	curarg = sdsempty();
	if (curarg == NULL)
		return -1;

	while (*c != '\0') {
		if (*c != '%' || c[1] == '\0') {
			if (*c == ' ') {
				if (touched) {
					newargv = realloc(curargv, sizeof(char*)*(argc + 1));
					if (newargv == NULL) goto memory_err;
					curargv = newargv;
					curargv[argc++] = curarg;
					totlen += (int)bulklen(sdslen(curarg));

					/* curarg is put in argv so it can be overwritten. */
					curarg = sdsempty();
					if (curarg == NULL) goto memory_err;
					touched = 0;
				}
			}
			else {
				newarg = sdscatlen(curarg, c, 1);
				if (newarg == NULL) goto memory_err;
				curarg = newarg;
				touched = 1;
			}
		}
		else {
			char *arg;
			size_t size;

			/* Set newarg so it can be checked even if it is not touched. */
			newarg = curarg;

			switch (c[1]) {
			case 's':
				arg = va_arg(ap, char*);
				size = strlen(arg);
				if (size > 0)
					newarg = sdscatlen(curarg, arg, size);
				break;
			case 'b':
				arg = va_arg(ap, char*);
				size = va_arg(ap, size_t);
				if (size > 0)
					newarg = sdscatlen(curarg, arg, size);
				break;
			case '%':
				newarg = sdscat(curarg, "%");
				break;
			default:
				/* Try to detect printf format */
			{
				static const char intfmts[] = "diouxX";
				static const char flags[] = "#0-+ ";
				char _format[16];
				const char *_p = c + 1;
				size_t _l = 0;
				va_list _cpy;

				/* Flags */
				while (*_p != '\0' && strchr(flags, *_p) != NULL) _p++;

				/* Field width */
				while (*_p != '\0' && isdigit(*_p)) _p++;

				/* Precision */
				if (*_p == '.') {
					_p++;
					while (*_p != '\0' && isdigit(*_p)) _p++;
				}

				/* Copy va_list before consuming with va_arg */
				va_copy(_cpy, ap);

				/* Integer conversion (without modifiers) */
				if (strchr(intfmts, *_p) != NULL) {
					va_arg(ap, int);
					goto fmt_valid;
				}

				/* Double conversion (without modifiers) */
				if (strchr("eEfFgGaA", *_p) != NULL) {
					va_arg(ap, double);
					goto fmt_valid;
				}

				/* Size: char */
				if (_p[0] == 'h' && _p[1] == 'h') {
					_p += 2;
					if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
						va_arg(ap, int); /* char gets promoted to int */
						goto fmt_valid;
					}
					goto fmt_invalid;
				}

				/* Size: short */
				if (_p[0] == 'h') {
					_p += 1;
					if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
						va_arg(ap, int); /* short gets promoted to int */
						goto fmt_valid;
					}
					goto fmt_invalid;
				}

				/* Size: PORT_LONGLONG */
				if (_p[0] == 'l' && _p[1] == 'l') {
					_p += 2;
					if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
						va_arg(ap, PORT_LONGLONG);
						goto fmt_valid;
					}
					goto fmt_invalid;
				}

				/* Size: PORT_LONG */
				if (_p[0] == 'l') {
					_p += 1;
					if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
						va_arg(ap, PORT_LONG);
						goto fmt_valid;
					}
					goto fmt_invalid;
				}

			fmt_invalid:
				va_end(_cpy);
				goto format_err;

			fmt_valid:
				_l = (_p + 1) - c;
				if (_l < sizeof(_format) - 2) {
					memcpy(_format, c, _l);
					_format[_l] = '\0';
					newarg = sdscatvprintf(curarg, _format, _cpy);

					/* Update current position (note: outer blocks
					 * increment c twice so compensate here) */
					c = _p - 1;
				}

				va_end(_cpy);
				break;
			}
			}

			if (newarg == NULL) goto memory_err;
			curarg = newarg;

			touched = 1;
			c++;
		}
		c++;
	}

	/* Add the last argument if needed */
	if (touched) {
		newargv = realloc(curargv, sizeof(char*)*(argc + 1));
		if (newargv == NULL) goto memory_err;
		curargv = newargv;
		curargv[argc++] = curarg;
		totlen += (int)bulklen(sdslen(curarg));
	}	else {
		sdsfree(curarg);
	}

	/* Clear curarg because it was put in curargv or was free'd. */
	curarg = NULL;

	/* Add bytes needed to hold multi bulk count */
	totlen += 1 + countDigits(argc) + 2;

	/* Build the command at protocol level */
	cmd = malloc(totlen + 1);
	if (cmd == NULL) goto memory_err;

	pos = sprintf(cmd, "*%d\r\n", argc);
	for (j = 0; j < argc; j++) {
		pos += sprintf(cmd + pos, "$%Iu\r\n", sdslen(curargv[j]));                  WIN_PORT_FIX /* %zu -> %Iu */
			memcpy(cmd + pos, curargv[j], sdslen(curargv[j]));
		pos += (int)sdslen(curargv[j]);
		sdsfree(curargv[j]);
		cmd[pos++] = '\r';
		cmd[pos++] = '\n';
	}
	assert(pos == totlen);
	cmd[pos] = '\0';

	free(curargv);
	*target = cmd;
	return totlen;

format_err:
	error_type = -2;
	goto cleanup;

memory_err:
	error_type = -1;
	goto cleanup;

cleanup:
	if (curargv) {
		while (argc--)
			sdsfree(curargv[argc]);
		free(curargv);
	}

	sdsfree(curarg);

	/* No need to check cmd since it is the last statement that can fail,
	 * but do it anyway to be as defensive as possible. */
	if (cmd != NULL)
		free(cmd);

	return error_type;
}

/* Format a command according to the Redis protocol. This function
 * takes a format similar to printf:
 *
 * %s represents a C null terminated string you want to interpolate
 * %b represents a binary safe string
 *
 * When using %b you need to provide both the pointer to the string
 * and the length in bytes as a size_t. Examples:
 *
 * len = redisFormatCommand(target, "GET %s", mykey);
 * len = redisFormatCommand(target, "SET %s %b", mykey, myval, myvallen);
 */
int redisFormatCommand(char **target, const char *format, ...) {
	va_list ap;
	int len;
	va_start(ap, format);
	len = redisvFormatCommand(target, format, ap);
	va_end(ap);

	/* The API says "-1" means bad result, but we now also return "-2" in some
	 * cases.  Force the return value to always be -1. */
	if (len < 0)
		len = -1;

	return len;
}

/* Format a command according to the Redis protocol using an sds string and
 * sdscatfmt for the processing of arguments. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
int redisFormatSdsCommandArgv(sds *target, int argc, const char **argv,
	const size_t *argvlen)
{
	sds cmd;
	PORT_ULONGLONG totlen;
	int j;
	size_t len;

	/* Abort on a NULL target */
	if (target == NULL)
		return -1;

	/* Calculate our total size */
	totlen = 1 + countDigits(argc) + 2;
	for (j = 0; j < argc; j++) {
		len = argvlen ? argvlen[j] : strlen(argv[j]);
		totlen += bulklen(len);
	}

	/* Use an SDS string for command construction */
	cmd = sdsempty();
	if (cmd == NULL)
		return -1;

	/* We already know how much storage we need */
	cmd = sdsMakeRoomFor(cmd, totlen);
	if (cmd == NULL)
		return -1;

	/* Construct command */
	cmd = sdscatfmt(cmd, "*%i\r\n", argc);
	for (j = 0; j < argc; j++) {
		len = argvlen ? argvlen[j] : strlen(argv[j]);
		cmd = sdscatfmt(cmd, "$%u\r\n", len);
		cmd = sdscatlen(cmd, argv[j], len);
		cmd = sdscatlen(cmd, "\r\n", sizeof("\r\n") - 1);
	}

	assert(sdslen(cmd) == totlen);

	*target = cmd;
	return totlen;
}

void redisFreeSdsCommand(sds cmd) {
	sdsfree(cmd);
}

/* Format a command according to the Redis protocol. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
int redisFormatCommandArgv(char **target, int argc, const char **argv, const size_t *argvlen) {
	char *cmd = NULL; /* final command */
	int pos; /* position in final command */
	size_t len;
	int totlen, j;

	/* Abort on a NULL target */
	if (target == NULL)
		return -1;

	/* Calculate number of bytes needed for the command */
	totlen = 1 + countDigits(argc) + 2;
	for (j = 0; j < argc; j++) {
		len = argvlen ? argvlen[j] : strlen(argv[j]);
		totlen += bulklen(len);
	}

	/* Build the command at protocol level */
	cmd = malloc(totlen + 1);
	if (cmd == NULL)
		return -1;

	pos = sprintf(cmd, "*%d\r\n", argc);
	for (j = 0; j < argc; j++) {
		len = argvlen ? argvlen[j] : strlen(argv[j]);
		pos += sprintf(cmd + pos, "$%Iu\r\n", len);                                 WIN_PORT_FIX /* %zu -> %Iu */
			memcpy(cmd + pos, argv[j], len);
		pos += (int)len;
		cmd[pos++] = '\r';
		cmd[pos++] = '\n';
	}
	assert(pos == totlen);
	cmd[pos] = '\0';

	*target = cmd;
	return totlen;
}

void redisFreeCommand(char *cmd) {
	free(cmd);
}

void __redisSetError(redisContext *c, int type, const char *str) {
	size_t len;

	c->err = type;
	if (str != NULL) {
		len = strlen(str);
		len = len < (sizeof(c->errstr) - 1) ? len : (sizeof(c->errstr) - 1);
		memcpy(c->errstr, str, len);
		c->errstr[len] = '\0';
    } else {
		/* Only REDIS_ERR_IO may lack a description! */
		assert(type == REDIS_ERR_IO);
		__redis_strerror_r(errno, c->errstr, sizeof(c->errstr));
	}
}

redisReader *redisReaderCreate(void) {
	return redisReaderCreateWithFunctions(&defaultFunctions);
}

static char *redisTlsDupString(const char *s) {
	size_t len = strlen(s) + 1;
	char *copy = malloc(len);
	if (copy != NULL)
		memcpy(copy, s, len);
	return copy;
}

static void redisTlsSetError(redisContext *c, int type, const char *prefix) {
	char buf[256];
	unsigned long err = ERR_get_error();
	size_t len;

	if (err == 0 && type != REDIS_ERR_SSL) {
		__redisSetError(c, type, prefix);
		return;
	}

	buf[0] = '\0';
	if (err != 0)
		ERR_error_string_n(err, buf, sizeof(buf));
	len = strlen(buf);
	if (prefix != NULL && len + 2 < sizeof(buf)) {
		memmove(buf + strlen(prefix) + 2, buf, len + 1);
		memcpy(buf, prefix, strlen(prefix));
		memcpy(buf + strlen(prefix), ": ", 2);
	}
	__redisSetError(c, type, buf);
}

void redisTlsCleanup(redisContext *c) {
	SSL *ssl = (SSL *)c->tls_ssl;

	if (ssl != NULL) {
		SSL_free(ssl);
		c->tls_ssl = NULL;
		c->tls_in_bio = NULL;
		c->tls_out_bio = NULL;
	}
	if (c->tls_ctx != NULL) {
		SSL_CTX_free((SSL_CTX *)c->tls_ctx);
		c->tls_ctx = NULL;
	}
}

static ssize_t redisTlsRawWrite(redisContext *c, const void *buf, size_t len) {
	return write(c->fd, buf, len);
}

static int redisTlsDrainOutput(redisContext *c) {
	BIO *out = (BIO *)c->tls_out_bio;
	char *data;
	long pending;
	ssize_t nwritten, total;

	while ((pending = BIO_get_mem_data(out, &data)) > 0) {
		total = redisTlsRawWrite(c, data, (size_t)pending);
		if (total <= 0) {
			__redisSetError(c, REDIS_ERR_IO, NULL);
			return REDIS_ERR;
		}
		nwritten = total;
		while (nwritten < pending) {
			total = redisTlsRawWrite(c, data + nwritten, (size_t)(pending - nwritten));
			if (total <= 0) {
				__redisSetError(c, REDIS_ERR_IO, NULL);
				return REDIS_ERR;
			}
			nwritten += total;
		}
		BIO_reset(out);
	}
	return REDIS_OK;
}

int redisTlsHandshake(redisContext *c) {
	SSL *ssl = (SSL *)c->tls_ssl;
	BIO *in = (BIO *)c->tls_in_bio;
	char buf[1024 * 16];
	int rc, sslerr, nread, i;

	SSL_set_connect_state(ssl);
	for (i = 0; i < 32; i++) {
		rc = SSL_connect(ssl);
		if (redisTlsDrainOutput(c) == REDIS_ERR)
			return REDIS_ERR;
		if (rc == 1)
			return REDIS_OK;

		sslerr = SSL_get_error(ssl, rc);
		if (sslerr == SSL_ERROR_WANT_WRITE)
			continue;
		if (sslerr != SSL_ERROR_WANT_READ) {
			redisTlsSetError(c, REDIS_ERR_SSL, "TLS handshake failed");
			return REDIS_ERR;
		}

		nread = (int)read(c->fd, buf, sizeof(buf));
		if (nread > 0) {
			BIO_write(in, buf, nread);
			continue;
		}
		if (nread == 0 || (nread < 0 && errno != EAGAIN && errno != EINTR)) {
			__redisSetError(c, nread == 0 ? REDIS_ERR_EOF : REDIS_ERR_IO,
				nread == 0 ? "Server closed the connection" : NULL);
			return REDIS_ERR;
		}
	}
	__redisSetError(c, REDIS_ERR_OTHER, "TLS handshake timed out");
	return REDIS_ERR;
}

int redisTlsSetup(redisContext *c, const char *host) {
	SSL_CTX *ctx;
	SSL *ssl;
	BIO *in, *out;
	const char *verify_host;

	ctx = SSL_CTX_new(TLS_client_method());
	if (ctx == NULL) {
		redisTlsSetError(c, REDIS_ERR_SSL, "TLS context creation failed");
		return REDIS_ERR;
	}
	c->tls_ctx = ctx;

	if (c->tls_verify) {
		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
		if (c->tls_ca_cert != NULL) {
			if (SSL_CTX_load_verify_locations(ctx, c->tls_ca_cert, NULL) != 1) {
				redisTlsSetError(c, REDIS_ERR_SSL, "Could not load CA certificate");
				return REDIS_ERR;
			}
		} else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
			redisTlsSetError(c, REDIS_ERR_SSL, "Could not load default CA certificates");
			return REDIS_ERR;
		}
	} else {
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	}

	if (c->tls_cert != NULL) {
		if (SSL_CTX_use_certificate_chain_file(ctx, c->tls_cert) != 1) {
			redisTlsSetError(c, REDIS_ERR_SSL, "Could not load client certificate");
			return REDIS_ERR;
		}
		if (c->tls_key == NULL)
			c->tls_key = redisTlsDupString(c->tls_cert);
		if (SSL_CTX_use_PrivateKey_file(ctx, c->tls_key, SSL_FILETYPE_PEM) != 1) {
			redisTlsSetError(c, REDIS_ERR_SSL, "Could not load client private key");
			return REDIS_ERR;
		}
		if (SSL_CTX_check_private_key(ctx) != 1) {
			redisTlsSetError(c, REDIS_ERR_SSL, "Client certificate and key do not match");
			return REDIS_ERR;
		}
	}

	ssl = SSL_new(ctx);
	if (ssl == NULL) {
		redisTlsSetError(c, REDIS_ERR_SSL, "TLS connection creation failed");
		return REDIS_ERR;
	}
	c->tls_ssl = ssl;

	in = BIO_new(BIO_s_mem());
	out = BIO_new(BIO_s_mem());
	if (in == NULL || out == NULL) {
		redisTlsSetError(c, REDIS_ERR_SSL, "TLS BIO creation failed");
		return REDIS_ERR;
	}
	c->tls_in_bio = in;
	c->tls_out_bio = out;
	SSL_set_bio(ssl, in, out);

	verify_host = (c->tls_host != NULL) ? c->tls_host : host;
	if (verify_host != NULL && verify_host[0] != '\0') {
		SSL_set_tlsext_host_name(ssl, verify_host);
		if (c->tls_verify)
			SSL_set1_host(ssl, verify_host);
	}
	return REDIS_OK;
}

void redisContextInitTlsStrings(redisContext *c, const char *host,
	const char *ca_cert, const char *cert, const char *key, int verify)
{
	if (host != NULL) c->tls_host = redisTlsDupString(host);
	if (ca_cert != NULL) c->tls_ca_cert = redisTlsDupString(ca_cert);
	if (cert != NULL) c->tls_cert = redisTlsDupString(cert);
	if (key != NULL) c->tls_key = redisTlsDupString(key);
	c->tls_verify = verify;
}

static redisContext *redisContextInit(void) {
	redisContext *c;

	c = calloc(1, sizeof(redisContext));
	if (c == NULL)
		return NULL;

	c->err = 0;
	c->errstr[0] = '\0';
	c->fd = -1;
	c->obuf = sdsempty();
	c->reader = redisReaderCreate();
	c->tcp.host = NULL;
	c->tcp.source_addr = NULL;
	c->unix_sock.path = NULL;
	c->timeout = NULL;

	if (c->obuf == NULL || c->reader == NULL) {
		redisFree(c);
		return NULL;
	}

	return c;
}

void redisFree(redisContext *c) {
	if (c == NULL)
		return;
	redisTlsCleanup(c);
	if (c->fd >= 0)
		close(c->fd);
	if (c->obuf != NULL)
		sdsfree(c->obuf);
	if (c->reader != NULL)
		redisReaderFree(c->reader);
	if (c->tcp.host)
		free(c->tcp.host);
	if (c->tcp.source_addr)
		free(c->tcp.source_addr);
	if (c->unix_sock.path)
		free(c->unix_sock.path);
	if (c->timeout)
		free(c->timeout);
	if (c->tls_host)
		free(c->tls_host);
	if (c->tls_ca_cert)
		free(c->tls_ca_cert);
	if (c->tls_cert)
		free(c->tls_cert);
	if (c->tls_key)
		free(c->tls_key);
	free(c);
}

int redisFreeKeepFd(redisContext *c) {
	int fd = c->fd;
	c->fd = -1;
	redisFree(c);
	return fd;
}

int redisReconnect(redisContext *c) {
	int rc;

	c->err = 0;
	memset(c->errstr, '\0', strlen(c->errstr));

	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	redisTlsCleanup(c);

	sdsfree(c->obuf);
	redisReaderFree(c->reader);

	c->obuf = sdsempty();
	c->reader = redisReaderCreate();

	if (c->connection_type == REDIS_CONN_TCP) {
		rc = redisContextConnectBindTcp(c, c->tcp.host, c->tcp.port,
			c->timeout, c->tcp.source_addr);
    } else if (c->connection_type == REDIS_CONN_UNIX) {
		rc = redisContextConnectUnix(c, c->unix_sock.path, c->timeout);
    } else {
		/* Something bad happened here and shouldn't have. There isn't
		   enough information in the context to reconnect. */
		__redisSetError(c, REDIS_ERR_OTHER, "Not enough information to reconnect");
		return REDIS_ERR;
	}
	if (rc == REDIS_ERR || !(c->flags & REDIS_TLS))
		return rc;

	if (redisTlsSetup(c, c->tcp.host) == REDIS_ERR)
		return REDIS_ERR;
	return redisTlsHandshake(c);
}

/* Connect to a Redis instance. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
redisContext *redisConnect(const char *ip, int port) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags |= REDIS_BLOCK;
	redisContextConnectTcp(c, ip, port, NULL);
	return c;
}

redisContext *redisConnectTLS(const char *ip, int port, const char *ca_cert,
	const char *cert, const char *key, const char *sni, int verify)
{
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags |= REDIS_BLOCK | REDIS_TLS;
	redisContextInitTlsStrings(c, sni, ca_cert, cert, key, verify);
	redisContextConnectTcp(c, ip, port, NULL);
	if (c->err)
		return c;
	if (redisTlsSetup(c, ip) == REDIS_ERR)
		return c;
	redisTlsHandshake(c);
	return c;
}

redisContext *redisConnectWithTimeout(const char *ip, int port, const struct timeval tv) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags |= REDIS_BLOCK;
	redisContextConnectTcp(c, ip, port, &tv);
	return c;
}

redisContext *redisConnectNonBlock(const char *ip, int port) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags &= ~REDIS_BLOCK;
	redisContextConnectTcp(c, ip, port, NULL);
	return c;
}

redisContext *redisConnectBindNonBlock(const char *ip, int port,
	const char *source_addr) {
	redisContext *c = redisContextInit();
	c->flags &= ~REDIS_BLOCK;
	redisContextConnectBindTcp(c, ip, port, NULL, source_addr);
	return c;
}

redisContext *redisConnectBindNonBlockWithReuse(const char *ip, int port,
	const char *source_addr) {
	redisContext *c = redisContextInit();
	c->flags &= ~REDIS_BLOCK;
	c->flags |= REDIS_REUSEADDR;
	redisContextConnectBindTcp(c, ip, port, NULL, source_addr);
	return c;
}

redisContext *redisConnectUnix(const char *path) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags |= REDIS_BLOCK;
	redisContextConnectUnix(c, path, NULL);
	return c;
}

redisContext *redisConnectUnixWithTimeout(const char *path, const struct timeval tv) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags |= REDIS_BLOCK;
	redisContextConnectUnix(c, path, &tv);
	return c;
}

redisContext *redisConnectUnixNonBlock(const char *path) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->flags &= ~REDIS_BLOCK;
	redisContextConnectUnix(c, path, NULL);
	return c;
}

redisContext *redisConnectFd(int fd) {
	redisContext *c;

	c = redisContextInit();
	if (c == NULL)
		return NULL;

	c->fd = fd;
	c->flags |= REDIS_BLOCK | REDIS_CONNECTED;
	return c;
}

#ifdef _WIN32
redisContext *redisPreConnectNonBlock(const char *ip, int port, SOCKADDR_STORAGE *ss) {
	redisContext *c = redisContextInit();
	c->fd = -1;
	c->flags &= ~REDIS_BLOCK;
	redisContextPreConnectTcp(c, ip, port, NULL, ss);
	return c;
}
#endif

/* Set read/write timeout on a blocking socket. */
int redisSetTimeout(redisContext *c, const struct timeval tv) {
	if (c->flags & REDIS_BLOCK)
		return redisContextSetTimeout(c, tv);
	return REDIS_ERR;
}

/* Enable connection KeepAlive. */
int redisEnableKeepAlive(redisContext *c) {
	if (redisKeepAlive(c, REDIS_KEEPALIVE_INTERVAL) != REDIS_OK)
		return REDIS_ERR;
	return REDIS_OK;
}

/* Use this function to handle a read event on the descriptor. It will try
 * and read some bytes from the socket and feed them to the reply parser.
 *
 * After this function is called, you may use redisContextReadReply to
 * see if there is a reply available. */
int redisBufferRead(redisContext *c) {
	char buf[1024 * 16];
	int nread, sslrc, sslerr, i, got;

	/* Return early when the context has seen an error. */
	if (c->err)
		return REDIS_ERR;

	if (c->flags & REDIS_TLS) {
		SSL *ssl = (SSL *)c->tls_ssl;
		BIO *in = (BIO *)c->tls_in_bio;

		if (ssl == NULL || in == NULL) {
			__redisSetError(c, REDIS_ERR_OTHER, "TLS connection is not initialized");
			return REDIS_ERR;
		}
		for (i = 0; i < 32; i++) {
			sslrc = SSL_read(ssl, buf, sizeof(buf));
			if (sslrc > 0) {
				if (redisReaderFeed(c->reader, buf, sslrc) != REDIS_OK) {
					__redisSetError(c, c->reader->err, c->reader->errstr);
					return REDIS_ERR;
				}
				/* One decrypted record is enough for this read cycle, just as
				 * the plaintext path returns after a single socket read. The
				 * blocking command loop calls us again when it needs more. */
				sslerr = SSL_ERROR_NONE;
				break;
			}
			sslerr = SSL_get_error(ssl, sslrc);
			if (sslerr == SSL_ERROR_WANT_WRITE) {
				if (redisTlsDrainOutput(c) == REDIS_ERR)
					return REDIS_ERR;
				continue;
			}
			if (sslerr != SSL_ERROR_WANT_READ)
				break;

			nread = (int)read(c->fd, buf, sizeof(buf));
			if (nread > 0) {
				BIO_write(in, buf, nread);
				continue;
			}
			if (nread == 0) {
				__redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
				return REDIS_ERR;
			}
			if (errno != EAGAIN && errno != EINTR) {
				__redisSetError(c, REDIS_ERR_IO, NULL);
				return REDIS_ERR;
			}
			break;
		}
		got = (sslrc > 0) ? REDIS_OK : REDIS_ERR;
		if (got == REDIS_ERR) {
			if (sslerr == SSL_ERROR_ZERO_RETURN)
				__redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
			else
				redisTlsSetError(c, REDIS_ERR_SSL, "TLS read failed");
		}
		return got;
	}

	nread = (int)read(c->fd, buf, sizeof(buf));                                   WIN_PORT_FIX /* cast (int) */
		if (nread == -1) {
			if ((errno == EAGAIN && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
				/* Try again later */
			}
			else {
				__redisSetError(c, REDIS_ERR_IO, NULL);
				return REDIS_ERR;
			}
		}
		else if (nread == 0) {
			__redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
			return REDIS_ERR;
		}
		else {
			if (redisReaderFeed(c->reader, buf, nread) != REDIS_OK) {
				__redisSetError(c, c->reader->err, c->reader->errstr);
				return REDIS_ERR;
			}
		}
		return REDIS_OK;
}

#ifdef _WIN32
/* Use this function if the caller has already read the data. It will
 * feed bytes to the reply parser.
 *
 * After this function is called, you may use redisContextReadReply to
 * see if there is a reply available. */
int redisBufferReadDone(redisContext *c, char *buf, ssize_t nread) {
	if (nread == -1) {
		if (errno == EAGAIN && !(c->flags & REDIS_BLOCK)) {
			/* Try again later */
		}
		else {
			__redisSetError(c, REDIS_ERR_IO, NULL);
			return REDIS_ERR;
		}
	}
	else if (nread == 0) {
		__redisSetError(c, REDIS_ERR_EOF, sdsnew("Server closed the connection"));
		return REDIS_ERR;
	}
	else {
		if (redisReaderFeed(c->reader, buf, nread) != REDIS_OK) {
			__redisSetError(c, c->reader->err, c->reader->errstr);
			return REDIS_ERR;
		}
	}
	return REDIS_OK;
}
#endif

/* Write the output buffer to the socket.
 *
 * Returns REDIS_OK when the buffer is empty, or (a part of) the buffer was
 * successfully written to the socket. When the buffer is empty after the
 * write operation, "done" is set to 1 (if given).
 *
 * Returns REDIS_ERR if an error occurred trying to write and sets
 * c->errstr to hold the appropriate error string.
 */
int redisBufferWrite(redisContext *c, int *done) {
	int nwritten;

	/* Return early when the context has seen an error. */
	if (c->err)
		return REDIS_ERR;

	if ((c->flags & REDIS_TLS) && sdslen(c->obuf) > 0) {
		SSL *ssl = (SSL *)c->tls_ssl;
		size_t total = 0;
		int sslrc;

		if (ssl == NULL) {
			__redisSetError(c, REDIS_ERR_OTHER, "TLS connection is not initialized");
			return REDIS_ERR;
		}
		while (total < sdslen(c->obuf)) {
			sslrc = SSL_write(ssl, c->obuf + total, (int)(sdslen(c->obuf) - total));
			if (sslrc <= 0) {
				if (SSL_get_error(ssl, sslrc) == SSL_ERROR_WANT_WRITE) {
					if (redisTlsDrainOutput(c) == REDIS_ERR)
						return REDIS_ERR;
					continue;
				}
				redisTlsSetError(c, REDIS_ERR_SSL, "TLS write failed");
				return REDIS_ERR;
			}
			total += (size_t)sslrc;
		}
		if (redisTlsDrainOutput(c) == REDIS_ERR)
			return REDIS_ERR;
		sdsfree(c->obuf);
		c->obuf = sdsempty();
	}
	else if (sdslen(c->obuf) > 0) {
		nwritten = (int)write(c->fd, c->obuf, sdslen(c->obuf));                   WIN_PORT_FIX /* cast (int) */
			if (nwritten == -1) {
				if ((errno == EAGAIN && !(c->flags & REDIS_BLOCK)) || (errno == EINTR)) {
					/* Try again later */
				}
				else {
					__redisSetError(c, REDIS_ERR_IO, NULL);
					return REDIS_ERR;
				}
			}
			else if (nwritten > 0) {
				if (nwritten == (signed)sdslen(c->obuf)) {
					sdsfree(c->obuf);
					c->obuf = sdsempty();
				}
				else {
					sdsrange(c->obuf, nwritten, -1);
				}
			}
	}
	if (done != NULL) *done = (sdslen(c->obuf) == 0);
	return REDIS_OK;
}

#ifdef _WIN32
/* Use this function if the caller has already written the data.
 */
int redisBufferWriteDone(redisContext *c, int nwritten, int *done) {
	if (nwritten > 0) {
		if (nwritten == (signed)sdslen(c->obuf)) {
			sdsfree(c->obuf);
			c->obuf = sdsempty();
		}
		else {
			sdsrange(c->obuf, nwritten, -1);
		}
	}
	if (done != NULL) *done = (sdslen(c->obuf) == 0);
	return REDIS_OK;
}
#endif

/* Internal helper function to try and get a reply from the reader,
 * or set an error in the context otherwise. */
int redisGetReplyFromReader(redisContext *c, void **reply) {
	if (redisReaderGetReply(c->reader, reply) == REDIS_ERR) {
		__redisSetError(c, c->reader->err, c->reader->errstr);
		return REDIS_ERR;
	}
	return REDIS_OK;
}

int redisGetReply(redisContext *c, void **reply) {
	int wdone = 0;
	void *aux = NULL;

	/* Try to read pending replies */
	if (redisGetReplyFromReader(c, &aux) == REDIS_ERR)
		return REDIS_ERR;

	/* For the blocking context, flush output buffer and read reply */
	if (aux == NULL && c->flags & REDIS_BLOCK) {
		/* Write until done */
		do {
			if (redisBufferWrite(c, &wdone) == REDIS_ERR)
				return REDIS_ERR;
		} while (!wdone);

		/* Read until there is a reply */
		do {
			if (redisBufferRead(c) == REDIS_ERR)
				return REDIS_ERR;
			if (redisGetReplyFromReader(c, &aux) == REDIS_ERR)
				return REDIS_ERR;
		} while (aux == NULL);
	}

	/* Set reply object */
	if (reply != NULL) *reply = aux;
	return REDIS_OK;
}

/* Helper function for the redisAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call redisGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 */
int __redisAppendCommand(redisContext *c, const char *cmd, size_t len) {
	sds newbuf;

	newbuf = sdscatlen(c->obuf, cmd, len);
	if (newbuf == NULL) {
		__redisSetError(c, REDIS_ERR_OOM, "Out of memory");
		return REDIS_ERR;
	}

	c->obuf = newbuf;
	return REDIS_OK;
}

int redisAppendFormattedCommand(redisContext *c, const char *cmd, size_t len) {
	if (__redisAppendCommand(c, cmd, len) != REDIS_OK) {
		return REDIS_ERR;
	}

	return REDIS_OK;
}

int redisvAppendCommand(redisContext *c, const char *format, va_list ap) {
	char *cmd;
	int len;

	len = redisvFormatCommand(&cmd, format, ap);
	if (len == -1) {
		__redisSetError(c, REDIS_ERR_OOM, "Out of memory");
		return REDIS_ERR;
	}
	else if (len == -2) {
		__redisSetError(c, REDIS_ERR_OTHER, "Invalid format string");
		return REDIS_ERR;
	}

	if (__redisAppendCommand(c, cmd, len) != REDIS_OK) {
		free(cmd);
		return REDIS_ERR;
	}

	free(cmd);
	return REDIS_OK;
}

int redisAppendCommand(redisContext *c, const char *format, ...) {
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = redisvAppendCommand(c, format, ap);
	va_end(ap);
	return ret;
}

int redisAppendCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen) {
	sds cmd;
	int len;

	len = redisFormatSdsCommandArgv(&cmd, argc, argv, argvlen);
	if (len == -1) {
		__redisSetError(c, REDIS_ERR_OOM, "Out of memory");
		return REDIS_ERR;
	}

	if (__redisAppendCommand(c, cmd, len) != REDIS_OK) {
		sdsfree(cmd);
		return REDIS_ERR;
	}

	sdsfree(cmd);
	return REDIS_OK;
}

/* Helper function for the redisCommand* family of functions.
 *
 * Write a formatted command to the output buffer. If the given context is
 * blocking, immediately read the reply into the "reply" pointer. When the
 * context is non-blocking, the "reply" pointer will not be used and the
 * command is simply appended to the write buffer.
 *
 * Returns the reply when a reply was successfully retrieved. Returns NULL
 * otherwise. When NULL is returned in a blocking context, the error field
 * in the context will be set.
 */
static void *__redisBlockForReply(redisContext *c) {
	void *reply;

	if (c->flags & REDIS_BLOCK) {
		if (redisGetReply(c, &reply) != REDIS_OK)
			return NULL;
		return reply;
	}
	return NULL;
}

void *redisvCommand(redisContext *c, const char *format, va_list ap) {
	if (redisvAppendCommand(c, format, ap) != REDIS_OK)
		return NULL;
	return __redisBlockForReply(c);
}

void *redisCommand(redisContext *c, const char *format, ...) {
	va_list ap;
	void *reply = NULL;
	va_start(ap, format);
	reply = redisvCommand(c, format, ap);
	va_end(ap);
	return reply;
}

void *redisCommandArgv(redisContext *c, int argc, const char **argv, const size_t *argvlen) {
	if (redisAppendCommandArgv(c, argc, argv, argvlen) != REDIS_OK)
		return NULL;
	return __redisBlockForReply(c);
}