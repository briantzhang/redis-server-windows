/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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
#include "Win32_Interop/Win32_Portability.h"
#include "Win32_Interop/win32_types.h"
#include "Win32_Interop/Win32_Error.h"
#include "Win32_Interop/win32fixes.h"
#endif

#include "server.h"
#include <sys/stat.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#define AOF_CHECK_LINE_MAX 1024

#define ERROR(...) { \
    char __buf[1024]; \
    snprintf(__buf, sizeof(__buf), __VA_ARGS__); \
    snprintf(error, sizeof(error), "0x%16llx: %s", (PORT_LONGLONG)epos, __buf); \
}

static char error[1044];
static off_t epos;
static int check_errors;
static int check_fixed;

/* When running as a standalone tool, server.aof_* still holds the compiled-in
 * defaults. initServerConfig() allocates those strings and the shared objects
 * needed by the RDB preamble checker without starting an event loop. */
static void initCheckConfig(void) {
    initServerConfig();
    if (shared.integers[0] == NULL) createSharedObjects();
    server.loading_process_events_interval_bytes = 0;
}

/* Directory arguments are treated as a multi-part AOF: read the manifest and
 * check every referenced file in order. Manifest parsing here is deliberately
 * independent of the server's in-memory manifest, so a broken manifest is
 * reported as a check error instead of being silently repaired. */
static int readManifest(const char *dir, const char *name, list *files) {
    sds manifest_path = sdscatprintf(sdsempty(),"%s/%s.manifest",dir,name);
    char line[AOF_CHECK_LINE_MAX];
    FILE *fp = fopen(manifest_path,"r");
    int seen_incr = 0, retval = C_ERR;
    size_t prefix = strlen(name);

    if (fp == NULL) {
        printf("Cannot open AOF manifest: %s\n", manifest_path);
        goto cleanup;
    }
    while (fgets(line,sizeof(line),fp) != NULL) {
        char file[AOF_CHECK_LINE_MAX];
        PORT_LONGLONG seq;
        char type;
        char *p = line, *end;
        sds path;

        while (*p == ' ' || *p == '\t') p++;
        end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = '\0';
        if (*p == '\0' || *p == '#') continue;
        if (sscanf(p,"file %1023s seq %lld type %c",file,&seq,&type) != 3 ||
            seq <= 0 || strncmp(file,name,prefix) != 0 || file[prefix] != '.' ||
            strchr(file,'/') != NULL || strchr(file,'\\') != NULL ||
            (type != 'b' && type != 'i'))
        {
            printf("Invalid line in AOF manifest %s: %s\n", manifest_path, p);
            goto cleanup;
        }
        if (type == 'b' && listLength(files) != 0) {
            printf("AOF manifest %s lists a base file out of order\n", manifest_path);
            goto cleanup;
        }
        if (type == 'i') seen_incr = 1;
        path = sdscatprintf(sdsempty(),"%s/%s",dir,file);
        listAddNodeTail(files,path);
    }
    if (!seen_incr) {
        printf("AOF manifest %s lists no incremental file\n", manifest_path);
        goto cleanup;
    }
    retval = C_OK;
cleanup:
    sdsfree(manifest_path);
    if (fp) fclose(fp);
    return retval;
}

static void freePathList(list *files) {
    listSetFreeMethod(files,sdsfree);
    listRelease(files);
}

static int consumeNewline(char *buf) {
    if (strncmp(buf,"\r\n",2) != 0) {
        ERROR("Expected \\r\\n, got: %02x%02x",buf[0],buf[1]);
        return 0;
    }
    return 1;
}

int readLong(FILE *fp, char prefix, PORT_LONG *target) {
    char buf[128], *eptr;
    epos = ftello(fp);
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        return 0;
    }
    if (buf[0] != prefix) {
        ERROR("Expected prefix '%c', got: '%c'",prefix,buf[0]);
        return 0;
    }
    *target = strtol(buf+1,&eptr,10);
    return consumeNewline(eptr);
}

int readBytes(FILE *fp, char *target, PORT_LONG length) {
    PORT_LONG real;
    epos = ftello(fp);
    real = (PORT_LONG)fread(target,1,length,fp);
    if (real != length) {
        ERROR("Expected to read %Id bytes, got %Id bytes",length,real);                        WIN_PORT_FIX /* %ld -> %Id */
        return 0;
    }
    return 1;
}

int readString(FILE *fp, char** target) {
    PORT_LONG len;
    *target = NULL;
    if (!readLong(fp,'$',&len)) {
        return 0;
    }

    /* Increase length to also consume \r\n */
    len += 2;
    *target = (char*)zmalloc(len);
    if (!readBytes(fp,*target,len)) {
        return 0;
    }
    if (!consumeNewline(*target+len-2)) {
        return 0;
    }
    (*target)[len-2] = '\0';
    return 1;
}

int readArgc(FILE *fp, PORT_LONG *target) {
    return readLong(fp,'*',target);
}

