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

#ifdef _WIN32
#include "Win32_Interop/win32_types.h"
#include "Win32_Interop/Win32_Error.h"
#endif

#include "server.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"
#include "stream.h"

#include <math.h>
#include <sys/types.h>
#ifdef _WIN32
#include <stdio.h>
#include "Win32_Interop/Win32_QFork.h"
#include "Win32_Interop/win32_wsiocp.h"
#include <direct.h>
#define MAXPATHLEN 1024
#else
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/param.h>
#endif

#define rdbExitReportCorruptRDB(...) rdbCheckThenExit(__LINE__,__VA_ARGS__)

extern int rdbCheckMode;
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

WIN32_ONLY(__declspec(noreturn))
void rdbCheckThenExit(int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading function at rdb.c:%d -> ", linenum);
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (!rdbCheckMode) {
        serverLog(LL_WARNING, "%s", msg);
        char *argv[2] = {"",server.rdb_filename};
        redis_check_rdb_main(2,argv,NULL);
    } else {
        rdbCheckError("%s",msg);
    }
    exit(1);
}

/* RDB version currently being loaded. Set by rdbLoadRio() and by the DUMP
 * payload loader before rdbLoadObject() decodes values. The server is
 * single-threaded while loading, so a file-scope value is safe. */
int rdb_loading_version = RDB_VERSION;

static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return (int)len;                  WIN_PORT_FIX /* cast (int) */
}

/* This is just a wrapper for the low level function rioRead() that will
 * automatically abort if it is not possible to read the specified amount
 * of bytes. */
void rdbLoadRaw(rio *rdb, void *buf, uint64_t len) {
    if (rioRead(rdb,buf,len) == 0) {
        rdbExitReportCorruptRDB(
            "Impossible to read %llu bytes in rdbLoadRaw()",
            (PORT_ULONGLONG) len);
        return; /* Not reached. */
    }
}

int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth. */
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME
 * opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS
 * opcode. */
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    rdbLoadRaw(rdb,&t32,4);
    return (time_t)t32;
}

int rdbSaveMillisecondTime(rio *rdb, PORT_LONGLONG t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. */
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past,
 * allowing big endian systems to load their own old RDB files. */
PORT_LONGLONG rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
    rdbLoadRaw(rdb,&t64,8);
    if (rdbver >= 9) /* Check the top comment of this function. */
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. */
    return (PORT_LONGLONG)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the RDB_* definitions for more information
 * on the types of encoding. */
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        uint32_t len32 = htonl(len);
        if (rdbWriteRaw(rdb,&len32,4) == -1) return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonu64(len);
        if (rdbWriteRaw(rdb,&len,8) == -1) return -1;
        nwritten = 1+8;
    }
    return (int)nwritten;                                                       WIN_PORT_FIX /* cast (int) */
}


/* Load an encoded length. If the loaded length is a normal length as stored
 * with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded'
 * is set to 1 and the encoding format is stored at '*lenptr'.
 *
 * See the RDB_ENC_* definitions in rdb.h for more information on special
 * encodings.
 *
 * The function returns -1 on error, 0 on success. */
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;
        *lenptr = ntohu64(len);
    } else {
        rdbExitReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR
 * (since it is a too large count to be applicable in any Redis data
 * structure). */
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;

    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) return RDB_LENERR;
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
int rdbEncodeInteger(PORT_LONGLONG value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((PORT_LONGLONG)1<<31) && value <= ((PORT_LONGLONG)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenerincLoadStringObject() for more info. */
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int encode = flags & RDB_LOAD_ENC;
    unsigned char enc[4];
    PORT_LONGLONG val;

    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        rdbExitReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
    }
    if (plain || sds) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) *lenptr = len;
        p = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        memcpy(p,buf,len);
        return p;
    } else if (encode) {
        return createStringObjectFromLongLongForValue(val);
    } else {
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    PORT_LONGLONG value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    return rdbEncodeInteger(value,enc);
}

ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;
    nwritten += n;

    return nwritten;

writeerr:
    return -1;
}

ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. */
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;

    /* Allocate our target according to the uncompressed size. */
    if (plain) {
        val = zmalloc(len);
    } else {
        val = sdsnewlen(SDS_NOINIT,len);
    }
    if (lenptr) *lenptr = len;

    /* Load the compressed representation and uncompress it to target. */
    if (rioRead(rdb,c,clen) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) {
        if (rdbCheckMode) rdbCheckSetError("Invalid LZF compressed string");
        goto err;
    }
    zfree(c);

    if (plain || sds) {
        return val;
    } else {
        return createObject(OBJ_STRING,val);
    }
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, PORT_LONGLONG value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);
        serverAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead. */
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(rdb,(PORT_LONG)obj->ptr);
    } else {
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 *
 * On I/O error NULL is returned.
 */
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
    int encode = flags & RDB_LOAD_ENC;
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int isencoded;
    uint64_t len;

    len = rdbLoadLen(rdb,&isencoded);
    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        case RDB_ENC_LZF:
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbExitReportCorruptRDB("Unknown RDB string encoding type %d",len);
        }
    }

    if (len == RDB_LENERR) return NULL;
    if (plain || sds) {
        void *buf = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        if (lenptr) *lenptr = len;
        if (len && rioRead(rdb,buf,len) == 0) {
            if (plain)
                zfree(buf);
            else
                sdsfree(buf);
            return NULL;
        }
        return buf;
    } else {
        robj *o = encode ? createStringObject(SDS_NOINIT,len) :
                           createRawStringObject(SDS_NOINIT,len);
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    }
}

robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}

robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((PORT_LONGLONG)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(PORT_LONGLONG)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = (unsigned char)strlen((char*)buf+1);                           WIN_PORT_FIX /* cast (unsigned char) */
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;
#ifdef _WIN32
    double scannedVal = 0;
    int assigned = 0;
    memset(buf, 0, sizeof(buf));
#endif

    if (rioRead(rdb,&len,1) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
#ifdef _WIN32
        assigned = sscanf_s(buf, "%lg", &scannedVal);
        if( assigned != 0 ) {
            (*val) = scannedVal;
            return 0;
        } else {
            return -1;
        }
#else
        sscanf(buf, "%lg", val);
        return 0;
#endif
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise
 * the value is copied verbatim from memory to disk.
 *
 * Return -1 on error, the size of the serialized value on success. */
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for
 * more info. On error -1 is returned, otherwise 0. */
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. */
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. */
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o". */
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case OBJ_STRING:
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb,RDB_TYPE_LIST_QUICKLIST);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. */
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the informations about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to
 * put a reference to the same structure. */
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) return -1;
    nwritten += n;

    /* Save each entry. */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) return -1;
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1)
                return -1;
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) return -1;
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. */
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. */
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) return -1;
    nwritten += n;

    /* Save each consumer. */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) return -1;
        nwritten += n;

        /* Last seen time. */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1)
            return -1;
        nwritten += n;

        /* RDB 11: last successful delivery time (0 means never). */
        if ((n = rdbSaveLen(rdb,consumer->active_time < 0 ?
                                 0 : (uint64_t)consumer->active_time)) == -1)
            return -1;
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. */
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1)
            return -1;
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object.
 * Returns -1 on error, number of bytes written on success. */
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key) {
    ssize_t n = 0, nwritten = 0;
    int stream_counters_present = 0;

    if (o->type == OBJ_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == OBJ_LIST) {
        /* Save a list value */
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;

            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;

            while(node) {
                if (quicklistNodeIsCompressed(node)) {
                    void *data;
                    size_t compress_len = quicklistGetLzf(node, &data);
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                } else {
                    if ((n = rdbSaveRawString(rdb,node->zl,node->sz)) == -1) return -1;
                    nwritten += n;
                }
                node = node->next;
            }
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds ele = dictGetKey(de);
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))
                    == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;

            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). */
            zskiplistNode *zn = zsl->tail;
            while (zn != NULL) {
                if ((n = rdbSaveRawString(rdb,
                    (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
                {
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;

        } else if (o->encoding == OBJ_ENCODING_HT) {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds field = dictGetKey(de);
                sds value = dictGetVal(de);

                if ((n = rdbSaveRawString(rdb,(unsigned char*)field,
                        sdslen(field))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value,
                        sdslen(value))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. */
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are,
         * when loading back, we'll use the first entry of each listpack
         * to insert it back into the radix tree. */
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) return -1;
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) return -1;
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for
         * number of items: not a great CPU / space tradeoff. */
        if ((n = rdbSaveLen(rdb,s->length)) == -1) return -1;
        nwritten += n;
        /* Save the last entry ID. */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) return -1;
        nwritten += n;
        /* Redis 6.2 stores stream counters after the last ID. A sentinel
         * length marks their presence so snapshots written by the older
         * single-file format can still be loaded. */
        if ((n = rdbSaveLen(rdb,STREAM_RDB_EXT_MARK)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->entries_added)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->max_deleted_entry_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->max_deleted_entry_id.seq)) == -1) return -1;
        nwritten += n;
        stream_counters_present = 1;

        /* The consumer groups and their clients are part of the stream
         * type, so serialize every consumer group. */

        /* Save the number of groups. */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1)
                    return -1;
                nwritten += n;

                /* Last ID. */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) return -1;
                nwritten += n;
                if (stream_counters_present) {
                    if ((n = rdbSaveLen(rdb,STREAM_RDB_EXT_MARK)) == -1)
                        return -1;
                    nwritten += n;
                    if ((n = rdbSaveLen(rdb,cg->entries_read)) == -1)
                        return -1;
                    nwritten += n;
                }

                /* Save the global PEL. */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) return -1;
                nwritten += n;

                /* Save the consumers of this group. */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) return -1;
                nwritten += n;
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. */
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;

        /* Write the "module" identifier as prefix, so that we'll be able
         * to call the right module during loading. */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) return -1;

        /* Then write the module-specific representation + EOF marker. */
        moduleInitIOContext(io,mt,rdb,key);
        io.bytes += retval;
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1)
            io.error = 1;
        else
            io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
