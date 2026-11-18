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
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <dirent.h>
#else
#include "Win32_Interop/Win32_QFork.h"
#include <direct.h>
#define MAXPATHLEN 1024
#endif

void aofUpdateCurrentSize(void);
void aofClosePipes(void);

/* ----------------------------------------------------------------------------
 * Multi-part AOF manifest
 *
 * The AOF lives in a directory (appenddirname) as one base file plus one or
 * more incremental files. The manifest, <appendfilename>.manifest, lists the
 * files to load in order and is replaced atomically after every rewrite.
 *
 * An AOF written by an older build (a single <appendfilename> file in the
 * working directory) is still loaded, and the first rewrite migrates it to
 * the multi-part layout.
 * ------------------------------------------------------------------------- */

#define AOF_MANIFEST_LINE_MAX 1024

static void aofInfoFree(void *ptr) {
    aofInfo *ai = ptr;

    if (ai == NULL) return;
    sdsfree(ai->file_name);
    zfree(ai);
}

static aofInfo *aofInfoCreate(const char *name, PORT_LONGLONG seq, int type) {
    aofInfo *ai = zcalloc(sizeof(*ai));

    ai->file_name = sdsnew(name);
    ai->seq = seq;
    ai->type = type;
    return ai;
}

static sds aofGetManifestPath(void) {
    return sdscatprintf(sdsempty(),"%s/%s.manifest",
        server.aof_dirname, server.aof_filename);
}

static sds aofGetAofPath(const char *file_name) {
    return sdscatprintf(sdsempty(),"%s/%s",server.aof_dirname,file_name);
}

static sds aofGetBaseFileName(PORT_LONGLONG seq) {
    return sdscatprintf(sdsempty(),"%s.%lld.base.%s",server.aof_filename,
        (long long)seq, server.aof_use_rdb_preamble ? "rdb" : "aof");
}

static sds aofGetIncrFileName(PORT_LONGLONG seq) {
    return sdscatprintf(sdsempty(),"%s.%lld.incr.aof",server.aof_filename,
        (long long)seq);
}

static aofManifest *aofManifestCreate(void) {
    aofManifest *am = zcalloc(sizeof(*am));

    am->incr_aof_list = listCreate();
    am->dirname = sdsnew(server.aof_dirname);
    am->file_name = sdsnew(server.aof_filename);
    am->manifest_filepath = aofGetManifestPath();
    return am;
}

void aofFreeManifest(aofManifest *am) {
    if (am == NULL) return;
    if (am->base_aof_info) aofInfoFree(am->base_aof_info);
    listSetFreeMethod(am->incr_aof_list,aofInfoFree);
    listRelease(am->incr_aof_list);
    sdsfree(am->dirname);
    sdsfree(am->file_name);
    sdsfree(am->manifest_filepath);
    zfree(am);
}

static int aofManifestAddFile(aofManifest *am, const char *name,
                              PORT_LONGLONG seq, int type)
{
    if (type == AOF_FILE_TYPE_BASE) {
        if (am->base_aof_info != NULL) return C_ERR;
        am->base_aof_info = aofInfoCreate(name,seq,type);
        if (seq > am->curr_base_file_seq) am->curr_base_file_seq = seq;
    } else {
        aofInfo *ai = aofInfoCreate(name,seq,type);
        listAddNodeTail(am->incr_aof_list,ai);
        if (seq > am->curr_incr_file_seq) am->curr_incr_file_seq = seq;
    }
    return C_OK;
}

static int aofManifestHasFile(const char *name) {
    aofManifest *am = server.aof_manifest;
    listIter li;
    listNode *ln;

    if (am == NULL) return 0;
    if (am->base_aof_info &&
        strcmp(am->base_aof_info->file_name,name) == 0) return 1;
    listRewind(am->incr_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = listNodeValue(ln);
        if (strcmp(ai->file_name,name) == 0) return 1;
    }
    return 0;
}

/* Write the in-memory manifest to <appendfilename>.manifest. The file is
 * written to a temporary name and renamed into place, so a crash leaves
 * either the old or the new manifest. */
int aofRewriteManifest(void) {
    aofManifest *am = server.aof_manifest;
    sds tmpfile;
    FILE *fp;
    listIter li;
    listNode *ln;

    if (am == NULL) return C_ERR;
    tmpfile = sdscatprintf(sdsempty(),"%s.tmp-%d",
        am->manifest_filepath,(int)getpid());
    fp = fopen(tmpfile,"wb");
    if (fp == NULL) {
        serverLog(LL_WARNING,"Can't open the AOF manifest %s for writing: %s",
            tmpfile, IF_WIN32(wsa_strerror(errno), strerror(errno)));
        sdsfree(tmpfile);
        return C_ERR;
    }
    if (am->base_aof_info) {
        fprintf(fp,"file %s seq %lld type b\n",
            am->base_aof_info->file_name,(long long)am->base_aof_info->seq);
    }
    listRewind(am->incr_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = listNodeValue(ln);
        fprintf(fp,"file %s seq %lld type i\n",
            ai->file_name,(long long)ai->seq);
    }
    if (fflush(fp) == EOF || fsync(fileno(fp)) == -1) {
        serverLog(LL_WARNING,"Can't flush the AOF manifest %s: %s",
            tmpfile, IF_WIN32(wsa_strerror(errno), strerror(errno)));
        fclose(fp);
        unlink(tmpfile);
        sdsfree(tmpfile);
        return C_ERR;
    }
    fclose(fp);
    if (rename(tmpfile,am->manifest_filepath) == -1) {
        serverLog(LL_WARNING,"Can't rename the AOF manifest %s to %s: %s",
            tmpfile, am->manifest_filepath,
            IF_WIN32(wsa_strerror(errno), strerror(errno)));
        unlink(tmpfile);
        sdsfree(tmpfile);
        return C_ERR;
    }
    sdsfree(tmpfile);
    return C_OK;
}

static int aofManifestParseLine(char *line, aofManifest *am) {
    char name[AOF_MANIFEST_LINE_MAX];
    long long seq = 0;
    char type = 0;
    size_t prefix = strlen(server.aof_filename);

    if (sscanf(line,"file %1023s seq %lld type %c",name,&seq,&type) != 3)
        return C_ERR;
    if (seq <= 0) return C_ERR;
    if (strncmp(name,server.aof_filename,prefix) != 0 ||
        name[prefix] != '.' ||
        strchr(name,'/') != NULL ||
        strchr(name,'\\') != NULL) return C_ERR;
    if (type == 'b')
        return aofManifestAddFile(am,name,(PORT_LONGLONG)seq,
                                  AOF_FILE_TYPE_BASE);
    if (type == 'i')
        return aofManifestAddFile(am,name,(PORT_LONGLONG)seq,
                                  AOF_FILE_TYPE_INCR);
    return C_ERR;
}

static int aofManifestRead(void) {
    aofManifest *am = server.aof_manifest;
    char line[AOF_MANIFEST_LINE_MAX];
    int lineno = 0;
    FILE *fp;

    fp = fopen(am->manifest_filepath,"r");
    if (fp == NULL) return C_ERR;
    while (fgets(line,sizeof(line),fp) != NULL) {
        char *p = line;
        size_t len;

        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' ||
                           p[len-1] == ' ' || p[len-1] == '\t'))
        {
            p[--len] = '\0';
        }
        if (len == 0 || p[0] == '#') continue;
        if (aofManifestParseLine(p,am) != C_OK) {
            serverLog(LL_WARNING,"Invalid line %d in AOF manifest %s: %s",
                lineno, am->manifest_filepath, p);
            fclose(fp);
            return C_ERR;
        }
    }
    fclose(fp);
    if (listLength(am->incr_aof_list) == 0) {
        serverLog(LL_WARNING,"AOF manifest %s lists no incremental file",
            am->manifest_filepath);
        return C_ERR;
    }
    return C_OK;
}

static int aofEnsureDir(void) {
    struct redis_stat sb;

    if (redis_stat(server.aof_dirname,&sb) == 0) return C_OK;
#ifdef _WIN32
    if (_mkdir(server.aof_dirname) == -1) return C_ERR;
#else
    if (mkdir(server.aof_dirname,0755) == -1) return C_ERR;
#endif
    return C_OK;
}

static int aofCreateEmptyFile(const char *file_name) {
    sds path = aofGetAofPath(file_name);
    int fd;

    fd = open(path,O_WRONLY|O_CREAT|O_TRUNC WIN32_ONLY(|_O_BINARY),
              IF_WIN32(_S_IREAD|_S_IWRITE,0644));
    sdsfree(path);
    if (fd == -1) return C_ERR;
    close(fd);
    return C_OK;
}