off_t process(FILE *fp) {
    PORT_LONG argc;
    off_t pos = 0;
    int i, multi = 0;
    char *str;

    while(1) {
        if (!multi) pos = ftello(fp);
        if (!readArgc(fp, &argc)) break;

        for (i = 0; i < argc; i++) {
            if (!readString(fp,&str)) break;
            if (i == 0) {
                if (strcasecmp(str, "multi") == 0) {
                    if (multi++) {
                        ERROR("Unexpected MULTI");
                        break;
                    }
                } else if (strcasecmp(str, "exec") == 0) {
                    if (--multi) {
                        ERROR("Unexpected EXEC");
                        break;
                    }
                }
            }
            zfree(str);
        }

        /* Stop if the loop did not finish */
        if (i < argc) {
            if (str) zfree(str);
            break;
        }
    }

    if (feof(fp) && multi && strlen(error) == 0) {
        ERROR("Reached EOF before reading EXEC for MULTI");
    }
    if (strlen(error) > 0) {
        printf("%s\n", error);
    }
    return pos;
}

static void checkSingleFile(const char *filename, int fix) {
    char *check_argv[2] = { (char*)filename, NULL };
    int check_argc = 2;
    FILE *fp = fopen(filename, IF_WIN32("r+b", "r+"));
    if (fp == NULL) {
        printf("Cannot open file: %s\n", filename);
        exit(1);
    }

    struct redis_stat sb;
    if (redis_fstat(fileno(fp),&sb) == -1) {
        printf("Cannot stat file: %s\n", filename);
        exit(1);
    }

    off_t size = sb.st_size;
    if (size == 0) {
        printf("Empty file: %s\n", filename);
        exit(1);
    }

    /* This AOF file may have an RDB preamble. Check this to start, and if this
     * is the case, start processing the RDB part. */
    if (size >= 8) {    /* There must be at least room for the RDB header. */
        char sig[5];
        int has_preamble = fread(sig,sizeof(sig),1,fp) == 1 &&
                            memcmp(sig,"REDIS",sizeof(sig)) == 0;
        rewind(fp);
        if (has_preamble) {
            printf("The AOF appears to start with an RDB preamble.\n"
                   "Checking the RDB preamble to start:\n");
            if (redis_check_rdb_main(check_argc,check_argv,fp) == C_ERR) {
                printf("RDB preamble of AOF file is not sane, aborting.\n");
                exit(1);
            } else {
                printf("RDB preamble is OK, proceeding with AOF tail...\n");
            }
        }
    }

    off_t pos = process(fp);
    off_t diff = size-pos;
    printf("AOF analyzed: size=%lld, ok_up_to=%lld, diff=%lld\n",
        (PORT_LONGLONG) size, (PORT_LONGLONG) pos, (PORT_LONGLONG) diff);
    if (diff > 0) {
        if (fix) {
            char buf[2];
            printf("This will shrink the AOF from %lld bytes, with %lld bytes, to %lld bytes\n", (PORT_LONGLONG)size, (PORT_LONGLONG)diff, (PORT_LONGLONG)pos);
            printf("Continue? [y/N]: ");
            if (fgets(buf,sizeof(buf),stdin) == NULL ||
                strncasecmp(buf,"y",1) != 0) {
                    printf("Aborting...\n");
                    exit(1);
            }
            if (ftruncate(fileno(fp), pos) == -1) {
                printf("Failed to truncate AOF\n");
                check_errors++;
            } else {
                printf("Successfully truncated AOF\n");
                check_fixed++;
            }
        } else {
            printf("AOF is not valid. "
                   "Use the --fix option to try fixing it.\n");
            check_errors++;
        }
    } else if (strlen(error) > 0) {
        check_errors++;
    }

    fclose(fp);
}

int redis_check_aof_main(int argc, char **argv) {
    char *filename;
    int fix = 0, is_dir = 0;
#ifdef _WIN32
    _fmode = _O_BINARY;
    setmode(_fileno(stdin), _O_BINARY);
    setmode(_fileno(stdout), _O_BINARY);
    setmode(_fileno(stderr), _O_BINARY);
#endif

    if (argc < 2) {
        printf("Usage: %s [--fix] <file.aof|directory>\n", argv[0]);
        exit(1);
    } else if (argc == 2) {
        filename = argv[1];
    } else if (argc == 3) {
        if (strcmp(argv[1],"--fix") != 0) {
            printf("Invalid argument: %s\n", argv[1]);
            exit(1);
        }
        filename = argv[2];
        fix = 1;
    } else {
        printf("Invalid arguments\n");
        exit(1);
    }

    initCheckConfig();

    {
        struct redis_stat sb;
        if (redis_stat(filename,&sb) != 0) {
            printf("Cannot open file: %s\n", filename);
            exit(1);
        }
#ifndef _WIN32
        is_dir = S_ISDIR(sb.st_mode);
#else
        is_dir = (sb.st_mode & _S_IFMT) == _S_IFDIR;
#endif
    }

    if (is_dir) {
        list *files = listCreate();
        listNode *ln;
        listIter li;
        int i = 0;

        if (readManifest(filename,server.aof_filename,files) != C_OK) {
            freePathList(files);
            exit(1);
        }
        listRewind(files,&li);
        while ((ln = listNext(&li)) != NULL) {
            sds path = listNodeValue(ln);
            printf("Checking AOF part %d: %s\n", ++i, path);
            error[0] = '\0';

            /* checkSingleFile() needs the same argv[1] as a single-file run for
             * the RDB preamble checker. Temporarily point it at this part. */
            argv[1] = path;
            checkSingleFile(path, fix && i == listLength(files));
        }
        freePathList(files);
        printf("AOF parts checked: %d, errors: %d, fixed: %d\n",
            i, check_errors, check_fixed);
    } else {
        checkSingleFile(filename, fix);
    }

    if (check_errors > 0) exit(1);
    printf("AOF is valid\n");
    exit(0);
}