size_t rdbSavedObjectLen(robj *o) {
    ssize_t len = rdbSaveObject(NULL,o,NULL);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, PORT_LONGLONG expiretime) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time */
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save the LRU info. */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(val);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val,key) == -1) return -1;
    return 1;
}

/* Save an AUX field. */
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) return -1;
    len += ret;
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). */
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, PORT_LONGLONG val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
int rdbSaveInfoAuxFields(rio *rdb, int flags, rdbSaveInfo *rsi) {
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_preamble = (flags & RDB_SAVE_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. */
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;

    /* Handle saving options that generate aux fields. */
    if (rsi) {
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db)
            == -1) return -1;
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid)
            == -1) return -1;
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset)
            == -1) return -1;
    }
    if (rdbSaveAuxFieldStrInt(rdb,"aof-preamble",aof_preamble) == -1) return -1;
    return 1;
}

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt) {
    /* Save a module-specific aux value. */
    RedisModuleIO io;
    int retval = rdbSaveType(rdb, RDB_OPCODE_MODULE_AUX);
    if (retval == -1) return -1;
    moduleInitIOContext(io,mt,rdb,NULL);                           WIN_PORT_FIX /*moved earlier before first usage of "io"*/
    io.bytes += retval;

    /* Write the "module" identifier as prefix, so that we'll be able
     * to call the right module during loading. */
    retval = rdbSaveLen(rdb,mt->id);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* write the 'when' so that we can provide it on loading. add a UINT opcode
     * for backwards compatibility, everything after the MT needs to be prefixed
     * by an opcode. */
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_UINT);
    if (retval == -1) return -1;
    io.bytes += retval;
    retval = rdbSaveLen(rdb,when);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* Then write the module-specific representation + EOF marker. */
    mt->aux_save(&io,when);
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
    if (retval == -1)
        io.error = 1;
    else
        io.bytes += retval;

    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error)
        return -1;
    return io.bytes;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
int rdbSaveRio(rio *rdb, int *error, int flags, rdbSaveInfo *rsi) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    uint64_t cksum;
    size_t processed = 0;

    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum;
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    if (rdbSaveInfoAuxFields(rdb,flags,rsi) == -1) goto werr;
    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) goto werr;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);

        /* Write the SELECT DB opcode */
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        /* Write the RESIZE DB opcode. We trim the size to UINT32_MAX, which
         * is currently the largest type we are able to represent in RDB sizes.
         * However this does not limit the actual size of the DB to load since
         * these sizes are just hints to resize the hash tables. */
        uint64_t db_size, expires_size;
        db_size = dictSize(db->dict);
        expires_size = dictSize(db->expires);
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) goto werr;
        if (rdbSaveLen(rdb,db_size) == -1) goto werr;
        if (rdbSaveLen(rdb,expires_size) == -1) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetKey(de);
            robj key, *o = dictGetVal(de);
            PORT_LONGLONG expire;

            initStaticStringObject(key,keystr);
            expire = getExpire(db,&key);
            if (rdbSaveKeyValuePair(rdb,&key,o,expire) == -1) goto werr;

            /* When this RDB is produced as part of an AOF rewrite, move
             * accumulated diff from parent to child while rewriting in
             * order to have a smaller final write. */
            if (flags & RDB_SAVE_AOF_PREAMBLE &&
                rdb->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES)
            {
                processed = rdb->processed_bytes;
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }

    /* If we are storing the replication information on disk, persist
     * the script cache as well: on successful PSYNC after a restart, we need
     * to be able to process any EVALSHA inside the replication backlog the
     * master will send us. */
    if (rsi && dictSize(server.lua_scripts)) {
        di = dictGetIterator(server.lua_scripts);
        while((de = dictNext(di)) != NULL) {
            robj *body = dictGetVal(de);
            if (rdbSaveAuxField(rdb,"lua",3,body->ptr,sdslen(body->ptr)) == -1)
                goto werr;
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }

    /* Function libraries are stored as AUX fields. Older readers skip them. */
    if (functionsSaveAuxField(rdb) == C_ERR) goto werr;

    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) goto werr;

    /* EOF opcode */
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

werr:
    if (error) *error = errno;
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
int rdbSaveRioWithEOFMark(rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(rdb,error,RDB_SAVE_NONE,rsi) == C_ERR) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    return C_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
int rdbSave(char *filename, rdbSaveInfo *rsi) {
    char tmpfile[256];
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    FILE *fp;
    rio rdb;
    int error = 0;

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    WIN32_ONLY(tmpfile[sizeof(tmpfile) - 1] = 0;) /*get rid of C6053 warning*/
    fp = fopen(tmpfile,IF_WIN32("wb","w"));
    if (!fp) {
        char *cwdp = IF_WIN32(_getcwd, getcwd)(cwd, MAXPATHLEN);
        serverLog(LL_WARNING,
            "Failed opening the RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return C_ERR;
    }

    rioInitWithFile(&rdb,fp);

    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);

    if (rdbSaveRio(&rdb,&error,RDB_SAVE_NONE,rsi) == C_ERR) {
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            IF_WIN32(wsa_strerror(errno),strerror(errno)));
        unlink(tmpfile);
        return C_ERR;
    }

    serverLog(LL_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    server.lastbgsave_status = C_OK;
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error saving DB on disk: %s", IF_WIN32(wsa_strerror(errno),strerror(errno)));
    fclose(fp);
    unlink(tmpfile);
    return C_ERR;
}

int rdbSaveBackground(char *filename, rdbSaveInfo *rsi) {
    pid_t childpid;
    PORT_LONGLONG start;

    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    server.dirty_before_bgsave = server.dirty;
    server.lastbgsave_try = time(NULL);
    openChildInfoPipe();

    start = ustime();
#ifdef _WIN32
    childpid = BeginForkOperation_Rdb(filename, &server, sizeof(server), dictGetHashFunctionSeed(), modules);
#else
    if ((childpid = fork()) == 0) {
        int retval;

        /* Child */
        closeClildUnusedResourceAfterFork();
        redisSetProcTitle("redis-rdb-bgsave");
        retval = rdbSave(filename,rsi);
        if (retval == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            server.child_info_data.cow_size = private_dirty;
            sendChildInfo(CHILD_INFO_TYPE_RDB);
        }
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
#endif
        /* Parent */
        server.stat_fork_time = ustime()-start;
//[tporadowski/redis] issue #46: ustime() -> gettimeofday_highres() uses GetSystemTimePreciseAsFileTime when available (Windows 8, Windows Server 2012) or
//                    falls back to GetSystemTimeAsFileTime which does not have such high resolution, so "stat_fork_time" may be 0 here
#ifdef _WIN32
        if (server.stat_fork_time == 0) {
            server.stat_fork_time = 100000; //let's pretend it took 100ms (100000 microseconds)
        }
#endif
        server.stat_fork_rate = (double)(zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024 * 1024 * 1024)); /* GB per second. */  WIN_PORT_FIX
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            closeChildInfoPipe();
            server.lastbgsave_status = C_ERR;
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                IF_WIN32(wsa_strerror(errno),strerror(errno)));
            return C_ERR;
        }
        serverLog(LL_NOTICE,"Background saving started by pid %d",childpid);
        server.rdb_save_time_start = time(NULL);
        server.rdb_child_pid = childpid;
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;
        updateDictResizePolicy();
        return C_OK;