/* Remove files inside appenddirname that the manifest does not reference,
 * for example the base file of a rewrite that crashed before the manifest
 * switch. Only names starting with "<appendfilename>." are considered. */
void aofCleanupUnusedFiles(void) {
    aofManifest *am = server.aof_manifest;
    sds prefix, manifest_name;

    if (am == NULL) return;
    prefix = sdscatprintf(sdsempty(),"%s.",server.aof_filename);
    manifest_name = sdscatprintf(sdsempty(),"%s.manifest",server.aof_filename);
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        sds pattern = sdscatprintf(sdsempty(),"%s\\%s.*",
            server.aof_dirname,server.aof_filename);

        h = FindFirstFileA(pattern,&fd);
        sdsfree(pattern);
        if (h == INVALID_HANDLE_VALUE) {
            sdsfree(prefix);
            sdsfree(manifest_name);
            return;
        }
        do {
            sds path;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (strncmp(fd.cFileName,prefix,sdslen(prefix)) != 0) continue;
            if (aofManifestHasFile(fd.cFileName)) continue;
            if (strcmp(fd.cFileName,manifest_name) == 0) continue;
            path = aofGetAofPath(fd.cFileName);
            unlink(path);
            sdsfree(path);
        } while (FindNextFileA(h,&fd));
        FindClose(h);
    }
#else
    {
        DIR *dir = opendir(server.aof_dirname);
        struct dirent *de;

        if (dir == NULL) {
            sdsfree(prefix);
            sdsfree(manifest_name);
            return;
        }
        while ((de = readdir(dir)) != NULL) {
            sds path;
            if (strncmp(de->d_name,prefix,sdslen(prefix)) != 0) continue;
            if (aofManifestHasFile(de->d_name)) continue;
            if (strcmp(de->d_name,manifest_name) == 0) continue;
            path = aofGetAofPath(de->d_name);
            unlink(path);
            sdsfree(path);
        }
        closedir(dir);
    }
#endif
    sdsfree(prefix);
    sdsfree(manifest_name);
}

/* Load (or create) the manifest for an AOF-enabled server. When a legacy
 * single-file AOF is found instead, aof_use_legacy_file is set so that the
 * regular load path reads it, and a rewrite is scheduled to migrate it. */
int aofLoadManifestFromDisk(void) {
    struct redis_stat sb;
    sds manifest_path;
    int manifest_exists, legacy_exists;

    if (server.aof_manifest) {
        aofFreeManifest(server.aof_manifest);
        server.aof_manifest = NULL;
    }
    server.aof_use_legacy_file = 0;

    manifest_path = aofGetManifestPath();
    manifest_exists = (redis_stat(manifest_path,&sb) == 0);
    sdsfree(manifest_path);
    legacy_exists = (redis_stat(server.aof_filename,&sb) == 0);

    if (manifest_exists) {
        if (legacy_exists) {
            serverLog(LL_NOTICE,
                "AOF manifest found, ignoring the legacy file %s",
                server.aof_filename);
        }
        server.aof_manifest = aofManifestCreate();
        if (aofManifestRead() != C_OK) {
            aofFreeManifest(server.aof_manifest);
            server.aof_manifest = NULL;
            return C_ERR;
        }
    } else if (legacy_exists) {
        server.aof_use_legacy_file = 1;
        /* Keep an empty in-memory manifest so the first rewrite can migrate the
         * legacy file to the multi-part layout. */
        server.aof_manifest = aofManifestCreate();
    } else {
        aofManifest *am;
        sds incr_name;

        if (aofEnsureDir() != C_OK) {
            serverLog(LL_WARNING,"Can't create the AOF directory %s: %s",
                server.aof_dirname,
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            return C_ERR;
        }
        am = aofManifestCreate();
        incr_name = aofGetIncrFileName(1);
        if (aofCreateEmptyFile(incr_name) != C_OK ||
            aofManifestAddFile(am,incr_name,1,AOF_FILE_TYPE_INCR) != C_OK)
        {
            serverLog(LL_WARNING,"Can't create the initial AOF file: %s",
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            sdsfree(incr_name);
            aofFreeManifest(am);
            return C_ERR;
        }
        sdsfree(incr_name);
        server.aof_manifest = am;
        if (aofRewriteManifest() != C_OK) {
            aofFreeManifest(server.aof_manifest);
            server.aof_manifest = NULL;
            return C_ERR;
        }
    }
    aofCleanupUnusedFiles();
    return C_OK;
}

/* Open the file new writes are appended to: the last incremental file of the
 * manifest, or the legacy single file while an upgrade is pending. */
int aofOpenCurrentAofFile(void) {
    sds path;
    int fd;

    if (server.aof_use_legacy_file || server.aof_manifest == NULL) {
        path = sdsnew(server.aof_filename);
    } else {
        aofInfo *ai = listNodeValue(listLast(server.aof_manifest->incr_aof_list));
        path = aofGetAofPath(ai->file_name);
    }
    fd = open(path,O_WRONLY|O_APPEND|O_CREAT WIN32_ONLY(|_O_BINARY),
              IF_WIN32(_S_IREAD|_S_IWRITE,0644));
    if (fd == -1) {
        serverLog(LL_WARNING,"Can't open the append-only file %s: %s",
            path, IF_WIN32(wsa_strerror(errno), strerror(errno)));
        sdsfree(path);
        return C_ERR;
    }
    sdsfree(path);
    server.aof_fd = fd;
    aofUpdateCurrentSize();
    server.aof_fsync_offset = server.aof_current_size;
    return C_OK;
}

/* Load the base file and every incremental file listed by the manifest. */
int aofLoadAppendOnlyFiles(void) {
    aofManifest *am = server.aof_manifest;
    listIter li;
    listNode *ln;
    struct redis_stat sb;

    if (server.aof_use_legacy_file || am == NULL) {
        if (loadAppendOnlyFile(server.aof_filename) != C_OK) return C_ERR;
        server.aof_base_size = 0;
        if (redis_stat(server.aof_filename,&sb) == 0)
            server.aof_rewrite_base_size = sb.st_size;
        else
            server.aof_rewrite_base_size = 0;
        /* Migrate the legacy file to the multi-part layout. */
        server.aof_rewrite_scheduled = 1;
        return C_OK;
    }

    if (am->base_aof_info) {
        sds path = aofGetAofPath(am->base_aof_info->file_name);
        if (loadAppendOnlyFile(path) != C_OK) { sdsfree(path); return C_ERR; }
        if (redis_stat(path,&sb) == 0)
            server.aof_base_size = sb.st_size;
        else
            server.aof_base_size = 0;
        sdsfree(path);
    } else {
        server.aof_base_size = 0;
    }
    listRewind(am->incr_aof_list,&li);
    while ((ln = listNext(&li)) != NULL) {
        aofInfo *ai = listNodeValue(ln);
        sds path = aofGetAofPath(ai->file_name);
        if (loadAppendOnlyFile(path) != C_OK) { sdsfree(path); return C_ERR; }
        sdsfree(path);
    }
    server.aof_rewrite_base_size = server.aof_base_size;
    return C_OK;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite buffer implementation.
 *
 * The following code implement a simple buffer used in order to accumulate
 * changes while the background process is rewriting the AOF file.
 *
 * We only need to append, but can't just use realloc with a large block
 * because 'huge' reallocs are not always handled as one could expect
 * (via remapping of pages at OS level) but may involve copying data.
 *
 * For this reason we use a list of blocks, every block is
 * AOF_RW_BUF_BLOCK_SIZE bytes.
 * ------------------------------------------------------------------------- */

#define AOF_RW_BUF_BLOCK_SIZE (1024*1024*10)    /* 10 MB per block */

typedef struct aofrwblock {
    PORT_ULONG used, free;
    char buf[AOF_RW_BUF_BLOCK_SIZE];
} aofrwblock;

/* This function free the old AOF rewrite buffer if needed, and initialize
 * a fresh new one. It tests for server.aof_rewrite_buf_blocks equal to NULL
 * so can be used for the first initialization as well. */
void aofRewriteBufferReset(void) {
    if (server.aof_rewrite_buf_blocks)
        listRelease(server.aof_rewrite_buf_blocks);

    server.aof_rewrite_buf_blocks = listCreate();
    listSetFreeMethod(server.aof_rewrite_buf_blocks,zfree);
}

/* Return the current size of the AOF rewrite buffer. */
PORT_ULONG aofRewriteBufferSize(void) {
    listNode *ln;
    listIter li;
    PORT_ULONG size = 0;

    listRewind(server.aof_rewrite_buf_blocks,&li);
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);
        size += block->used;
    }
    return size;
}

/* Event handler used to send data to the child process doing the AOF
 * rewrite. We send pieces of our AOF differences buffer so that the final
 * write when the child finishes the rewrite will be small. */
void aofChildWriteDiffData(aeEventLoop *el, int fd, void *privdata, int mask) {
    listNode *ln;
    aofrwblock *block;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(privdata);
    UNUSED(mask);

    while(1) {
        ln = listFirst(server.aof_rewrite_buf_blocks);
        block = ln ? ln->value : NULL;
        if (server.aof_stop_sending_diff || !block) {
#ifndef _WIN32
            aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,
                              AE_WRITABLE);
#endif
            return;
        }
        if (block->used > 0) {
            nwritten = write(server.aof_pipe_write_data_to_child,
                             block->buf,block->used);
            if (nwritten <= 0) return;
            memmove(block->buf,block->buf+nwritten,block->used-nwritten);
            block->used -= nwritten;
            block->free += nwritten;
        }
        if (block->used == 0) listDelNode(server.aof_rewrite_buf_blocks,ln);
    }
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed. */
void aofRewriteBufferAppend(unsigned char *s, PORT_ULONG len) {
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    while(len) {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. */
        if (block) {
            PORT_ULONG thislen = (block->free < len) ? block->free : len;
            if (thislen) {  /* The current block is not already full. */
                memcpy(block->buf+block->used, s, thislen);
                block->used += thislen;
                block->free -= thislen;
                s += thislen;
                len -= thislen;
            }
        }

        if (len) { /* First block to allocate, or need another block. */
            int numblocks;

            block = zmalloc(sizeof(*block));
#ifdef _WIN32
            if (block) {
#endif
                block->free = AOF_RW_BUF_BLOCK_SIZE;
                block->used = 0;
#ifdef _WIN32
            }
#endif
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. */
            numblocks = (int) listLength(server.aof_rewrite_buf_blocks);         WIN_PORT_FIX /* cast (int) */
            if (((numblocks+1) % 10) == 0) {
                int level = ((numblocks+1) % 100) == 0 ? LL_WARNING :
                                                         LL_NOTICE;
                serverLog(level,"Background AOF buffer size: %Iu MB", WIN_PORT_FIX /* %lu -> %Iu */
                    aofRewriteBufferSize()/(1024*1024));
            }
        }
    }
#ifndef _WIN32
    /* Install a file event to send data to the rewrite child if there is
     * not one already. */
    if (aeGetFileEvents(server.el,server.aof_pipe_write_data_to_child) == 0) {
        aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child,
            AE_WRITABLE, aofChildWriteDiffData, NULL);
    }
#endif
}

/* Write the buffer (possibly composed of multiple blocks) into the specified
 * fd. If a short write or any other error happens -1 is returned,
 * otherwise the number of bytes written is returned. */
ssize_t aofRewriteBufferWrite(int fd) {
    listNode *ln;
    listIter li;
    ssize_t count = 0;

    listRewind(server.aof_rewrite_buf_blocks,&li);
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);
        ssize_t nwritten;

        if (block->used) {
            nwritten = write(fd,block->buf,block->used);
            if (nwritten != (ssize_t)block->used) {
                if (nwritten == 0) errno = EIO;
                return -1;
            }
            count += nwritten;
        }
    }
    return count;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */

/* Return true if an AOf fsync is currently already in progress in a
 * BIO thread. */
int aofFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
}

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
void aof_background_fsync(int fd) {
    bioCreateBackgroundJob(BIO_AOF_FSYNC,(void*)(PORT_LONG)fd,NULL,NULL);
}

/* Kills an AOFRW child process if exists */
static void killAppendOnlyChild(void) {

    POSIX_ONLY(int statloc;)

    /* No AOFRW child? return. */
    if (server.aof_child_pid == -1) return;
    /* Kill AOFRW child, wait for child exit. */
    serverLog(LL_NOTICE,"Killing running AOF rewrite child: %Id", WIN_PORT_FIX /* %ld -> %Id */
        (PORT_LONG) server.aof_child_pid);
#ifdef _WIN32
        AbortForkOperation();
#else
    if (kill(server.aof_child_pid,SIGUSR1) != -1) {
        while(wait3(&statloc,0,NULL) != server.aof_child_pid);
    }
#endif
    /* Reset the buffer accumulating changes while the child saves. */
    aofRewriteBufferReset();
    aofRemoveTempFile(server.aof_child_pid);
    server.aof_child_pid = -1;
    server.aof_rewrite_time_start = -1;
    /* Close pipes used for IPC between the two processes. */
    aofClosePipes();
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
void stopAppendOnly(void) {
    serverAssert(server.aof_state != AOF_OFF);
    flushAppendOnlyFile(1);
    redis_fsync(server.aof_fd);
    close(server.aof_fd);

    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    killAppendOnlyChild();
    /* Keep the manifest in memory: a BGREWRITEAOF issued while the AOF is off
     * still has to produce the next base/incremental files, and the files it
     * references must not be garbage collected until the rewrite completes. */
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
int startAppendOnly(void) {
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    int newfd;

    serverAssert(server.aof_state == AOF_OFF);
    if (server.aof_manifest == NULL &&
        !server.aof_use_legacy_file &&
        aofLoadManifestFromDisk() != C_OK)
    {
        serverLog(LL_WARNING,
            "Redis needs to enable the AOF but can't load or create "
            "the AOF manifest");
        return C_ERR;
    }
    if (aofOpenCurrentAofFile() != C_OK) {
        char *cwdp = IF_WIN32(_getcwd, getcwd)(cwd, MAXPATHLEN);

        serverLog(LL_WARNING,
            "Redis needs to enable the AOF but can't open the "
            "append only file %s/%s (in server root dir %s): %s",
            server.aof_dirname, server.aof_filename,
            cwdp ? cwdp : "unknown",
            IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return C_ERR;
    }
    newfd = server.aof_fd;
    if (server.rdb_child_pid != -1) {
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING,"AOF was enabled but there is already a child process saving an RDB file on disk. An AOF background was scheduled to start when possible.");
    } else {
        /* If there is a pending AOF rewrite, we need to switch it off and
         * start a new one: the old one cannot be reused because it is not
         * accumulating the AOF buffer. */
        if (server.aof_child_pid != -1) {
            serverLog(LL_WARNING,"AOF was enabled but there is already an AOF rewriting in background. Stopping background AOF and starting a rewrite now.");
            killAppendOnlyChild();
        }
        if (rewriteAppendOnlyFileBackground() == C_ERR) {
            close(newfd);
            serverLog(LL_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
            return C_ERR;
        }
    }
    /* We correctly switched on AOF, now wait for the rewrite to be complete
     * in order to append data on disk. */
    server.aof_state = AOF_WAIT_REWRITE;
    server.aof_last_fsync = server.unixtime;
    server.aof_fd = newfd;
    return C_OK;
}

/* This is a wrapper to the write syscall in order to retry on short writes
 * or if the syscall gets interrupted. It could look strange that we retry
 * on short writes given that we are writing to a block device: normally if
 * the first call is short, there is a end-of-space condition, so the next
 * is likely to fail. However apparently in modern systems this is no longer
 * true, and in general it looks just more resilient to retry the write. If
 * there is an actual error condition we'll get it at the next try. */
ssize_t aofWrite(int fd, const char *buf, size_t len) {
    ssize_t nwritten = 0, totwritten = 0;

    while(len) {
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return totwritten ? totwritten : -1;
        }

        len -= nwritten;
        buf += nwritten;
        totwritten += nwritten;
    }

    return totwritten;
}

/* Write the append only file buffer on disk.
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * About the 'force' argument:
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync. */
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
#define AOF_ANNOTATION_LINE_MAX_LEN 28 /* "#TS:<unix time>" plus CRLF */
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;

    if (sdslen(server.aof_buf) == 0) {
        /* Check if we need to do fsync even the aof buffer is empty,
         * because previously in AOF_FSYNC_EVERYSEC mode, fsync is
         * called only when aof buffer is not empty, so if users
         * stop write commands before fsync called in one second,
         * the data in page cache cannot be flushed in time. */
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC &&
            server.aof_fsync_offset != server.aof_current_size &&
            server.unixtime > server.aof_last_fsync &&
            !(sync_in_progress = aofFsyncInProgress())) {
            goto try_fsync;
        } else {
            return;
        }
    }

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = aofFsyncInProgress();

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        if (sync_in_progress) {
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. */
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                return;
            }
            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds. */
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */

    latencyStartMonitor(latency);
    nwritten = aofWrite(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. */
    if (sync_in_progress) {
        latencyAddSampleIfNeeded("aof-write-pending-fsync",latency);
    } else if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) {
        latencyAddSampleIfNeeded("aof-write-active-child",latency);
    } else {
        latencyAddSampleIfNeeded("aof-write-alone",latency);
    }
    latencyAddSampleIfNeeded("aof-write",latency);

    /* We performed the write so reset the postponed flush sentinel to zero. */
    server.aof_flush_postponed_start = 0;

    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. */
        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. */
        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING,"Error writing to the AOF file: %s",
                    IF_WIN32(wsa_strerror(errno), strerror(errno)));
                server.aof_last_write_errno = errno;
            }
        } else {
            if (can_log) {
                serverLog(LL_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (PORT_LONGLONG)nwritten,
                                       (PORT_LONGLONG)sdslen(server.aof_buf));
            }

            if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
                }
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. */
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the
             * reply for the client is already in the output buffers, and we
             * have the contract with the user that on acknowledged write data
             * is synced on disk. */
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                sdsrange(server.aof_buf,(int)nwritten,-1);                      WIN_PORT_FIX /* cast (int) */
            }
            return; /* We'll try again on the next call... */
        }
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }
    server.aof_current_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). */
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000) {
        sdsclear(server.aof_buf);
    } else {
        sdsfree(server.aof_buf);
        server.aof_buf = sdsempty();
    }