#ifndef _WIN32
    }
#endif
    return C_OK; /* unreached */
}

void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,sizeof(tmpfile),"temp-%d.rdb", (int) childpid);
    WIN32_ONLY(tmpfile[sizeof(tmpfile) - 1] = 0;) /*get rid of C6053 warning*/
    unlink(tmpfile);
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. */
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbExitReportCorruptRDB(
                    "Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
robj *rdbLoadObject(int rdbtype, rio *rdb, robj *key) {
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            dec = getDecodedObject(ele);
            size_t len = sdslen(dec->ptr);
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        size_t max_entries = server.set_max_intset_entries;
        if (max_entries >= 1<<30) max_entries = 1<<30;
        if (len > max_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the set */
        for (i = 0; i < len; i++) {
            PORT_LONGLONG llval;
            sds sdsele;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element. */
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. */
            if (o->encoding == OBJ_ENCODING_HT) {
                dictAdd((dict*)o->ptr,sdsele,NULL);
            } else {
                sdsfree(sdsele);
            }
        }
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read list/set value. */
        uint64_t zsetlen;
        size_t maxelelen = 0, totelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;

        if (zsetlen > DICT_HT_INITIAL_SIZE)
            dictExpand(zs->dict,zsetlen);

        /* Load every single element of the sorted set. */
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) return NULL;
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) return NULL;
            }

            /* Don't care about integer-encoded strings. */
            if (sdslen(sdsele) > maxelelen) maxelelen = sdslen(sdsele);
            totelelen += sdslen(sdsele);

            znode = zslInsert(zs->zsl,score,sdsele);
            dictAdd(zs->dict,sdsele,&znode->score);
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value &&
            ziplistSafeToAdd(NULL, totelelen))
        {
            zsetConvert(o,OBJ_ENCODING_ZIPLIST);
        }
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds field, value;

        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;

        o = createHashObject();

        /* Too many entries? Use a hash table. */
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

        /* Load every field and value into the ziplist */
        while (o->encoding == OBJ_ENCODING_ZIPLIST && len > 0) {
            len--;
            /* Load raw strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            /* Convert to hash table if size threshold is exceeded */
            if (sdslen(field) > server.hash_max_ziplist_value ||
                sdslen(value) > server.hash_max_ziplist_value ||
                !ziplistSafeToAdd(o->ptr, sdslen(field)+sdslen(value)))
            {
                hashTypeConvert(o, OBJ_ENCODING_HT);
                ret = dictAdd((dict*)o->ptr, field, value);
                if (ret == DICT_ERR) {
                    rdbExitReportCorruptRDB("Duplicate hash fields detected");
                }
                break;
            }

            /* Add pair to ziplist */
            o->ptr = ziplistPush(o->ptr, (unsigned char*)field,
                    sdslen(field), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, (unsigned char*)value,
                    sdslen(value), ZIPLIST_TAIL);

            sdsfree(field);
            sdsfree(value);
        }

        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,len);

        /* Load remaining fields and values into the hash table */
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            /* Add pair to hash table */
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbExitReportCorruptRDB("Duplicate keys detected");
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST) {
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        while (len--) {
            unsigned char *zl =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (zl == NULL) return NULL;
            quicklistAppendZiplist(o->ptr, zl);
        }
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST)
    {
        unsigned char *encoded =
            rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
        if (encoded == NULL) return NULL;
        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        switch(rdbtype) {
            case RDB_TYPE_HASH_ZIPMAP:
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        if (!ziplistSafeToAdd(zl, (size_t)flen + vlen)) {
                            rdbExitReportCorruptRDB("Hash zipmap too big (%u)", flen);
                        }

                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST:
                o->type = OBJ_LIST;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                listTypeConvert(o,OBJ_ENCODING_QUICKLIST);
                break;
            case RDB_TYPE_SET_INTSET:
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST:
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST:
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS) {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);
        int stream_counters_present = 0;

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbExitReportCorruptRDB("Stream master ID loading failed: invalid encoding or I/O error.");
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbExitReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
            }

            /* Load the listpack. */
            unsigned char *lp =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (lp == NULL) return NULL;
            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on
                 * deletion we should remove the radix tree key if the
                 * resulting listpack is empty. */
                rdbExitReportCorruptRDB("Empty listpack inside stream");
            }

            /* Insert the key in the radix tree. */
            int retval = raxInsert(s->rax,
                (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval)
                rdbExitReportCorruptRDB("Listpack re-added with existing key");
        }
        /* Load total number of items inside the stream. */
        s->length = rdbLoadLen(rdb,NULL);
        /* Load the last entry ID. */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);
        s->entries_added = s->length;
        /* A sentinel length marks the local extension that carries the 6.2
         * stream counters. Snapshots from the old single-file layout omit it
         * and keep the historical behaviour of deriving the counters from
         * the current stream. */
        if (rdbLoadLen(rdb,NULL) == STREAM_RDB_EXT_MARK) {
            s->entries_added = rdbLoadLen(rdb,NULL);
            s->max_deleted_entry_id.ms = rdbLoadLen(rdb,NULL);
            s->max_deleted_entry_id.seq = rdbLoadLen(rdb,NULL);
            stream_counters_present = 1;
        }

        /* Consumer groups loading */
        size_t cgroups_count = rdbLoadLen(rdb,NULL);
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the
             * consumer group ASAP and populate its structure as
             * we read more data. */
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbExitReportCorruptRDB(
                    "Error reading the consumer group name from Stream");
            }
            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            uint64_t entries_read = 0;
            int group_counters_present = 0;
            if (stream_counters_present &&
                rdbLoadLen(rdb,NULL) == STREAM_RDB_EXT_MARK)
            {
                entries_read = rdbLoadLen(rdb,NULL);
                group_counters_present = 1;
            }
            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id);
            if (cgroup == NULL)
                rdbExitReportCorruptRDB("Duplicated consumer group name %s",
                                         cgname);
            if (group_counters_present) cgroup->entries_read = entries_read;
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. */
            size_t pel_size = rdbLoadLen(rdb,NULL);
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                rdbLoadRaw(rdb,rawid,sizeof(rawid));
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (!raxInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL))
                    rdbExitReportCorruptRDB("Duplicated gobal PEL entry "
                                            "loading stream consumer group");
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. */
            size_t consumers_num = rdbLoadLen(rdb,NULL);
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbExitReportCorruptRDB(
                        "Error reading the consumer name from Stream group");
                }
                streamConsumer *consumer =
                    streamLookupConsumer(cgroup,cname,SLC_NONE);
                sdsfree(cname);
                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                consumer->active_time = -1;
                if (rdb_loading_version >= 11) {
                    uint64_t active = rdbLoadLen(rdb,NULL);
                    if (active) consumer->active_time = active;
                }

                /* Load the PEL about entries owned by this specific
                 * consumer. */
                pel_size = rdbLoadLen(rdb,NULL);
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    rdbLoadRaw(rdb,rawid,sizeof(rawid));
                    streamNACK *nack = raxFind(cgroup->pel,rawid,sizeof(rawid));
                    if (nack == raxNotFound)
                        rdbExitReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. */
                    nack->consumer = consumer;
                    if (!raxInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL))
                        rdbExitReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                }
            }
        }
    } else if (rdbtype == RDB_TYPE_MODULE || rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);
        char name[10];

        if (rdbCheckMode && rdbtype == RDB_TYPE_MODULE_2) {
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data I can't load: no matching module '%s'", name);
            exit(1);
        }
        RedisModuleIO io;
        moduleInitIOContext(io,mt,rdb,key);
        io.ver = (rdbtype == RDB_TYPE_MODULE) ? 1 : 2;
        /* Call the rdb_load method of the module providing the 10 bit
         * encoding version in the lower 10 bits of the module ID. */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. */
        if (io.ver == 2) {
            uint64_t eof = rdbLoadLen(rdb,NULL);
            if (eof != RDB_MODULE_OPCODE_EOF) {
                serverLog(LL_WARNING,"The RDB file contains module data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                exit(1);
            }
        }

        if (ptr == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
            exit(1);
        }
        o = createModuleObject(mt,ptr);
    } else {
        rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
void startLoading(FILE *fp) {
    struct IF_WIN32(_stat64,stat) sb;                                           // TODO: verify for 32-bit

    /* Load the DB */
    server.loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_loaded_bytes = 0;
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 0;
    } else {
        server.loading_total_bytes = sb.st_size;
    }
}