try_fsync:
    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    if (server.aof_no_fsync_on_rewrite &&
        (server.aof_child_pid != -1 || server.rdb_child_pid != -1))
            return;

    /* Perform the fsync if needed. */
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* redis_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        latencyStartMonitor(latency);
        redis_fsync(server.aof_fd); /* Let's try to get this data on the disk */
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-fsync-always",latency);
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        if (!sync_in_progress) {
            aof_background_fsync(server.aof_fd);
            server.aof_fsync_offset = server.aof_current_size;
        }
        server.aof_last_fsync = server.unixtime;
    }
}

sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}

/* Create the sds representation of an PEXPIREAT command, using
 * 'seconds' as time to live and 'cmd' to understand what command
 * we are translating into a PEXPIREAT.
 *
 * This command is used in order to translate EXPIRE and PEXPIRE commands
 * into PEXPIREAT command so that we retain precision in the append only
 * file, and the time is always absolute and not relative. */
sds catAppendOnlyExpireAtCommand(sds buf, struct redisCommand *cmd, robj *key, robj *seconds) {
    PORT_LONGLONG when;
    robj *argv[3];

    /* Make sure we can use strtoll */
    seconds = getDecodedObject(seconds);
    when = strtoll(seconds->ptr,NULL,10);
    /* Convert argument into milliseconds for EXPIRE, SETEX, EXPIREAT */
    if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
        cmd->proc == expireatCommand)
    {
        when *= 1000;
    }
    /* Convert into absolute time for EXPIRE, PEXPIRE, SETEX, PSETEX */
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        when += mstime();
    }
    decrRefCount(seconds);

    argv[0] = createStringObject("PEXPIREAT",9);
    argv[1] = key;
    argv[2] = createStringObjectFromLongLong(when);
    buf = catAppendOnlyGenericCommand(buf, 3, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    return buf;
}

/* Generate a "#TS:<unix time>" annotation for the AOF. An annotation is
 * produced at most once per second unless 'force' is set, which the rewrite
 * child uses to record the time the rewrite started. */
static sds genAofTimestampAnnotationIfNeeded(int force) {
    sds ts = NULL;

    if (force || server.aof_cur_timestamp < server.unixtime) {
        server.aof_cur_timestamp = force ? time(NULL) : server.unixtime;
        ts = sdscatfmt(sdsempty(),"#TS:%I\r\n",server.aof_cur_timestamp);
        serverAssert(sdslen(ts) <= AOF_ANNOTATION_LINE_MAX_LEN);
    }
    return ts;
}

void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    robj *tmpargv[3];

    /* Annotate the write with the current time when configured. */
    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(0);
        if (ts != NULL) {
            buf = sdscatsds(buf,ts);
            sdsfree(ts);
        }
    }

    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. */
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        WIN32_ONLY(seldb[sizeof(seldb)-1] = 0;) /*get rid of C6053 warning*/
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%Iu\r\n%s\r\n", WIN_PORT_FIX /* %lu -> %Iu */
            (PORT_ULONG)strlen(seldb),seldb);
        server.aof_selected_db = dictid;
    }

    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == expireatCommand) {
        /* Translate EXPIRE/PEXPIRE/EXPIREAT into PEXPIREAT */
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);
    } else if (cmd->proc == setexCommand || cmd->proc == psetexCommand) {
        /* Translate SETEX/PSETEX to SET and PEXPIREAT */
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);
    } else if (cmd->proc == setCommand && argc > 3) {
        int i;
        robj *timearg = NULL;
        struct redisCommand *timecmd = NULL;
        /* Record one absolute expiry. PXAT is already absolute; EX/PX are not.
         * Decode before comparing: an integer-encoded argument stores the
         * number in ptr, and strcasecmp on it is an access violation. */
        buf = catAppendOnlyGenericCommand(buf,3,argv);
        for (i = 3; i < argc; i ++) {
            robj *opt;
            if (i+1 >= argc) break;
            opt = getDecodedObject(argv[i]);
            if (!strcasecmp(opt->ptr, "ex")) {
                timearg = argv[i+1]; timecmd = server.expireCommand;
            } else if (!strcasecmp(opt->ptr, "px")) {
                timearg = argv[i+1]; timecmd = server.pexpireCommand;
            } else if (!strcasecmp(opt->ptr, "pxat")) {
                timearg = argv[i+1]; timecmd = server.pexpireatCommand;
            }
            decrRefCount(opt);
        }
        if (timearg)
            buf = catAppendOnlyExpireAtCommand(buf,timecmd,argv[1],timearg);
    } else {
        /* All the other commands don't need translation or need the
         * same translation already operated in the command vector
         * for the replication itself. */
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    if (server.aof_state == AOF_ON)
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    if (server.aof_child_pid != -1)
        aofRewriteBufferAppend((unsigned char*)buf,(PORT_ULONG)sdslen(buf));    WIN_PORT_FIX /* cast (PORT_ULONG) */

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
struct client *createFakeClient(void) {
    struct client *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->fd = -1;
    c->name = NULL;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    c->btype = BLOCKED_NONE;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

void freeFakeClientArgv(struct client *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);
}

void freeFakeClient(struct client *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    zfree(c);
}

/* Replay the append log file. On success C_OK is returned. On non fatal
 * error (the append only file is zero-length) C_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
int loadAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    FILE *fp = fopen(filename, IF_WIN32("rb", "r"));
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    PORT_LONG loops = 0;
    off_t valid_up_to = 0; /* Offset of latest well-formed command loaded. */
    off_t valid_before_multi = 0; /* Offset before MULTI command loaded. */

    if (fp == NULL) {
        serverLog(LL_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Handle a zero-length AOF file as a special case. An empty AOF file
     * is a valid AOF because an empty server with AOF enabled will create
     * a zero length file at startup, that will remain like that if no write
     * operation is received. */
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.aof_current_size = 0;
        server.aof_fsync_offset = 0;
        fclose(fp);
        return C_OK;
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.aof_state = AOF_OFF;

    fakeClient = createFakeClient();
    startLoading(fp);

    /* Check if this AOF file has an RDB preamble. In that case we need to
     * load the RDB file and later continue loading the AOF tail. */
    char sig[5]; /* "REDIS" */
    if (fread(sig,1,5,fp) != 5 || memcmp(sig,"REDIS",5) != 0) {
        /* No RDB preamble, seek back at 0 offset. */
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
    } else {
        /* RDB preamble. Pass loading the RDB functions. */
        rio rdb;

        serverLog(LL_NOTICE,"Reading RDB preamble from AOF file...");
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
        rioInitWithFile(&rdb,fp);
        if (rdbLoadRio(&rdb,NULL,1) != C_OK) {
            serverLog(LL_WARNING,"Error reading the RDB preamble of the AOF file, AOF loading aborted");
            goto readerr;
        } else {
            serverLog(LL_NOTICE,"Reading the remaining AOF tail...");
        }
    }

    /* Read the actual AOF file, in REPL format, command by command. */
    while(1) {
        int argc, j;
        PORT_ULONG len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        if (!(loops++ % 1000)) {
            loadingProgress((off_t) ftello(fp));                                 WIN_PORT_FIX /* cast (off_t) */
            processEventsWhileBlocked();
        }

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        /* "#TS:<unix time>" annotations are comments, not commands. */
        if (buf[0] == '#') continue;
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;

        /* Load the next command in the AOF as our fake client
         * argv. */
        argv = zmalloc(sizeof(robj*)*argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;

        for (j = 0; j < argc; j++) {
            /* Parse the argument len. */
            char *readres = fgets(buf,sizeof(buf),fp);
            if (readres == NULL || buf[0] != '$') {
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                if (readres == NULL)
                    goto readerr;
                else
                    goto fmterr;
            }
            len = strtol(buf+1,NULL,10);

            /* Read it into a string object. */
            argsds = sdsnewlen(SDS_NOINIT,len);
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            argv[j] = createObject(OBJ_STRING,argsds);

            /* Discard CRLF. */
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
        }

        /* Command lookup */
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            serverLog(LL_WARNING,
                "Unknown command '%s' reading the append only file",
                (char*)argv[0]->ptr);
            exit(1);
        }

        if (cmd == server.multiCommand) valid_before_multi = valid_up_to;

        /* Run the command in the context of a fake client */
        fakeClient->cmd = cmd;
        if (fakeClient->flags & CLIENT_MULTI &&
            fakeClient->cmd->proc != execCommand)
        {
            queueMultiCommand(fakeClient);
        } else {
            cmd->proc(fakeClient);
        }

        /* The fake client should not have a reply */
        serverAssert(fakeClient->bufpos == 0 &&
                     listLength(fakeClient->reply) == 0);

        /* The fake client should never get blocked */
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        freeFakeClientArgv(fakeClient);
        fakeClient->cmd = NULL;
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, handle it as it was
     * a short read, even if technically the protocol is correct: we want
     * to remove the unprocessed tail and continue. */
    if (fakeClient->flags & CLIENT_MULTI) {
        serverLog(LL_WARNING,
            "Revert incomplete MULTI/EXEC transaction in AOF file");
        valid_up_to = valid_before_multi;
        goto uxeof;
    }

loaded_ok: /* DB loaded, cleanup and return C_OK to the caller. */
    fclose(fp);
    freeFakeClient(fakeClient);
    server.aof_state = old_aof_state;
    stopLoading();
    /* Sizes are accounted by the caller, which knows the manifest. */
    return C_OK;

readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
    if (!feof(fp)) {
        if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
        serverLog(LL_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
        exit(1);
    }

uxeof: /* Unexpected AOF end of file. */
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING,"!!! Warning: short read while loading the AOF file !!!");
        serverLog(LL_WARNING,"!!! Truncating the AOF at offset %llu !!!",
            (PORT_ULONGLONG) valid_up_to);
        if (valid_up_to == -1 || truncate(filename,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING,"Last valid command offset is invalid");
            } else {
                serverLog(LL_WARNING,"Error truncating the AOF file: %s",
                    IF_WIN32(wsa_strerror(errno), strerror(errno)));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                serverLog(LL_WARNING,"Can't seek the end of the AOF file: %s",
                    IF_WIN32(wsa_strerror(errno), strerror(errno)));
            } else {
                serverLog(LL_WARNING,
                    "AOF loaded anyway because aof-load-truncated is enabled");
                goto loaded_ok;
            }
        }
    }
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
    serverLog(LL_WARNING,"Unexpected end of file reading the append only file. You can: 1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename>. 2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.");
    exit(1);

fmterr: /* Format error. */
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
    serverLog(LL_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. */
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    if (obj->encoding == OBJ_ENCODING_INT) {
        return (int)rioWriteBulkLongLong(r,(PORT_LONG)obj->ptr);               WIN_PORT_FIX /* cast (int) */
    } else if (sdsEncodedObject(obj)) {
        return (int)rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));           WIN_PORT_FIX /* cast (int) */
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
int rewriteListObject(rio *r, robj *key, robj *o) {
    PORT_LONGLONG count = 0, items = listTypeLength(o);

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;

        while (quicklistNext(li,&entry)) {
            if (count == 0) {
                PORT_LONGLONG cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?  WIN_PORT_FIX /* int -> PORT_LONGLONG */
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }

            if (entry.value) {
                if (rioWriteBulkString(r,(char*)entry.value,entry.sz) == 0) return 0;
            } else {
                if (rioWriteBulkLongLong(r,entry.longval) == 0) return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        quicklistReleaseIterator(li);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
int rewriteSetObject(rio *r, robj *key, robj *o) {
    PORT_LONGLONG count = 0, items = setTypeSize(o);

    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : (int)items;               WIN_PORT_FIX /* cast (int) */

                if (rioWriteBulkCount(r,'*',(PORT_LONG)2+cmd_items) == 0) return 0;  WIN_PORT_FIX /* cast (PORT_LONG) */
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkLongLong(r,llval) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : (int)items;               WIN_PORT_FIX /* cast (int) */

                if (rioWriteBulkCount(r,'*',(PORT_LONG)2+cmd_items) == 0) return 0;  WIN_PORT_FIX /* cast (PORT_LONG) */
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkString(r,ele,sdslen(ele)) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    PORT_LONGLONG count = 0, items = zsetLength(o);

    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        PORT_LONGLONG vll;
        double score;

        eptr = ziplistIndex(zl,0);
        serverAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        while (eptr != NULL) {
            serverAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : (int)items;               WIN_PORT_FIX /* cast (int) */

                if (rioWriteBulkCount(r,'*',2+((PORT_LONG)cmd_items)*2) == 0) return 0;  WIN_PORT_FIX /* cast (PORT_LONG) */
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,score) == 0) return 0;
            if (vstr != NULL) {
                if (rioWriteBulkString(r,(char*)vstr,vlen) == 0) return 0;
            } else {
                if (rioWriteBulkLongLong(r,vll) == 0) return 0;
            }
            zzlNext(zl,&eptr,&sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : (int)items;               WIN_PORT_FIX /* cast (int) */

                if (rioWriteBulkCount(r,'*',2+((PORT_LONG)cmd_items)*2) == 0) return 0;  WIN_PORT_FIX /* cast (PORT_LONG) */
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,*score) == 0) return 0;
            if (rioWriteBulkString(r,ele,sdslen(ele)) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 * The 'hi' argument passes a valid Redis hash iterator.
 * The 'what' filed specifies if to write a key or a value and can be
 * either OBJ_HASH_KEY or OBJ_HASH_VALUE.
 *
 * The function returns 0 on error, non-zero on success. */
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        PORT_LONGLONG vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            return (int)rioWriteBulkString(r, (char*)vstr, vlen);       WIN_PORT_FIX /* cast (int) */
        else
            return (int)rioWriteBulkLongLong(r, vll);                   WIN_PORT_FIX /* cast (int) */
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        return (int)rioWriteBulkString(r, value, sdslen(value));        WIN_PORT_FIX /* cast (int) */
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    PORT_LONGLONG count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (int) ((items > AOF_REWRITE_ITEMS_PER_CMD) ? WIN_PORT_FIX /* cast (int) */
                AOF_REWRITE_ITEMS_PER_CMD : items);

            if (rioWriteBulkCount(r,'*',2+((PORT_LONG)cmd_items)*2) == 0) return 0;  WIN_PORT_FIX /* cast (PORT_LONG) */
            if (rioWriteBulkString(r,"HMSET",5) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
        }

        if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) == 0) return 0;
        if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE) == 0) return 0;
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Helper for rewriteStreamObject() that generates a bulk string into the
 * AOF representing the ID 'id'. */
int rioWriteBulkStreamID(rio *r,streamID *id) {
    int retval;

    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    retval = rioWriteBulkString(r,replyid,sdslen(replyid));
    sdsfree(replyid);
    return retval;
}

/* Helper for rewriteStreamObject(): emit the XCLAIM needed in order to
 * add the message described by 'nack' having the id 'rawid', into the pending
 * list of the specified consumer. All this in the context of the specified
 * key and group. */
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer, unsigned char *rawid, streamNACK *nack) {
     /* XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
               RETRYCOUNT <count> JUSTID FORCE. */
    streamID id;
    streamDecodeID(rawid,&id);
    if (rioWriteBulkCount(r,'*',12) == 0) return 0;
    if (rioWriteBulkString(r,"XCLAIM",6) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    if (rioWriteBulkString(r,"0",1) == 0) return 0;
    if (rioWriteBulkStreamID(r,&id) == 0) return 0;
    if (rioWriteBulkString(r,"TIME",4) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_time) == 0) return 0;
    if (rioWriteBulkString(r,"RETRYCOUNT",10) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_count) == 0) return 0;
    if (rioWriteBulkString(r,"JUSTID",6) == 0) return 0;
    if (rioWriteBulkString(r,"FORCE",5) == 0) return 0;
    return 1;
}