/* Refresh the loading progress info */
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished */
void stopLoading(void) {
    server.loading = 0;
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. */
        updateCachedTime(0);
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            replicationSendNewlineToMaster();
        loadingProgress((off_t)r->processed_bytes);                             WIN_PORT_FIX /* cast (off_t) */
        processEventsWhileBlocked();
    }
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. */
int rdbLoadRio(rio *rdb, rdbSaveInfo *rsi, int loading_aof) {
    uint64_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];

    rdb->update_cksum = rdbLoadProgressCallback;
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
    if (rioRead(rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return C_ERR;
    }
    rdbver = atoi(buf+5);
    rdb_loading_version = rdbver;
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. */
    PORT_LONGLONG lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    PORT_LONGLONG lru_clock = LRU_CLOCK();
    
    while(1) {
        robj *key, *val;

        /* Read type. */
        if ((type = rdbLoadType(rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            expiretime = rdbLoadTime(rdb);
            expiretime *= 1000;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(rdb,&byte,1) == 0) goto eoferr;
            lfu_freq = byte;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
            uint64_t qword;
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            lru_idle = qword;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint64_t db_size, expires_size;
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            dictExpand(db->dict,db_size);
            dictExpand(db->expires,expires_size);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are requierd to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) goto eoferr;

            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. */
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Load the script back in memory. */
                if (luaCreateFunction(NULL,server.lua,auxval) == NULL) {
                    rdbExitReportCorruptRDB(
                        "Can't load Lua script from RDB file! "
                        "BODY: %s", auxval->ptr);
                }
            } else if (functionsLoadAuxField(auxkey,auxval) < 0) {
                rdbExitReportCorruptRDB(
                    "Can't load function library from RDB file!");
            } else if (strcasecmp(auxkey->ptr,"function-library") != 0) {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* Load module data that is not related to the Redis key space.
             * Such data can be potentially be stored both before and after the
             * RDB keys-values section. */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            int when_opcode = rdbLoadLen(rdb,NULL);
            int when = rdbLoadLen(rdb,NULL);
            if (when_opcode != RDB_MODULE_OPCODE_UINT)
                rdbExitReportCorruptRDB("bad when_opcode");
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                if (!mt->aux_load) {
                    /* Module doesn't support AUX. */
                    serverLog(LL_WARNING,"The RDB file contains module AUX data, but the module '%s' doesn't seem to support it.", name);
                    exit(1);
                }

                RedisModuleIO io;
                moduleInitIOContext(io,mt,rdb,NULL);
                io.ver = 2;
                /* Call the rdb_load method of the module providing the 10 bit
                 * encoding version in the lower 10 bits of the module ID. */
                if (mt->aux_load(&io,moduleid&1023, when) || io.error) {
                    moduleTypeNameByID(name,moduleid);
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
                    exit(1);
                }
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                uint64_t eof = rdbLoadLen(rdb,NULL);
                if (eof != RDB_MODULE_OPCODE_EOF) {
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                    exit(1);
                }
                continue;
            } else {
                /* RDB check mode. */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
                continue; /* Read next opcode. */
            }
        }

        /* Read key */
        if ((key = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,rdb,key)) == NULL) goto eoferr;
        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave. */
        if (server.masterhost == NULL && !loading_aof && expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
        } else {
            /* Add the new object in the hash table */
            dbAdd(db,key,val);

            /* Set the expire time if needed */
            if (expiretime != -1) setExpire(NULL,db,key,expiretime);
            
            /* Set usage information (for eviction). */
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock);

            /* Decrement the key refcount since dbAdd() will take its
             * own reference. */
            decrRefCount(key);
        }

        /* Reset the state that is key-specified and is populated by
         * opcodes before the key, so that we start from scratch again. */
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    if (rdbver >= 5) {
        uint64_t cksum, expected = rdb->cksum;

        if (rioRead(rdb,&cksum,8) == 0) goto eoferr;
        if (server.rdb_checksum) {
            memrev64ifbe(&cksum);
            if (cksum == 0) {
                serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
                serverLog(LL_WARNING,"Wrong RDB checksum. Aborting now.");
                rdbExitReportCorruptRDB("RDB CRC error");
            }
        }
    }
    return C_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    serverLog(LL_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbExitReportCorruptRDB("Unexpected EOF reading RDB file");
    return C_ERR; /* Just to avoid warning */
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The
 * filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO
 * output is initialized and finalized.
 *
 * If you pass an 'rsi' structure initialied with RDB_SAVE_OPTION_INIT, the
 * loading code will fiil the information fields in the structure. */
int rdbLoad(char *filename, rdbSaveInfo *rsi) {
    FILE *fp;
    rio rdb;
    int retval;

    if ((fp = fopen(filename,IF_WIN32("rb","r"))) == NULL) return C_ERR;
    startLoading(fp);
    rioInitWithFile(&rdb,fp);
    retval = rdbLoadRio(&rdb,rsi,0);
    fclose(fp);
    stopLoading();
    return retval;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = C_OK;
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.rdb_child_pid);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error condition. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_DISK);
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Salves socket transfers for
 * diskless replication. */
void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    uint64_t *ok_slaves;

    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_start = -1;

    /* If the child returns an OK exit code, read the set of slave client
     * IDs and the associated status code. We'll terminate all the slaves
     * in error state.
     *
     * If the process returned an error, consider the list of slaves that
     * can continue to be empty, so that it's just a special case of the
     * normal code path. */
    ok_slaves = zmalloc(sizeof(uint64_t)); /* Make space for the count. */
    ok_slaves[0] = 0;
    if (!bysignal && exitcode == 0) {
        int readlen = sizeof(uint64_t);

        if (read(server.rdb_pipe_read_result_from_child, ok_slaves, readlen) ==
                 readlen)
        {
            readlen = (int)(ok_slaves[0]*sizeof(uint64_t)*2);                   WIN_PORT_FIX /* cast (int) */

            /* Make space for enough elements as specified by the first
             * uint64_t element in the array. */
            ok_slaves = zrealloc(ok_slaves,sizeof(uint64_t)+readlen);
            if (readlen &&
                read(server.rdb_pipe_read_result_from_child, ok_slaves+1,
                     readlen) != readlen)
            {
                ok_slaves[0] = 0;
            }
        }
    }

    close(server.rdb_pipe_read_result_from_child);
    close(server.rdb_pipe_write_result_to_parent);

    /* We can continue the replication process with all the slaves that
     * correctly received the full payload. Others are terminated. */
    listNode *ln;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            uint64_t j;
            int errorcode = 0;

            /* Search for the slave ID in the reply. In order for a slave to
             * continue the replication process, we need to find it in the list,
             * and it must have an error code set to 0 (which means success). */
            for (j = 0; j < ok_slaves[0]; j++) {
                if (slave->id == ok_slaves[2*j+1]) {
                    errorcode = (int)(ok_slaves[2*j+2]);                        WIN_PORT_FIX /* cast (int) */
                    break; /* Found in slaves list. */
                }
            }
            if (j == ok_slaves[0] || errorcode != 0) {
                serverLog(LL_WARNING,
                "Closing slave %s: child->slave RDB transfer failed: %s",
                    replicationGetSlaveName(slave),
                    (errorcode == 0) ? "RDB transfer child aborted"
                                     : strerror(errorcode));
                freeClient(slave);
            } else {
#ifdef _WIN32
                if (slave->flags & CLIENT_TLS) {
                    /* The child only wrote into the relay socket. The parent
                     * is still relaying the payload through the replica's TLS
                     * session, and sendBulkToSlaveTlsPipe() puts the replica
                     * online when the relay reports EOF. */
                    continue;
                }
#endif
                serverLog(LL_WARNING,
                "Slave %s correctly received the streamed RDB file.",
                    replicationGetSlaveName(slave));
                /* Restore the socket as non-blocking. */
                anetNonBlock(NULL,slave->fd);
                anetSendTimeout(NULL,slave->fd,0);
            }
        }
    }
    zfree(ok_slaves);

    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_SOCKET);
}

/* When a background RDB saving/transfer terminates, call the right handler. */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }
}

#ifdef _WIN32
/* The child process that produces a diskless RDB has no TLS state, so it
 * cannot write into a TLS replica socket. To keep diskless replication
 * available for TLS replicas the child gets one end of a loopback socket pair
 * instead and the parent relays the payload through the replica's TLS
 * session. Returns C_OK and fills fds with [parent read end, child write
 * end]. */