/* Emit the commands needed to rebuild a stream object.
 * The function returns 0 on error, 1 on success. */
int rewriteStreamObject(rio *r, robj *key, robj *o) {
    stream *s = o->ptr;
    streamIterator si;
    streamIteratorStart(&si,s,NULL,NULL,0);
    streamID id;
    int64_t numfields;

    if (s->length) {
        /* Reconstruct the stream data using XADD commands. */
        while(streamIteratorGetID(&si,&id,&numfields)) {
            /* Emit a two elements array for each item. The first is
             * the ID, the second is an array of field-value pairs. */

            /* Emit the XADD <key> <id> ...fields... command. */
            if (rioWriteBulkCount(r,'*',3+numfields*2) == 0) return 0;
            if (rioWriteBulkString(r,"XADD",4) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
            if (rioWriteBulkStreamID(r,&id) == 0) return 0;
            while(numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si,&field,&value,&field_len,&value_len);
                if (rioWriteBulkString(r,(char*)field,field_len) == 0) return 0;
                if (rioWriteBulkString(r,(char*)value,value_len) == 0) return 0;
            }
        }
    } else {
        /* Use the XADD MAXLEN 0 trick to generate an empty stream if
         * the key we are serializing is an empty string, which is possible
         * for the Stream type. */
        if (rioWriteBulkCount(r,'*',7) == 0) return 0;
        if (rioWriteBulkString(r,"XADD",4) == 0) return 0;
        if (rioWriteBulkObject(r,key) == 0) return 0;
        if (rioWriteBulkString(r,"MAXLEN",6) == 0) return 0;
        if (rioWriteBulkString(r,"0",1) == 0) return 0;
        if (rioWriteBulkStreamID(r,&s->last_id) == 0) return 0;
        if (rioWriteBulkString(r,"x",1) == 0) return 0;
        if (rioWriteBulkString(r,"y",1) == 0) return 0;
    }

    /* Append XSETID after XADD, make sure lastid is correct,
     * in case of XDEL lastid. Also preserve the entry counters that the
     * legacy stream RDB format cannot store. */
    if (rioWriteBulkCount(r,'*',7) == 0) return 0;
    if (rioWriteBulkString(r,"XSETID",6) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkStreamID(r,&s->last_id) == 0) return 0;
    if (rioWriteBulkString(r,"ENTRIESADDED",12) == 0) return 0;
    if (rioWriteBulkLongLong(r,s->entries_added) == 0) return 0;
    if (rioWriteBulkString(r,"MAXDELETEDID",12) == 0) return 0;
    if (rioWriteBulkStreamID(r,&s->max_deleted_entry_id) == 0) return 0;


    /* Create all the stream consumer groups. */
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri,s->cgroups);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            streamCG *group = ri.data;
            /* Emit the XGROUP CREATE in order to create the group. */
            if (rioWriteBulkCount(r,'*',5) == 0) return 0;
            if (rioWriteBulkString(r,"XGROUP",6) == 0) return 0;
            if (rioWriteBulkString(r,"CREATE",6) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
            if (rioWriteBulkString(r,(char*)ri.key,ri.key_len) == 0) return 0;
            if (rioWriteBulkStreamID(r,&group->last_id) == 0) return 0;

            /* Generate XCLAIMs for each consumer that happens to
             * have pending entries. Empty consumers have no semantical
             * value so they are discarded. */
            raxIterator ri_cons;
            raxStart(&ri_cons,group->consumers);
            raxSeek(&ri_cons,"^",NULL,0);
            while(raxNext(&ri_cons)) {
                streamConsumer *consumer = ri_cons.data;
                /* For the current consumer, iterate all the PEL entries
                 * to emit the XCLAIM protocol. */
                raxIterator ri_pel;
                raxStart(&ri_pel,consumer->pel);
                raxSeek(&ri_pel,"^",NULL,0);
                while(raxNext(&ri_pel)) {
                    streamNACK *nack = ri_pel.data;
                    if (rioWriteStreamPendingEntry(r,key,(char*)ri.key,
                                                   ri.key_len,consumer,
                                                   ri_pel.key,nack) == 0)
                    {
                        return 0;
                    }
                }
                raxStop(&ri_pel);
            }
            raxStop(&ri_cons);
        }
        raxStop(&ri);
    }

    streamIteratorStop(&si);
    return 1;
}

/* Call the module type callback in order to rewrite a data type
 * that is exported by a module and is not handled by Redis itself.
 * The function returns 0 on error, 1 on success. */
int rewriteModuleObject(rio *r, robj *key, robj *o) {
    RedisModuleIO io;
    moduleValue *mv = o->ptr;
    moduleType *mt = mv->type;
    moduleInitIOContext(io,mt,r,key);
    mt->aof_rewrite(&io,key,mv->value);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return io.error ? 0 : 1;
}

/* This function is called by the child rewriting the AOF file to read
 * the difference accumulated from the parent into a buffer, that is
 * concatenated at the end of the rewrite. */
ssize_t aofReadDiffFromParent(void) {
#ifndef _WIN32
    char buf[65536]; /* Default pipe buffer size on most Linux systems. */
#else
    char* buf = zmalloc(65536); //allocate dynamically and get rid of C6262 warning
#endif
    ssize_t nread, total = 0;

    while ((nread =
            read(server.aof_pipe_read_data_from_parent,buf,IF_WIN32(65536,sizeof(buf)))) > 0) {
        server.aof_child_diff = sdscatlen(server.aof_child_diff,buf,nread);
        total += nread;
    }
#ifdef _WIN32
    zfree(buf);
#endif
    return total;
}

int rewriteAppendOnlyFileRio(rio *aof) {
    dictIterator *di = NULL;
    dictEntry *de;
    size_t processed = 0;
    int j;

    /* Record the time the rewritten file started. */
    if (server.aof_timestamp_enabled) {
        sds ts = genAofTimestampAnnotationIfNeeded(1);
        if (rioWrite(aof,ts,sdslen(ts)) == 0) {
            sdsfree(ts);
            goto werr;
        }
        sdsfree(ts);
    }

    /* Function libraries are replayed before any key-space command so that
     * functions called by the rewritten dataset already exist. */
    if (functionsRewriteAOF(aof) == C_ERR) goto werr;

    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);

        /* SELECT the new DB */
        if (rioWrite(aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            PORT_LONGLONG expiretime;

            keystr = dictGetKey(de);
            o = dictGetVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* Save the key and associated value */
            if (o->type == OBJ_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(aof,o) == 0) goto werr;
            } else if (o->type == OBJ_LIST) {
                if (rewriteListObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_SET) {
                if (rewriteSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_ZSET) {
                if (rewriteSortedSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_HASH) {
                if (rewriteHashObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_STREAM) {
                if (rewriteStreamObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_MODULE) {
                if (rewriteModuleObject(aof,&key,o) == 0) goto werr;
            } else {
                serverPanic("Unknown object type");
            }
            /* Save the expire time */
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(aof,expiretime) == 0) goto werr;
            }
            /* Read some diff from the parent process from time to time. */
            if (aof->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES) {
                processed = aof->processed_bytes;
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);
        di = NULL;
    }
    return C_OK;

werr:
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp;
    char tmpfile[256];
    char byte;

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile, IF_WIN32("wb", "w"));
    if (!fp) {
        serverLog(LL_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
        return C_ERR;
    }

    server.aof_child_diff = sdsempty();
    rioInitWithFile(&aof,fp);

    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof,REDIS_AUTOSYNC_BYTES);

    if (server.aof_use_rdb_preamble) {
        int error;
        if (rdbSaveRio(&aof,&error,RDB_SAVE_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }
    } else {
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;
    }

    /* Do an initial slow fsync here while the parent is still sending
     * data, in order to make the next final fsync faster. */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;

    /* Read again a few times to get more data from the parent.
     * We can't read forever (the server may receive data from clients
     * faster than it is able to send data to the child), so we try to read
     * some more data in a loop as soon as there is a good chance more data
     * will come. If it looks like we are wasting time, we abort (this
     * happens after 20 ms without new data). */
    int nodata = 0;
    mstime_t start = mstime();
    while(mstime()-start < 1000 && nodata < 20) {
        if (aeWait(server.aof_pipe_read_data_from_parent, AE_READABLE, 1) <= 0)
        {
            nodata++;
            continue;
        }
        nodata = 0; /* Start counting from zero, we stop on N *contiguous*
                       timeouts. */
        aofReadDiffFromParent();
    }

    /* Ask the master to stop sending diffs. */
    if (write(server.aof_pipe_write_ack_to_parent,"!",1) != 1) goto werr;
#ifndef _WIN32
    if (anetNonBlock(NULL,server.aof_pipe_read_ack_from_parent) != ANET_OK)
        goto werr;
#endif
    /* We read the ACK from the server using a 10 seconds timeout. Normally
     * it should reply ASAP, but just in case we lose its reply, we are sure
     * the child will eventually get terminated. */
    if (syncRead(server.aof_pipe_read_ack_from_parent,&byte,1,5000) != 1 ||
        byte != '!') goto werr;
    serverLog(LL_NOTICE,"Parent agreed to stop sending diffs. Finalizing AOF...");

    /* Read the final diff if any. */
    aofReadDiffFromParent();

    /* Write the received diff to the file. */
    serverLog(LL_NOTICE,
        "Concatenating %.2f MB of AOF diff received from parent.",
        (double) (sdslen(server.aof_child_diff) / (1024*1024)));  WIN_PORT_FIX
    if (rioWrite(&aof,server.aof_child_diff,sdslen(server.aof_child_diff)) == 0)
        goto werr;

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
        unlink(tmpfile);
        return C_ERR;
    }
    serverLog(LL_NOTICE,"SYNC append only file rewrite performed");
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", IF_WIN32(wsa_strerror(errno), strerror(errno)));
    fclose(fp);
    unlink(tmpfile);
    return C_ERR;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite pipes for IPC
 * -------------------------------------------------------------------------- */

/* This event handler is called when the AOF rewriting child sends us a
 * single '!' char to signal we should stop sending buffer diffs. The
 * parent sends a '!' as well to acknowledge. */
void aofChildPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    char byte;
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    if (read(fd,&byte,1) == 1 && byte == '!') {
        serverLog(LL_NOTICE,"AOF rewrite child asks to stop sending diffs.");
        server.aof_stop_sending_diff = 1;
        if (write(server.aof_pipe_write_ack_to_child,"!",1) != 1) {
            /* If we can't send the ack, inform the user, but don't try again
             * since in the other side the children will use a timeout if the
             * kernel can't buffer our write, or, the children was
             * terminated. */
            serverLog(LL_WARNING,"Can't send ACK to AOF child: %s",
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
        }
    }
#ifndef _WIN32
    /* Remove the handler since this can be called only one time during a
     * rewrite. */
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
#endif
}

/* Create the pipes used for parent - child process IPC during rewrite.
 * We have a data pipe used to send AOF incremental diffs to the child,
 * and two other pipes used by the children to signal it finished with
 * the rewrite so no more data should be written, and another for the
 * parent to acknowledge it understood this new condition. */
int aofCreatePipes(void) {
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    int j;

    if (pipe(fds) == -1) goto error; /* parent -> children data. */
    if (pipe(fds+2) == -1) goto error; /* children -> parent ack. */
    if (pipe(fds+4) == -1) goto error; /* parent -> children ack. */
    /* Parent -> children data is non blocking. */
#ifndef WIN32
    if (anetNonBlock(NULL,fds[0]) != ANET_OK) goto error;
    if (anetNonBlock(NULL,fds[1]) != ANET_OK) goto error;
    if (aeCreateFileEvent(server.el, fds[2], AE_READABLE, aofChildPipeReadable, NULL) == AE_ERR) goto error;
#else
    if (FDAPI_PipeSetNonBlock(fds[0], 1) != 0) goto error;
    if (FDAPI_PipeSetNonBlock(fds[1], 1) != 0) goto error;
    if (FDAPI_PipeSetNonBlock(fds[2], 1) != 0) goto error;
#endif

    server.aof_pipe_write_data_to_child = fds[1];
    server.aof_pipe_read_data_from_parent = fds[0];
    server.aof_pipe_write_ack_to_parent = fds[3];
    server.aof_pipe_read_ack_from_child = fds[2];
    server.aof_pipe_write_ack_to_child = fds[5];
    server.aof_pipe_read_ack_from_parent = fds[4];
    server.aof_stop_sending_diff = 0;
    return C_OK;

error:
    serverLog(LL_WARNING,"Error opening /setting AOF rewrite IPC pipes: %s",
        IF_WIN32(wsa_strerror(errno), strerror(errno)));
    for (j = 0; j < 6; j++) if(fds[j] != -1) close(fds[j]);
    return C_ERR;
}

void aofClosePipes(void) {
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
    aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,AE_WRITABLE);
    close(server.aof_pipe_write_data_to_child);
    close(server.aof_pipe_read_data_from_parent);
    close(server.aof_pipe_write_ack_to_parent);
    close(server.aof_pipe_read_ack_from_child);
    close(server.aof_pipe_write_ack_to_child);
    close(server.aof_pipe_read_ack_from_parent);
}

/* ----------------------------------------------------------------------------
 * AOF background rewrite
 * ------------------------------------------------------------------------- */

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    PORT_LONGLONG start;

    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;
    if (aofCreatePipes() != C_OK) return C_ERR;
    openChildInfoPipe();
    start = ustime();

#ifndef _WIN32
    if ((childpid = fork()) == 0) {
#endif
        char tmpfile[256];

#ifndef _WIN32
        /* Child */
        closeClildUnusedResourceAfterFork();
        redisSetProcTitle("redis-aof-rewrite");
#endif
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
#ifdef _WIN32
        childpid = BeginForkOperation_Aof(server.aof_pipe_write_ack_to_parent,
            server.aof_pipe_read_ack_from_parent,
            server.aof_pipe_read_data_from_parent,
            tmpfile,
            &server,
            sizeof(server),
            dictGetHashFunctionSeed(),
            modules);
#else
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "AOF rewrite: %Iu MB of memory used by copy-on-write", WIN_PORT_FIX /* %zu -> %Iu */
                    private_dirty/(1024*1024));
            }

            server.child_info_data.cow_size = private_dirty;
            sendChildInfo(CHILD_INFO_TYPE_AOF);
            exitFromChild(0);
        } else {
            exitFromChild(1);
        }
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
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            aofClosePipes();
            return C_ERR;
        }
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %d",childpid);
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        server.aof_child_pid = childpid;
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. */
        server.aof_selected_db = -1;
        replicationScriptCacheFlush();
        return C_OK;
#ifndef _WIN32
    }
#endif
    return C_OK; /* unreached */
}

void bgrewriteaofCommand(client *c) {
    if (server.aof_child_pid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (server.rdb_child_pid != -1) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReply(c,shared.err);
    }
}

void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    WIN32_ONLY(tmpfile[sizeof(tmpfile) - 1] = 0;) /*get rid of C6053 warning*/
    unlink(tmpfile);
}

/* Update the server.aof_current_size field explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current length, that is much faster. */
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    mstime_t latency;

#ifdef _WIN32
    if (server.aof_fd == -1) {
        serverLog(LL_NOTICE, "Unable to check the AOF length: %s", "appendfd is -1");
        return;
    }