static int createRelaySocketPair(int fds[2]) {
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    int listener = -1, client = -1, server = -1;

    listener = socket(AF_INET,SOCK_STREAM,0);
    if (listener == -1) return C_ERR;

    memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(listener,(struct sockaddr*)&sa,salen) == -1) goto cleanup;
    if (listen(listener,1) == -1) goto cleanup;
    if (getsockname(listener,(struct sockaddr*)&sa,&salen) == -1) goto cleanup;

    client = socket(AF_INET,SOCK_STREAM,0);
    if (client == -1) goto cleanup;
    if (connect(client,(struct sockaddr*)&sa,salen) == -1) goto cleanup;
    server = accept(listener,(struct sockaddr*)&sa,&salen);
    if (server == -1) goto cleanup;
    /* Sockets accepted outside the event loop are not associated with the
     * completion port yet; without this the readable event never fires. */
    if (WSIOCP_SocketAttach(server,NULL) == -1) goto cleanup;

    close(listener);
    fds[0] = server;    /* Read by the parent and relayed over TLS. */
    fds[1] = client;    /* Handed to the RDB child. */
    return C_OK;

cleanup:
    if (server != -1) close(server);
    if (client != -1) close(client);
    if (listener != -1) close(listener);
    return C_ERR;
}
#endif

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
int rdbSaveToSlavesSockets(rdbSaveInfo *rsi) {
    int *fds;
    uint64_t *clientids;
    int *relay_fds = NULL;
    int numfds;
#ifdef _WIN32
    int i;
#endif
    listNode *ln;
    listIter li;
    pid_t childpid;
    PORT_LONGLONG start;
    int pipefds[2];

    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    /* Before to fork, create a pipe that will be used in order to
     * send back to the parent the IDs of the slaves that successfully
     * received all the writes. */
    if (pipe(pipefds) == -1) return C_ERR;
    server.rdb_pipe_read_result_from_child = pipefds[0];
    server.rdb_pipe_write_result_to_parent = pipefds[1];

    /* Collect the file descriptors of the slaves we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    fds = zmalloc(sizeof(int)*listLength(server.slaves));
    /* We also allocate an array of corresponding client IDs. This will
     * be useful for the child process in order to build the report
     * (sent via unix pipe) that will be sent to the parent. */
    clientids = zmalloc(sizeof(uint64_t)*listLength(server.slaves));
#ifdef _WIN32
    /* Child end of the relay pair of each fd, or -1 for a plain replica. */
    relay_fds = zmalloc(sizeof(int)*listLength(server.slaves));
#endif
    numfds = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            int childfd = slave->fd;

#ifdef _WIN32
            relay_fds[numfds] = -1;
            if (slave->flags & CLIENT_TLS) {
                int pair[2];

                if (createRelaySocketPair(pair) == C_ERR) {
                    serverLog(LL_WARNING,
                        "Can't create the relay socket for replica %s",
                        replicationGetSlaveName(slave));
                    goto relay_error;
                }
                anetNonBlock(NULL,pair[0]);
                anetBlock(NULL,pair[1]);
                anetSendTimeout(NULL,pair[1],
                                (PORT_LONGLONG)server.repl_timeout*1000);
                if (aeCreateFileEvent(server.el,pair[0],AE_READABLE,
                                      sendBulkToSlave,slave) == AE_ERR)
                {
                    close(pair[0]);
                    close(pair[1]);
                    goto relay_error;
                }
                if (aeCreateFileEvent(server.el,slave->fd,AE_WRITABLE,
                                      sendBulkToSlave,slave) == AE_ERR)
                {
                    aeDeleteFileEvent(server.el,pair[0],AE_READABLE);
                    close(pair[0]);
                    close(pair[1]);
                    goto relay_error;
                }
                slave->repldbfd = pair[0];
                slave->repldboff = 0;
                /* Unknown size: sendBulkToSlave() reads this as "relay the
                 * payload from the parent end until it reports EOF". */
                slave->repldbsize = -1;
                childfd = pair[1];
                relay_fds[numfds] = pair[1];
            } else {
                anetBlock(NULL,slave->fd);
                anetSendTimeout(NULL,slave->fd,(PORT_ULONG)server.repl_timeout*1000);  WIN_PORT_FIX /* cast (PORT_ULONG) */
            }
#else
            /* Put the socket in blocking mode to simplify RDB transfer.
             * We'll restore it when the children returns (since duped socket
             * will share the O_NONBLOCK attribute with the parent). */
            anetBlock(NULL,slave->fd);
            anetSendTimeout(NULL,slave->fd,(PORT_ULONG)server.repl_timeout*1000);  WIN_PORT_FIX /* cast (PORT_ULONG) */
#endif
            clientids[numfds] = slave->id;
            fds[numfds++] = childfd;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
        }
    }

    goto relay_ready;

#ifdef _WIN32
relay_error:
    /* Undo the preparation done so far. The replicas left in
     * WAIT_BGSAVE_START state are freed by the caller. */
    for (i = 0; i < numfds; i++) {
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->id != clientids[i]) continue;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END)
                slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
            if (slave->repldbsize == -1 && slave->repldbfd != -1) {
                aeDeleteFileEvent(server.el,slave->repldbfd,AE_READABLE);
                aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
                close(slave->repldbfd);
                slave->repldbfd = -1;
                slave->repldbsize = 0;
            }
            break;
        }
        if (relay_fds[i] != -1) close(relay_fds[i]);
    }
    close(pipefds[0]);
    close(pipefds[1]);
    zfree(fds);
    zfree(clientids);
    zfree(relay_fds);
    return C_ERR;
#endif

relay_ready:
    /* Create the child process. */
    openChildInfoPipe();
    start = ustime();

#ifdef _WIN32
    childpid = BeginForkOperation_Socket(fds, numfds, clientids, pipefds[1], &server, sizeof(server), dictGetHashFunctionSeed(), modules);
    /* The child has its own duplicated socket for every fd now. Closing our
     * copy of the relay write end is what lets the parent see EOF. */
    for (i = 0; i < numfds; i++)
        if (relay_fds[i] != -1) close(relay_fds[i]);
#else
    if ((childpid = fork()) == 0) {
        /* Child */
        int retval;
        rio slave_sockets;

        rioInitWithFdset(&slave_sockets,fds,numfds);
        zfree(fds);

        closeClildUnusedResourceAfterFork();
        redisSetProcTitle("redis-rdb-to-slaves");

        retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL,rsi);
        if (retval == C_OK && rioFlush(&slave_sockets) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            server.child_info_data.cow_size = private_dirty;
            sendChildInfo(CHILD_INFO_TYPE_RDB);

            /* If we are returning OK, at least one slave was served
             * with the RDB file as expected, so we need to send a report
             * to the parent via the pipe. The format of the message is:
             *
             * <len> <slave[0].id> <slave[0].error> ...
             *
             * len, slave IDs, and slave errors, are all uint64_t integers,
             * so basically the reply is composed of 64 bits for the len field
             * plus 2 additional 64 bit integers for each entry, for a total
             * of 'len' entries.
             *
             * The 'id' represents the slave's client ID, so that the master
             * can match the report with a specific slave, and 'error' is
             * set to 0 if the replication process terminated with a success
             * or the error code if an error occurred. */
            void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
            uint64_t *len = msg;
            uint64_t *ids = len+1;
            int j, msglen;

            *len = numfds;
            for (j = 0; j < numfds; j++) {
                *ids++ = clientids[j];
                *ids++ = slave_sockets.io.fdset.state[j];
            }

            /* Write the message to the parent. If we have no good slaves or
             * we are unable to transfer the message to the parent, we exit
             * with an error so that the parent will abort the replication
             * process with all the childre that were waiting. */
            msglen = sizeof(uint64_t)*(1+2*numfds);
            if (*len == 0 ||
                write(server.rdb_pipe_write_result_to_parent,msg,msglen)
                != msglen)
            {
                retval = C_ERR;
            }
            zfree(msg);
        }
        zfree(clientids);
        rioFreeFdset(&slave_sockets);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
#endif
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                IF_WIN32(wsa_strerror(errno),strerror(errno)));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                int j;

                for (j = 0; j < numfds; j++) {
                    if (slave->id == clientids[j]) {
                        slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
#ifdef _WIN32
                        /* Drop the relay socket of a TLS replica too: the
                         * caller will not free these clients. */
                        if (slave->repldbsize == -1 && slave->repldbfd != -1) {
                            aeDeleteFileEvent(server.el,slave->repldbfd,
                                              AE_READABLE);
                            aeDeleteFileEvent(server.el,slave->fd,
                                              AE_WRITABLE);
                            close(slave->repldbfd);
                            slave->repldbfd = -1;
                            slave->repldbsize = 0;
                        }
#endif
                        break;
                    }
                }
            }
            close(pipefds[0]);
            close(pipefds[1]);
            closeChildInfoPipe();
        } else {
            server.stat_fork_time = ustime()-start;
//[tporadowski/redis] issue #46: ustime() -> gettimeofday_highres() uses GetSystemTimePreciseAsFileTime when available (Windows 8, Windows Server 2012) or
//                    falls back to GetSystemTimeAsFileTime which does not have such high resolution, so "stat_fork_time" may be 0 here
#ifdef _WIN32
            if (server.stat_fork_time == 0) {
                server.stat_fork_time = 100000; //let's pretend it took 100ms (100000 microseconds)
            }
#endif
            server.stat_fork_rate = (double)(zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024 * 1024 * 1024)); /* GB per second. */  WIN_PORT_FIX
            latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);

            serverLog(LL_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
        }
        zfree(clientids);
        zfree(fds);
#ifdef _WIN32
        zfree(relay_fds);
#endif
        return (childpid == -1) ? C_ERR : C_OK;
#ifndef _WIN32
    }
#endif
    return C_OK; /* Unreached. */
}

void saveCommand(client *c) {
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rdbSave(server.rdb_filename,rsiptr) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. */
    if (c->argc > 1) {
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);

    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
    } else if (server.aof_child_pid != -1) {
        if (schedule) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {
            addReplyError(c,
                "An AOF log rewriting in progress: can't BGSAVE right now. "
                "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
                "possible.");
        }
    } else if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function popultes 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related
 * information. */
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear
     * replid2. */
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write
         * command must generate a SELECT statement. */
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master
     * in order to fetch the currently selected DB. */
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master
     * is valid. */
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}

/* ----------------------------------------------------------------------------
 * Deep validation of DUMP payloads for RESTORE (sanitize-dump-payload).
 *
 * The normal RESTORE path validates the payload checksum and then lets
 * rdbLoadObject() build the value. Encoded values (ziplists, listpacks,
 * intsets, zipmaps) are stored as opaque byte strings and are only walked
 * when the key is accessed, so a payload with an inconsistent internal
 * structure can enter the dataset and crash the server later. This code
 * walks the serialized value without trusting any of its length fields and
 * rejects such payloads before rdbLoadObject() sees them.
 *
 * The walk is read-only, uses the same wire formats as rdbLoadObject(), and
 * returns C_ERR instead of terminating the server on malformed data.
 * ------------------------------------------------------------------------- */

typedef struct dumpCursor {
    const unsigned char *p;
    size_t len;
    size_t pos;
    int rdbver;                     /* RDB version from the DUMP footer. */
} dumpCursor;

typedef struct dumpString {
    const unsigned char *blob;  /* In-place payload bytes, or decompressed. */
    size_t len;
    unsigned char *alloc;       /* Owned decompression buffer, if any. */
} dumpString;