#endif

    latencyStartMonitor(latency);
    if (redis_fstat(server.aof_fd,&sb) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the AOF file length. stat: %s",
            IF_WIN32(wsa_strerror(errno), strerror(errno)));
    } else {
        server.aof_current_size = sb.st_size;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat",latency);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        int newfd = -1;
        int oldfd = server.aof_fd;
        char tmpfile[256];
        sds base_name = NULL, incr_name = NULL;
        sds base_path = NULL, incr_path = NULL;
        aofManifest *am = server.aof_manifest;
        aofInfo *old_base = NULL;
        list *old_incr = NULL;
        sds old_legacy = NULL;
        PORT_LONGLONG new_seq, old_base_seq, old_incr_seq;
        PORT_LONGLONG now = ustime();
        mstime_t latency;
        struct redis_stat sb;
        listIter li;
        listNode *ln;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        if (am == NULL && server.aof_state == AOF_OFF) {
            /* BGREWRITEAOF while the AOF is disabled still has to produce a
             * loadable multi-part AOF, so start from an empty manifest. */
            am = server.aof_manifest = aofManifestCreate();
        }
        if (am == NULL) {
            serverLog(LL_WARNING,
                "AOF manifest is missing, keeping the previous AOF files");
            goto rewrite_cleanup;
        }

        new_seq = (am->curr_base_file_seq > am->curr_incr_file_seq ?
                   am->curr_base_file_seq : am->curr_incr_file_seq) + 1;
        base_name = aofGetBaseFileName(new_seq);
        incr_name = aofGetIncrFileName(new_seq);
        base_path = aofGetAofPath(base_name);
        incr_path = aofGetAofPath(incr_name);

        /* The directory may not exist yet when migrating a legacy single-file
         * AOF to the multi-part layout. */
        if (aofEnsureDir() != C_OK) {
            serverLog(LL_WARNING,"Can't create the AOF directory %s: %s",
                server.aof_dirname,
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            goto rewrite_cleanup;
        }

        /* Flush the differences accumulated by the parent to the rewritten
         * base file. */
        latencyStartMonitor(latency);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            IF_WIN32((int)getpid(),(int)server.aof_child_pid));
        newfd = open(tmpfile,O_WRONLY|O_APPEND|O_CREAT WIN32_ONLY(|_O_BINARY),
                     IF_WIN32(_S_IREAD|_S_IWRITE,0644));
        if (newfd == -1) {
            serverLog(LL_WARNING,
                "Unable to open the temporary AOF produced by the child: %s",
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            goto rewrite_cleanup;
        }
        if (aofRewriteBufferWrite(newfd) == -1) {
            serverLog(LL_WARNING,
                "Error trying to flush the parent diff to the rewritten AOF: %s",
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            close(newfd);
            newfd = -1;
            goto rewrite_cleanup;
        }
        if (redis_fsync(newfd) == -1) {
            serverLog(LL_WARNING,
                "Error trying to fsync the rewritten AOF: %s",
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
        }
        close(newfd);
        newfd = -1;
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rewrite-diff-write",latency);

        serverLog(LL_NOTICE,
            "Residual parent diff successfully flushed to the rewritten AOF (%.2f MB)", (double) (aofRewriteBufferSize() / (1024*1024)));  WIN_PORT_FIX

        /* Move the base file into place and create the new incremental file. */
        latencyStartMonitor(latency);
        if (rename(tmpfile,base_path) == -1) {
            serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF file %s into %s: %s",
                tmpfile, base_path,
                IF_WIN32(wsa_strerror(errno), strerror(errno)));
            goto rewrite_cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rename",latency);

        if (aofCreateEmptyFile(incr_name) != C_OK) {
            serverLog(LL_WARNING,
                "Error trying to create the new incremental AOF file %s: %s",
                incr_name, IF_WIN32(wsa_strerror(errno), strerror(errno)));
            unlink(base_path);
            goto rewrite_cleanup;
        }
        newfd = open(incr_path,O_WRONLY|O_APPEND WIN32_ONLY(|_O_BINARY),
                     IF_WIN32(_S_IREAD|_S_IWRITE,0644));
        if (newfd == -1) {
            serverLog(LL_WARNING,
                "Error trying to open the new incremental AOF file %s: %s",
                incr_path, IF_WIN32(wsa_strerror(errno), strerror(errno)));
            unlink(base_path);
            unlink(incr_path);
            goto rewrite_cleanup;
        }

        /* Build the new manifest in memory and write it. The previous files
         * are kept until the manifest points at the new ones. */
        old_base = am->base_aof_info;
        old_incr = am->incr_aof_list;
        old_base_seq = am->curr_base_file_seq;
        old_incr_seq = am->curr_incr_file_seq;
        if (server.aof_use_legacy_file) old_legacy = sdsnew(server.aof_filename);

        am->base_aof_info = NULL;
        am->incr_aof_list = listCreate();
        listSetFreeMethod(am->incr_aof_list,aofInfoFree);
        aofManifestAddFile(am,base_name,new_seq,AOF_FILE_TYPE_BASE);
        aofManifestAddFile(am,incr_name,new_seq,AOF_FILE_TYPE_INCR);
        am->curr_base_file_seq = new_seq;
        am->curr_incr_file_seq = new_seq;

        if (aofRewriteManifest() != C_OK) {
            /* Roll back to the previous manifest: its files are intact. */
            aofInfoFree(am->base_aof_info);
            listSetFreeMethod(am->incr_aof_list,aofInfoFree);
            listRelease(am->incr_aof_list);
            am->base_aof_info = old_base;
            am->incr_aof_list = old_incr;
            am->curr_base_file_seq = old_base_seq;
            am->curr_incr_file_seq = old_incr_seq;
            old_base = NULL;
            old_incr = NULL;
            if (old_legacy) {
                sdsfree(old_legacy);
                old_legacy = NULL;
            }
            close(newfd);
            newfd = -1;
            unlink(base_path);
            unlink(incr_path);
            goto rewrite_cleanup;
        }

        /* From here on the new manifest is authoritative. */
        if (server.aof_fd == -1) {
            /* AOF disabled: keep the rewritten files, close our descriptor. */
            close(newfd);
            newfd = -1;
        } else {
#ifdef _WIN32
            /* Windows can't unlink a file while it is open. */
            close(server.aof_fd);
            oldfd = -1;
#endif
            server.aof_fd = newfd;
            newfd = -1;
            if (server.aof_fsync == AOF_FSYNC_ALWAYS)
                redis_fsync(server.aof_fd);
            else if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
                aof_background_fsync(server.aof_fd);
            server.aof_selected_db = -1;

            /* The rewrite buffer was just written into the base file. */
            sdsfree(server.aof_buf);
            server.aof_buf = sdsempty();
        }
        server.aof_use_legacy_file = 0;

        if (server.aof_fd != -1) {
            aofUpdateCurrentSize();
            server.aof_fsync_offset = server.aof_current_size;
        }
        if (redis_stat(base_path,&sb) == 0)
            server.aof_base_size = sb.st_size;
        else
            server.aof_base_size = 0;
        server.aof_rewrite_base_size = server.aof_base_size;

        /* Remove the files the previous manifest referenced. */
        if (old_base) {
            sds path = aofGetAofPath(old_base->file_name);
            unlink(path);
            sdsfree(path);
            aofInfoFree(old_base);
        }
        listSetFreeMethod(old_incr,aofInfoFree);
        listRewind(old_incr,&li);
        while ((ln = listNext(&li)) != NULL) {
            aofInfo *ai = listNodeValue(ln);
            sds path = aofGetAofPath(ai->file_name);
            unlink(path);
            sdsfree(path);
        }
        listRelease(old_incr);
        if (old_legacy) {
            unlink(old_legacy);
            sdsfree(old_legacy);
        }
#ifndef _WIN32
        if (oldfd != -1)
            bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)oldfd,NULL,NULL);
#endif

        server.aof_lastbgrewrite_status = C_OK;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);

rewrite_cleanup:
        if (newfd != -1) close(newfd);
        if (base_name) sdsfree(base_name);
        if (incr_name) sdsfree(incr_name);
        if (base_path) sdsfree(base_path);
        if (incr_path) sdsfree(incr_path);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error condition. */
        if (bysignal != SIGUSR1)
            server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofClosePipes();
    aofRewriteBufferReset();
    aofRemoveTempFile(IF_WIN32(getpid(), server.aof_child_pid));
    server.aof_child_pid = -1;
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}

#ifdef _WIN32
void aofProcessDiffRewriteEvents(aeEventLoop* eventLoop)
{
    // only do these checks in the parent process and if an aof rewrite is in progress
    if (server.aof_child_pid != -1 && server.aof_pipe_read_ack_from_child != -1) {
        //1) check if more data can be written to the child and write it.
        // in which case we dont need to send any more diffs to the parent
        if (server.aof_stop_sending_diff == 0) {
            aofChildWriteDiffData(eventLoop,server.aof_pipe_write_data_to_child,NULL,0);
        }

        //2) check if child has signaled parent to stop sending diffs
        if (server.aof_stop_sending_diff == 0) {
            aofChildPipeReadable(eventLoop,server.aof_pipe_read_ack_from_child,NULL,0);
        }
    }
}
#endif