static uint32_t dumpReadU32LE(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t dumpReadU32BE(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t dumpReadU64BE(const unsigned char *p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static int dumpNeed(dumpCursor *c, size_t n) {
    return (c->pos <= c->len && n <= c->len - c->pos) ? C_OK : C_ERR;
}

static int dumpSkip(dumpCursor *c, size_t n) {
    if (dumpNeed(c, n) == C_ERR) return C_ERR;
    c->pos += n;
    return C_OK;
}

static int dumpByte(dumpCursor *c, unsigned char *b) {
    if (dumpNeed(c, 1) == C_ERR) return C_ERR;
    *b = c->p[c->pos++];
    return C_OK;
}

/* Read an RDB length. String encodings are not valid here. */
static int dumpLen(dumpCursor *c, uint64_t *len) {
    unsigned char b;
    if (dumpByte(c, &b) == C_ERR) return C_ERR;
    switch (b >> 6) {
    case RDB_6BITLEN:
        *len = b & 0x3F;
        return C_OK;
    case RDB_14BITLEN: {
        unsigned char b2;
        if (dumpByte(c, &b2) == C_ERR) return C_ERR;
        *len = ((uint64_t)(b & 0x3F) << 8) | b2;
        return C_OK;
    }
    default:
        if (b == RDB_32BITLEN) {
            if (dumpNeed(c, 4) == C_ERR) return C_ERR;
            *len = dumpReadU32BE(c->p + c->pos);
            c->pos += 4;
            return C_OK;
        } else if (b == RDB_64BITLEN) {
            if (dumpNeed(c, 8) == C_ERR) return C_ERR;
            *len = dumpReadU64BE(c->p + c->pos);
            c->pos += 8;
            return C_OK;
        }
        return C_ERR;
    }
}

static uint64_t dumpMaxStringLength(void) {
    return server.proto_max_bulk_len > 0 ?
           (uint64_t)server.proto_max_bulk_len : UINT64_MAX;
}

/* Read an RDB string. When keep is true the bytes are exposed to the caller;
 * when false the string is only bounds-checked and skipped. */
static int dumpReadString(dumpCursor *c, dumpString *s, int keep) {
    unsigned char b;

    s->blob = NULL;
    s->len = 0;
    s->alloc = NULL;
    if (dumpNeed(c, 1) == C_ERR) return C_ERR;
    b = c->p[c->pos];
    if ((b & 0xC0) == (RDB_ENCVAL << 6)) {
        int enc = b & 0x3F;
        c->pos++;
        if (enc == RDB_ENC_INT8) return dumpSkip(c, 1);
        if (enc == RDB_ENC_INT16) return dumpSkip(c, 2);
        if (enc == RDB_ENC_INT32) return dumpSkip(c, 4);
        if (enc == RDB_ENC_LZF) {
            uint64_t clen, ulen;
            if (dumpLen(c, &clen) == C_ERR || dumpLen(c, &ulen) == C_ERR)
                return C_ERR;
            if (ulen == 0 || ulen > dumpMaxStringLength()) return C_ERR;
            if (dumpNeed(c, (size_t)clen) == C_ERR) return C_ERR;
            if (keep) {
                unsigned char *out = zmalloc((size_t)ulen);
                if (lzf_decompress(c->p + c->pos, (unsigned int)clen,
                                   out, (unsigned int)ulen) != ulen)
                {
                    zfree(out);
                    return C_ERR;
                }
                s->blob = out;
                s->len = (size_t)ulen;
                s->alloc = out;
            }
            return dumpSkip(c, (size_t)clen);
        }
        return C_ERR;
    } else {
        uint64_t len;
        if (dumpLen(c, &len) == C_ERR) return C_ERR;
        if (len > dumpMaxStringLength()) return C_ERR;
        if (dumpNeed(c, (size_t)len) == C_ERR) return C_ERR;
        if (keep) {
            s->blob = c->p + c->pos;
            s->len = (size_t)len;
        }
        return dumpSkip(c, (size_t)len);
    }
}

static void dumpFreeString(dumpString *s) {
    if (s->alloc) zfree(s->alloc);
    s->alloc = NULL;
    s->blob = NULL;
    s->len = 0;
}

/* ---- Ziplist ------------------------------------------------------------ */

#define DUMP_ZIP_STR_06B (0 << 6)
#define DUMP_ZIP_STR_14B (1 << 6)
#define DUMP_ZIP_STR_32B (2 << 6)
#define DUMP_ZIP_INT_16B (0xc0)
#define DUMP_ZIP_INT_32B (0xd0)
#define DUMP_ZIP_INT_64B (0xe0)
#define DUMP_ZIP_INT_24B (0xf0)
#define DUMP_ZIP_INT_8B  (0xfe)

/* Validate a serialized ziplist and return its entry count. When even is
 * true the number of entries must be even (hash and sorted-set pairs). */
static int dumpZiplistValidate(const unsigned char *zl, size_t len, int even,
                               size_t *out_count)
{
    uint32_t bytes, tail;
    uint16_t hdr_count;
    size_t pos = 10, count = 0, last_start = 10;
    size_t prev_entry_len = 0;

    if (len < 11) return C_ERR;
    bytes = dumpReadU32LE(zl);
    tail = dumpReadU32LE(zl + 4);
    hdr_count = (uint16_t)(zl[8] | (zl[9] << 8));
    if (bytes != len) return C_ERR;
    if (tail > len) return C_ERR;

    while (1) {
        unsigned char p;
        size_t payload, hdr, slen = 0;
        size_t entry_start = pos;

        if (pos >= len) return C_ERR;
        if (zl[pos] == 0xFF) {
            if (pos != len - 1) return C_ERR;
            break;
        }

        p = zl[pos];
        if (p < 254) {
            payload = p;
            pos += 1;
        } else {
            if (pos + 5 > len) return C_ERR;
            payload = dumpReadU32LE(zl + pos + 1);
            pos += 5;
        }

        if (count == 0) {
            if (payload != 0) return C_ERR;
        } else if (payload != prev_entry_len) {
            return C_ERR;
        }

        if (pos >= len) return C_ERR;
        p = zl[pos];
        if ((p & 0xC0) == DUMP_ZIP_STR_06B) {
            hdr = 1;
            slen = p & 0x3F;
        } else if ((p & 0xC0) == DUMP_ZIP_STR_14B) {
            if (pos + 1 >= len) return C_ERR;
            hdr = 2;
            slen = ((size_t)(p & 0x3F) << 8) | zl[pos + 1];
        } else if ((p & 0xC0) == DUMP_ZIP_STR_32B) {
            if (pos + 4 >= len) return C_ERR;
            hdr = 5;
            slen = dumpReadU32BE(zl + pos + 1);
        } else {
            hdr = 1;
            if (p == DUMP_ZIP_INT_16B) hdr += 2;
            else if (p == DUMP_ZIP_INT_32B) hdr += 4;
            else if (p == DUMP_ZIP_INT_64B) hdr += 8;
            else if (p == DUMP_ZIP_INT_24B) hdr += 3;
            else if (p == DUMP_ZIP_INT_8B) hdr += 0;
            else if (p >= 0xF1 && p <= 0xFD) hdr += 0;
            else return C_ERR;
        }
        if (hdr > len - pos || slen > len - pos - hdr) return C_ERR;
        pos += hdr + slen;

        prev_entry_len = pos - entry_start;
        last_start = entry_start;
        count++;
    }

    if (count == 0) {
        if (tail != 10) return C_ERR;
    } else if (tail != (uint32_t)last_start) {
        return C_ERR;
    }
    if (count < UINT16_MAX) {
        if (hdr_count != count) return C_ERR;
    } else if (hdr_count != UINT16_MAX) {
        return C_ERR;
    }
    if (even && (count & 1)) return C_ERR;
    if (out_count) *out_count = count;
    return C_OK;
}

/* ---- Listpack ----------------------------------------------------------- */

#define DUMP_LP_HDR_SIZE 6
#define DUMP_LP_EOF 0xFF

static size_t dumpLpBacklenSize(uint64_t l) {
    if (l <= 127) return 1;
    if (l < 16383) return 2;
    if (l < 2097151) return 3;
    if (l < 268435455) return 4;
    return 5;
}

static void dumpLpBacklenBytes(uint64_t l, unsigned char *buf) {
    if (l <= 127) {
        buf[0] = (unsigned char)l;
    } else if (l < 16383) {
        buf[0] = (unsigned char)(l >> 7);
        buf[1] = (unsigned char)((l & 127) | 128);
    } else if (l < 2097151) {
        buf[0] = (unsigned char)(l >> 14);
        buf[1] = (unsigned char)(((l >> 7) & 127) | 128);
        buf[2] = (unsigned char)((l & 127) | 128);
    } else if (l < 268435455) {
        buf[0] = (unsigned char)(l >> 21);
        buf[1] = (unsigned char)(((l >> 14) & 127) | 128);
        buf[2] = (unsigned char)(((l >> 7) & 127) | 128);
        buf[3] = (unsigned char)((l & 127) | 128);
    } else {
        buf[0] = (unsigned char)(l >> 28);
        buf[1] = (unsigned char)(((l >> 21) & 127) | 128);
        buf[2] = (unsigned char)(((l >> 14) & 127) | 128);
        buf[3] = (unsigned char)(((l >> 7) & 127) | 128);
        buf[4] = (unsigned char)((l & 127) | 128);
    }
}

/* Validate a serialized listpack and return its element count. */
static int dumpListpackValidate(const unsigned char *lp, size_t len,
                                size_t *out_count)
{
    uint32_t bytes;
    uint16_t hdr_count;
    size_t pos = DUMP_LP_HDR_SIZE, count = 0;

    if (len < DUMP_LP_HDR_SIZE + 1) return C_ERR;
    bytes = dumpReadU32LE(lp);
    hdr_count = (uint16_t)(lp[4] | (lp[5] << 8));
    if (bytes != len) return C_ERR;

    while (1) {
        unsigned char b;
        size_t entrylen, backlen_size;
        unsigned char expected[5];

        if (pos >= len) return C_ERR;
        b = lp[pos];
        if (b == DUMP_LP_EOF) {
            if (pos != len - 1) return C_ERR;
            break;
        }
        if ((b & 0x80) == 0x00) {
            entrylen = 1;                                  /* 7 bit uint */
        } else if ((b & 0xC0) == 0x80) {
            entrylen = 1 + (b & 0x3F);                     /* 6 bit string */
        } else if ((b & 0xE0) == 0xC0) {
            entrylen = 2;                                  /* 13 bit int */
        } else if ((b & 0xF0) == 0xE0) {
            if (pos + 1 >= len) return C_ERR;
            entrylen = 2 + ((((size_t)b & 0x0F) << 8) | lp[pos + 1]);
        } else if (b == 0xF0) {
            if (pos + 4 >= len) return C_ERR;
            entrylen = 5 + dumpReadU32LE(lp + pos + 1);
        } else if (b == 0xF1) {
            entrylen = 3;
        } else if (b == 0xF2) {
            entrylen = 4;
        } else if (b == 0xF3) {
            entrylen = 5;
        } else if (b == 0xF4) {
            entrylen = 9;
        } else {
            return C_ERR;                                  /* reserved */
        }
        if (entrylen > len - pos) return C_ERR;
        backlen_size = dumpLpBacklenSize(entrylen);
        if (backlen_size > len - pos - entrylen) return C_ERR;
        dumpLpBacklenBytes(entrylen, expected);
        if (memcmp(lp + pos + entrylen, expected, backlen_size) != 0)
            return C_ERR;
        pos += entrylen + backlen_size;
        count++;
    }

    if (count < UINT16_MAX) {
        if (hdr_count != count) return C_ERR;
    } else if (hdr_count != UINT16_MAX) {
        return C_ERR;
    }
    if (out_count) *out_count = count;
    return C_OK;
}

/* ---- Intset ------------------------------------------------------------- */

static int dumpIntsetValidate(const unsigned char *is, size_t len) {
    uint32_t encoding, count, i;
    PORT_LONGLONG prev = 0;
    int have_prev = 0;

    if (len < 8) return C_ERR;
    encoding = dumpReadU32LE(is);
    count = dumpReadU32LE(is + 4);
    if (encoding != 2 && encoding != 4 && encoding != 8) return C_ERR;
    if ((uint64_t)count * encoding != len - 8) return C_ERR;
    for (i = 0; i < count; i++) {
        const unsigned char *p = is + 8 + (size_t)i * encoding;
        PORT_LONGLONG v;
        if (encoding == 2) {
            v = (PORT_LONGLONG)(int16_t)(uint16_t)(p[0] | (p[1] << 8));
        } else if (encoding == 4) {
            v = (PORT_LONGLONG)(int32_t)dumpReadU32LE(p);
        } else {
            uint64_t raw = dumpReadU32LE(p) |
                           ((uint64_t)dumpReadU32LE(p + 4) << 32);
            v = (PORT_LONGLONG)raw;
        }
        if (have_prev && v <= prev) return C_ERR;
        prev = v;
        have_prev = 1;
    }
    return C_OK;
}

/* ---- Zipmap ------------------------------------------------------------- */

/* Decode a zipmap length at *pos. Returns (size_t)-1 on error or terminator. */
static size_t dumpZipmapLength(const unsigned char *zm, size_t len,
                               size_t *pos)
{
    unsigned char b;
    if (*pos >= len) return (size_t)-1;
    b = zm[(*pos)++];
    if (b < 254) return b;
    if (b == 254) {
        unsigned int v;
        if (len - *pos < sizeof(v)) return (size_t)-1;
        memcpy(&v, zm + *pos, sizeof(v));
        *pos += sizeof(v);
        return v;
    }
    return (size_t)-1;
}

static int dumpZipmapValidate(const unsigned char *zm, size_t len) {
    size_t pos = 1, count = 0;
    unsigned char zmlen;

    if (len < 2) return C_ERR;
    zmlen = zm[0];
    while (pos < len) {
        size_t kl, vl;
        if (zm[pos] == 0xFF) {
            if (pos != len - 1) return C_ERR;
            break;
        }
        kl = dumpZipmapLength(zm, len, &pos);
        if (kl == (size_t)-1 || kl > len - pos) return C_ERR;
        pos += kl;
        vl = dumpZipmapLength(zm, len, &pos);
        if (vl == (size_t)-1 || vl > len - pos) return C_ERR;
        pos += vl;
        if (pos >= len || zm[pos] != 0xFF) return C_ERR;
        pos++;
        count++;
    }
    if (zmlen != 254 && zmlen != count) return C_ERR;
    return C_OK;
}

/* ---- Module values ------------------------------------------------------ */

/* Walks the opaque opcode stream of a module value. Only modules that are
 * currently loaded can produce a loadable value, so an unknown module ID is
 * rejected here (rdbLoadObject would otherwise abort the server). */
static int dumpModuleValidate(dumpCursor *c) {
    uint64_t moduleid, opcode;
    dumpString s;

    if (dumpLen(c, &moduleid) == C_ERR) return C_ERR;
    if (moduleTypeLookupModuleByID(moduleid) == NULL) return C_ERR;
    while (1) {
        if (dumpLen(c, &opcode) == C_ERR) return C_ERR;
        if (opcode == RDB_MODULE_OPCODE_EOF) break;
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t v;
            if (dumpLen(c, &v) == C_ERR) return C_ERR;
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            if (dumpSkip(c, 4) == C_ERR) return C_ERR;
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            if (dumpSkip(c, 8) == C_ERR) return C_ERR;
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
        } else {
            return C_ERR;
        }
    }
    return C_OK;
}

/* ---- Object payloads ---------------------------------------------------- */

static int dumpValidateBlob(int rdbtype, const dumpString *s) {
    size_t count = 0;

    switch (rdbtype) {
    case RDB_TYPE_HASH_ZIPMAP:
        return dumpZipmapValidate(s->blob, s->len);
    case RDB_TYPE_LIST_ZIPLIST:
        return dumpZiplistValidate(s->blob, s->len, 0, &count);
    case RDB_TYPE_SET_INTSET:
        return dumpIntsetValidate(s->blob, s->len);
    case RDB_TYPE_ZSET_ZIPLIST:
    case RDB_TYPE_HASH_ZIPLIST:
        return dumpZiplistValidate(s->blob, s->len, 1, &count);
    default:
        return C_ERR;
    }
}

static int dumpValidateObject(dumpCursor *c, int rdbtype) {
    uint64_t len, i;
    dumpString s;

    switch (rdbtype) {
    case RDB_TYPE_STRING:
        return dumpReadString(c, &s, 0);
    case RDB_TYPE_LIST:
    case RDB_TYPE_SET:
        if (dumpLen(c, &len) == C_ERR) return C_ERR;
        for (i = 0; i < len; i++)
            if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
        return C_OK;
    case RDB_TYPE_ZSET:
    case RDB_TYPE_ZSET_2:
        if (dumpLen(c, &len) == C_ERR) return C_ERR;
        for (i = 0; i < len; i++) {
            unsigned char dlen;
            if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (dumpSkip(c, 8) == C_ERR) return C_ERR;
            } else {
                if (dumpByte(c, &dlen) == C_ERR) return C_ERR;
                if (dlen < 253 && dumpSkip(c, dlen) == C_ERR) return C_ERR;
            }
        }
        return C_OK;
    case RDB_TYPE_HASH:
        if (dumpLen(c, &len) == C_ERR) return C_ERR;
        for (i = 0; i < len; i++) {
            if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
            if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
        }
        return C_OK;
    case RDB_TYPE_HASH_ZIPMAP:
    case RDB_TYPE_LIST_ZIPLIST:
    case RDB_TYPE_SET_INTSET:
    case RDB_TYPE_ZSET_ZIPLIST:
    case RDB_TYPE_HASH_ZIPLIST: {
        int ret;
        if (dumpReadString(c, &s, 1) == C_ERR) return C_ERR;
        ret = dumpValidateBlob(rdbtype, &s);
        dumpFreeString(&s);
        return ret;
    }
    case RDB_TYPE_LIST_QUICKLIST:
        if (dumpLen(c, &len) == C_ERR) return C_ERR;
        if (len == 0) return C_ERR;
        for (i = 0; i < len; i++) {
            size_t count;
            int ok;
            if (dumpReadString(c, &s, 1) == C_ERR) return C_ERR;
            ok = (dumpZiplistValidate(s.blob, s.len, 0, &count) == C_OK &&
                  count > 0);
            dumpFreeString(&s);
            if (!ok) return C_ERR;
        }
        return C_OK;
    case RDB_TYPE_STREAM_LISTPACKS: {
        uint64_t listpacks, length, last_ms, last_seq, cgroups;
        uint64_t j;
        int stream_counters_present = 0;
        if (dumpLen(c, &listpacks) == C_ERR) return C_ERR;
        for (i = 0; i < listpacks; i++) {
            size_t count;
            int ok;
            if (dumpReadString(c, &s, 1) == C_ERR) return C_ERR;
            ok = (s.len == sizeof(streamID));
            dumpFreeString(&s);
            if (!ok) return C_ERR;
            if (dumpReadString(c, &s, 1) == C_ERR) return C_ERR;
            ok = (dumpListpackValidate(s.blob, s.len, &count) == C_OK &&
                  count > 0);
            dumpFreeString(&s);
            if (!ok) return C_ERR;
        }
        if (dumpLen(c, &length) == C_ERR) return C_ERR;
        if (dumpLen(c, &last_ms) == C_ERR) return C_ERR;
        if (dumpLen(c, &last_seq) == C_ERR) return C_ERR;
        /* The local stream extension starts with a fixed 9-byte 64-bit
         * length: RDB_64BITLEN plus the sentinel. Peek instead of consuming
         * so old snapshots keep their group count at this position. */
        if (dumpNeed(c, 9) == C_OK && c->p[c->pos] == RDB_64BITLEN &&
            dumpReadU64BE(c->p + c->pos + 1) == STREAM_RDB_EXT_MARK)
        {
            uint64_t entries_added, maxdel_ms, maxdel_seq;
            c->pos += 9;
            if (dumpLen(c, &entries_added) == C_ERR) return C_ERR;
            if (dumpLen(c, &maxdel_ms) == C_ERR) return C_ERR;
            if (dumpLen(c, &maxdel_seq) == C_ERR) return C_ERR;
            stream_counters_present = 1;
        }
        if (dumpLen(c, &cgroups) == C_ERR) return C_ERR;
        for (i = 0; i < cgroups; i++) {
            uint64_t ms, seq, pel, consumers;
            if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
            if (dumpLen(c, &ms) == C_ERR) return C_ERR;
            if (dumpLen(c, &seq) == C_ERR) return C_ERR;
            if (stream_counters_present) {
                uint64_t marker, entries_read;
                if (dumpLen(c, &marker) == C_ERR) return C_ERR;
                if (marker != STREAM_RDB_EXT_MARK) return C_ERR;
                if (dumpLen(c, &entries_read) == C_ERR) return C_ERR;
            }
            if (dumpLen(c, &pel) == C_ERR) return C_ERR;
            if (pel > (uint64_t)c->len) return C_ERR;
            for (j = 0; j < pel; j++) {
                uint64_t delivery_count;
                if (dumpSkip(c, sizeof(streamID) + 8) == C_ERR) return C_ERR;
                if (dumpLen(c, &delivery_count) == C_ERR) return C_ERR;
            }
            if (dumpLen(c, &consumers) == C_ERR) return C_ERR;
            if (consumers > (uint64_t)c->len) return C_ERR;
            for (j = 0; j < consumers; j++) {
                uint64_t pel_size;
                if (dumpReadString(c, &s, 0) == C_ERR) return C_ERR;
                if (dumpSkip(c, 8) == C_ERR) return C_ERR;
                if (c->rdbver >= 11 && dumpLen(c, &pel_size) == C_ERR)
                    return C_ERR; /* active_time */
                if (dumpLen(c, &pel_size) == C_ERR) return C_ERR;
                if (pel_size > (uint64_t)c->len) return C_ERR;
                if (dumpSkip(c, (size_t)pel_size * sizeof(streamID)) == C_ERR)
                    return C_ERR;
            }
        }
        return C_OK;
    }
    case RDB_TYPE_MODULE:
        return C_ERR; /* Pre-module-v2 values cannot be parsed. */
    case RDB_TYPE_MODULE_2:
        return dumpModuleValidate(c);
    default:
        return C_ERR;
    }
}

/* Deep-validate the value of a RESTORE payload. The payload layout is
 * <type> <value> <rdbver:2> <crc64:8>; the footer is excluded here because
 * verifyDumpPayload() has already checked it. Returns C_OK when the value is
 * safe to load, C_ERR when it is malformed. */
int rdbSanitizeDumpPayload(const unsigned char *payload, size_t len) {
    dumpCursor c;
    unsigned char type;

    if (len < 10) return C_ERR;
    c.p = payload;
    c.len = len - 10;
    c.pos = 0;
    c.rdbver = (payload[len-9] << 8) | payload[len-10];

    if (dumpByte(&c, &type) == C_ERR) return C_ERR;
    if (!rdbIsObjectType(type)) return C_ERR;
    if (dumpValidateObject(&c, type) == C_ERR) return C_ERR;
    if (c.pos != c.len) return C_ERR;
    return C_OK;
}
