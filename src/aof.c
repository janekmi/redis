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

#include "redis.h"
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

void aofUpdateCurrentSize(void);

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
    unsigned long used, free;
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
unsigned long aofRewriteBufferSize(void) {
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    if (block == NULL) return 0;
    unsigned long size =
        (listLength(server.aof_rewrite_buf_blocks)-1) * AOF_RW_BUF_BLOCK_SIZE;
    size += block->used;
    return size;
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed. */
void aofRewriteBufferAppend(unsigned char *s, unsigned long len) {
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    while(len) {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. */
        if (block) {
            unsigned long thislen = (block->free < len) ? block->free : len;
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
            block->free = AOF_RW_BUF_BLOCK_SIZE;
            block->used = 0;
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. */
            numblocks = listLength(server.aof_rewrite_buf_blocks);
            if (((numblocks+1) % 10) == 0) {
                int level = ((numblocks+1) % 100) == 0 ? REDIS_WARNING :
                                                         REDIS_NOTICE;
                redisLog(level,"Background AOF buffer size: %lu MB",
                    aofRewriteBufferSize()/(1024*1024));
            }
        }
    }
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

#ifdef USE_NVML
ssize_t aofRewriteBufferWritePMEMlog(PMEMlogpool* plp) {
    listNode *ln;
    listIter li;
    ssize_t count = 0;
    int appended_bytes;

    listRewind(server.aof_rewrite_buf_blocks,&li);
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);

        if (block->used) {

            appended_bytes = pmemlog_append(plp, block->buf, block->used);
            if (appended_bytes == -1) {
                redisLog(REDIS_ERR, "Error appending AOF NVML diff: %s", strerror(errno));
                return -1;
            }

            count += block->used;
        }
    }
    return count;
}
#endif

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
void aof_background_fsync(int fd) {
    bioCreateBackgroundJob(REDIS_BIO_AOF_FSYNC,(void*)(long)fd,NULL,NULL);
}

#ifdef USE_NVML
/* Open additional PMEMlog file, which will be used for rewrites, both files
 * will be available at launch, when rewrite will be triggered, then rewr pool
 * will be populated with fresh data, buffer will be appended and pointers of
 * pools will be exchanged */
int openAppendOnlyFileREWR(void) {
    int rewr_len;
    struct redis_stat sb;
    /* server.aof_filename + '_rewr' + '\0' */
    rewr_len = strlen(server.aof_filename) + 6;
    server.aof_filename_rewr = (char*) zmalloc(rewr_len * sizeof(char));
    snprintf(server.aof_filename_rewr, rewr_len, "%s_rewr", server.aof_filename);

    if (redis_stat(server.aof_filename_rewr,&sb) != -1) {
        if ((server.aof_plp_rewr = pmemlog_open(server.aof_filename_rewr)) == NULL) {
            redisLog(REDIS_WARNING, "Redis cannot open NVML AOF REWR file: %s", strerror(errno));
            exit(1);
        }
    } else {
        if ((server.aof_plp_rewr = pmemlog_create(server.aof_filename_rewr, server.aof_nvml_log_size, 0600)) == NULL) {
            redisLog(REDIS_WARNING, "Redis cannot create NVML AOF REWR file: %s", strerror(errno));
            return REDIS_ERR;
        }
    }

    return REDIS_OK;
}
#endif

void openAppendOnlyFile(void)
{
    if (server.aof_fd != -1) {
        return;
    }
#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        /* First open regular AOF PMEMLOG pool, then pool for rewrite
         * and if size of loaded file is different, than configured then
         * trigger rewrite */

        int stat_ret;
        struct stat *stat_buf;

        /* if file doesn't exist create it and truncate to configured size */
        stat_buf = zmalloc(sizeof (struct stat));
        stat_ret = stat(server.aof_filename, stat_buf);
        if (stat_ret != 0) {
            if (errno != ENOENT) {
                redisLog(REDIS_WARNING, "Can't stat the AOF file: %s",
                         strerror(errno));
                exit(1);
            } else {
                // file don't exist: create it
                if ((server.aof_plp = pmemlog_create(server.aof_filename, server.aof_nvml_log_size, 0600)) == NULL) {
                    redisLog(REDIS_WARNING, "Redis cannot create NVML AOF file: %s", strerror(errno));
                    exit(1);
                }
            }
        } else {
            // file exists: open it
            if ((server.aof_plp = pmemlog_open(server.aof_filename)) == NULL) {
                redisLog(REDIS_WARNING, "Redis cannot open NVML AOF file: %s", strerror(errno));
                exit(1);
            }
        }
        zfree(stat_buf);

        openAppendOnlyFileREWR();

        aofUpdateCurrentSize();
    } else {
#endif
        /* Regular AOF file */
        server.aof_fd = open(server.aof_filename, O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.aof_fd == -1) {
            redisLog(REDIS_WARNING, "Can't open the append-only file: %s", strerror(errno));
            exit(1);
        }
#ifdef USE_NVML
    }
#endif
}

#ifdef USE_NVML
void closeAppendOnlyFileREWR(void) {
    if (server.aof_plp_rewr != NULL) {
        pmemlog_close(server.aof_plp_rewr);
        server.aof_plp_rewr = NULL;

        close(server.aof_fd_rewr);
        server.aof_fd_rewr = -1;
    }
}
#endif

int closeAppendOnlyFile(void)
{
#ifdef USE_NVML
    if (server.aof_use_nvml == 1
            && server.aof_plp != NULL) {
        pmemlog_close(server.aof_plp);
        server.aof_plp = NULL;

        closeAppendOnlyFileREWR();
    } else {
#endif
        /* Append only file: fsync() the AOF and exit */
        redisLog(REDIS_NOTICE,"Calling fsync() on the AOF file before closing it.");
        aof_fsync(server.aof_fd);
#ifdef USE_NVML
    }
#endif

    close(server.aof_fd);
    server.aof_fd = -1;

    return REDIS_OK;
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
void stopAppendOnly(void) {
    redisAssert(server.aof_state != REDIS_AOF_OFF);
    flushAppendOnlyFile(1);
    closeAppendOnlyFile();

    server.aof_selected_db = -1;
    server.aof_state = REDIS_AOF_OFF;
    /* rewrite operation in progress? kill it, wait child exit */
    if (server.aof_child_pid != -1) {
        int statloc;

        redisLog(REDIS_NOTICE,"Killing running AOF rewrite child: %ld",
            (long) server.aof_child_pid);
        if (kill(server.aof_child_pid,SIGUSR1) != -1)
            wait3(&statloc,0,NULL);
        /* reset the buffer accumulating changes while the child saves */
        aofRewriteBufferReset();
        aofRemoveTempFile(server.aof_child_pid);
        server.aof_child_pid = -1;
        server.aof_rewrite_time_start = -1;
    }
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
int startAppendOnly(void) {
    server.aof_last_fsync = server.unixtime;
    redisAssert(server.aof_state == REDIS_AOF_OFF);
    openAppendOnlyFile();
    if (server.aof_fd == -1) {
        redisLog(REDIS_WARNING,"Redis needs to enable the AOF but can't open the append only file: %s",strerror(errno));
        return REDIS_ERR;
    }
    if (rewriteAppendOnlyFileBackground() == REDIS_ERR) {
        closeAppendOnlyFile();
        redisLog(REDIS_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
        return REDIS_ERR;
    }
    /* We correctly switched on AOF, now wait for the rewrite to be complete
     * in order to append data on disk. */
    server.aof_state = REDIS_AOF_WAIT_REWRITE;
    return REDIS_OK;
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
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;

    if (sdslen(server.aof_buf) == 0) return;

#ifdef USE_NVML
    if (server.aof_use_nvml == 0) {
#endif
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
            sync_in_progress = bioPendingJobsOfType(REDIS_BIO_AOF_FSYNC) != 0;

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
                redisLog(REDIS_NOTICE, "Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
            }
        }
#ifdef USE_NVML
    }
#endif
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */

    latencyStartMonitor(latency);
#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        size_t len = sdslen(server.aof_buf);
        size_t newlen = server.aof_current_size + len;

        /* Check if there's required space, if not try to rewrite first */
        if (newlen > server.aof_nvml_log_size)
        {
            /* if there's not enough space try to compact AOF file */
            redisLog(REDIS_ERR, "Not enough space to write AOF file");
            redisLog(REDIS_NOTICE, "Starting background rewrite of AOF file");
            if (rewriteAppendOnlyFileBackground() != REDIS_OK) {
                redisLog(REDIS_ERR, "Starting background rewrite of AOF file failed!");
            }
            nwritten = -1;
            errno = ENOSPC;
        } else {
            if (!pmemlog_append(server.aof_plp, server.aof_buf, len)) {
                nwritten = len;

                /* if reached threshold for AOF rewrite it */
                if (server.aof_rewrite_perc > 0 &&
                        server.aof_current_size > (server.aof_nvml_log_size *
                                               server.aof_rewrite_perc / 100)) {
                    redisLog(REDIS_NOTICE, "Starting background rewrite of AOF file");
                    if (rewriteAppendOnlyFileBackground() != REDIS_OK) {
                        redisLog(REDIS_ERR, "Starting background rewrite of AOF file failed!");
                    }
                }
            } else {
                nwritten = -1;
            }
        }
    } else {
#endif
        nwritten = write(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
#ifdef USE_NVML
    }
#endif
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

    if (nwritten != (signed)sdslen(server.aof_buf)) {
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
                redisLog(REDIS_WARNING,"Error writing to the AOF file: %s",
                    strerror(errno));
                server.aof_last_write_errno = errno;
            }
        } else {
            if (can_log) {
                redisLog(REDIS_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (long long)nwritten,
                                       (long long)sdslen(server.aof_buf));
            }

            if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
                if (can_log) {
                    redisLog(REDIS_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", strerror(errno));
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
            redisLog(REDIS_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            server.aof_last_write_status = REDIS_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... */
        }
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        if (server.aof_last_write_status == REDIS_ERR) {
            redisLog(REDIS_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = REDIS_OK;
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

    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    if (server.aof_no_fsync_on_rewrite &&
        (server.aof_child_pid != -1 || server.rdb_child_pid != -1))
            return;

    /* Perform the fsync if needed. */
#ifdef USE_NVML
    if (server.aof_use_nvml == 0) {
#endif
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* aof_fsync is defined as fdatasync() for Linux in order to avoid
             * flushing metadata. */
            latencyStartMonitor(latency);
            aof_fsync(server.aof_fd); /* Let's try to get this data on the disk */
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-fsync-always",latency);
            server.aof_last_fsync = server.unixtime;
        } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                    server.unixtime > server.aof_last_fsync)) {
            if (!sync_in_progress) aof_background_fsync(server.aof_fd);
            server.aof_last_fsync = server.unixtime;
        }
#ifdef USE_NVML
    }
#endif
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
    long long when;
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

void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    robj *tmpargv[3];

    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. */
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
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
    } else {
        /* All the other commands don't need translation or need the
         * same translation already operated in the command vector
         * for the replication itself. */
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    if (server.aof_state == REDIS_AOF_ON)
    {
#ifdef USE_NVML
        if (server.aof_use_nvml == 1 && server.aof_nvml_direct == 1) {
            size_t len = sdslen(buf);
            size_t newlen = server.aof_current_size + len;

            /* Check if there's required space, if not try to rewrite first */
            if (newlen > server.aof_nvml_log_size)
            {
                /* if there's not enough space try to compact AOF file */
                redisLog(REDIS_ERR, "Not enough space to write AOF file");
                redisLog(REDIS_NOTICE, "Starting background rewrite of AOF file");
                if (rewriteAppendOnlyFileBackground() != REDIS_OK) {
                    redisLog(REDIS_ERR, "Starting background rewrite of AOF file failed!");
                }
                errno = ENOSPC;
            } else {
                if (!pmemlog_append(server.aof_plp, buf, len)) {
                    server.aof_current_size += len;
                } else {
                    redisLog(REDIS_WARNING,"Error writing to the AOF file: %s",
                        strerror(errno));

                    /* fallback to buffer when write failed */
                    server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));
                }
            }
        } else {
#endif
            server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));
#ifdef USE_NVML
        }
#endif
    }

#ifdef USE_NVML
    /* If aof_use_nvml is in direct mode it didn't use append buffer */
    if (!(server.aof_use_nvml == 1 && server.aof_nvml_direct == 1))
#endif
        /* If a background append only file rewriting is in progress we want to
         * accumulate the differences between the child DB and the current one
         * in a buffer, so that when the child process will do its work we
         * can append the differences to the new append only file. */
        if (server.aof_child_pid != -1)
            aofRewriteBufferAppend((unsigned char*)buf,sdslen(buf));

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
struct redisClient *createFakeClient(void) {
    struct redisClient *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->fd = -1;
    c->name = NULL;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

void freeFakeClientArgv(struct redisClient *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);
}

void freeFakeClient(struct redisClient *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    zfree(c);
}

/* Replay the append log file. On success REDIS_OK is returned. On non fatal
 * error (the append only file is zero-length) REDIS_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
int loadAppendOnlyFile(char *filename) {
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int old_aof_state = server.aof_state;
    long loops = 0;
    off_t valid_up_to = 0; /* Offset of the latest well-formed command loaded. */

    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.aof_current_size = 0;
        fclose(fp);
        return REDIS_ERR;
    }

    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.aof_state = REDIS_AOF_OFF;

    fakeClient = createFakeClient();
    startLoading(fp);

    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            processEventsWhileBlocked();
        }

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;

        argv = zmalloc(sizeof(robj*)*argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;

        for (j = 0; j < argc; j++) {
            if (fgets(buf,sizeof(buf),fp) == NULL) {
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            if (buf[0] != '$') goto fmterr;
            len = strtol(buf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            argv[j] = createObject(REDIS_STRING,argsds);
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. */
                freeFakeClientArgv(fakeClient);
                goto readerr; /* discard CRLF */
            }
        }

        /* Command lookup */
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            redisLog(REDIS_WARNING,"Unknown command '%s' reading the append only file", (char*)argv[0]->ptr);
            exit(1);
        }

        /* Run the command in the context of a fake client */
        cmd->proc(fakeClient);

        /* The fake client should not have a reply */
        redisAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
        /* The fake client should never get blocked */
        redisAssert((fakeClient->flags & REDIS_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        freeFakeClientArgv(fakeClient);
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, log error and quit. */
    if (fakeClient->flags & REDIS_MULTI) goto uxeof;

loaded_ok: /* DB loaded, cleanup and return REDIS_OK to the caller. */
    fclose(fp);
    freeFakeClient(fakeClient);
    server.aof_state = old_aof_state;
    stopLoading();
    aofUpdateCurrentSize();
    server.aof_rewrite_base_size = server.aof_current_size;
    return REDIS_OK;

readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
    if (!feof(fp)) {
        redisLog(REDIS_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
        exit(1);
    }

uxeof: /* Unexpected AOF end of file. */
    if (server.aof_load_truncated) {
        redisLog(REDIS_WARNING,"!!! Warning: short read while loading the AOF file !!!");
        redisLog(REDIS_WARNING,"!!! Truncating the AOF at offset %llu !!!",
            (unsigned long long) valid_up_to);
        if (valid_up_to == -1 || truncate(filename,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                redisLog(REDIS_WARNING,"Last valid command offset is invalid");
            } else {
                redisLog(REDIS_WARNING,"Error truncating the AOF file: %s",
                    strerror(errno));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                redisLog(REDIS_WARNING,"Can't seek the end of the AOF file: %s",
                    strerror(errno));
            } else {
                redisLog(REDIS_WARNING,
                    "AOF loaded anyway because aof-load-truncated is enabled");
                goto loaded_ok;
            }
        }
    }
    redisLog(REDIS_WARNING,"Unexpected end of file reading the append only file. You can: 1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename>. 2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.");
    exit(1);

fmterr: /* Format error. */
    redisLog(REDIS_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

#ifdef USE_NVML
/* Callback for pmemlog_walk function */
int loadPMEMLogbuffer(const void *buf, size_t buflen, void *arg) {
    struct redisClient *fakeClient;
    char *bufchar = (char*) buf;
    const char *endptr = bufchar + buflen; /* end of load buffer */
    char *tmpptr;
    long loops = 0;
    char tmpbuf[32];

    fakeClient = createFakeClient();

    while (bufchar < endptr) {
        int argc, j;
        //size_t buflen;
        unsigned long len;
        robj **argv;
        sds argsds;
        struct redisCommand *cmd;

        if (!(loops++ % 1000)) {
            processEventsWhileBlocked();
        }

        tmpptr = strpbrk(bufchar, "\r");
        len = tmpptr - bufchar;
        if (len  > 32) goto fmterr;
        strncpy(tmpbuf, bufchar, len);
        tmpbuf[len] = '\0';

        if (tmpbuf[0] != '*') goto fmterr;
        argc = atoi(tmpbuf+1);
        if (argc < 1) goto fmterr;

        bufchar = tmpptr + 2 * sizeof(char);
        if (bufchar > endptr) goto fmterr;

        argv = zmalloc(sizeof(robj*)*argc);
        for (j = 0; j < argc; j++) {
            tmpptr = strpbrk(bufchar, "\r");
            len = tmpptr - bufchar;
            if (len > 32) goto fmterr;
            strncpy(tmpbuf, bufchar, len);
            tmpbuf[len] = '\0';

            if (tmpbuf[0] != '$') goto fmterr;
            len = strtol(tmpbuf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            bufchar = tmpptr + 2 * sizeof(char);
            if (bufchar > endptr) goto fmterr;

            tmpptr = strpbrk(bufchar, "\r");
            memcpy(argsds, bufchar, sizeof(char) * len);
            bufchar = tmpptr + 2 * sizeof(char);
            if (bufchar > endptr) goto fmterr;

            argv[j] = createObject(REDIS_STRING,argsds);
        }

        /* Command lookup */
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            redisLog(REDIS_WARNING,"Unknown command '%s' reading the append only file", (char*)argv[0]->ptr);
            exit(1);
        }
        /* Run the command in the context of a fake client */
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);

        /* The fake client should not have a reply */
        redisAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
        /* The fake client should never get blocked */
        redisAssert((fakeClient->flags & REDIS_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        for (j = 0; j < fakeClient->argc; j++)
            decrRefCount(fakeClient->argv[j]);
        zfree(fakeClient->argv);
    }

    freeFakeClient(fakeClient);
    return 0;

fmterr:
    redisLog(REDIS_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}


/* Replay the append log file. On error REDIS_OK is returned. On non fatal
 * error (the append only file is zero-length) REDIS_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
int loadAppendOnlyFilePMEMlog() {
    int old_aof_state = server.aof_state;

    /* We're loading the data from an open AOF. */
    if (server.aof_current_size == 0) {
        redisLog(REDIS_WARNING,"Empty AOF file (size == 0)");
        return REDIS_OK;
    }

    if (server.aof_plp == NULL) {
        redisLog(REDIS_WARNING,"Incorrect mapped AOF NVML pointer (NULL)");
        return REDIS_ERR;
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.aof_state = REDIS_AOF_OFF;

    /* Load the DB */
    server.loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_total_bytes = server.aof_current_size;

    /* If aof_mmap is enabled, the AOF file is already open and mmap'ed at initServer(). */
    pmemlog_walk(server.aof_plp, 0, loadPMEMLogbuffer, NULL);

    /* Do not unmap/close the AOF file! */

    server.aof_state = old_aof_state;
    stopLoading();

    aofUpdateCurrentSize();
    server.aof_rewrite_base_size = server.aof_current_size;

    if (server.aof_trigger_rewrite) {
        server.aof_trigger_rewrite = 0;
        rewriteAppendOnlyFileBackground();
    }
    return REDIS_OK;
}
#endif

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */

/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the redis.h dependency. */
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    if (obj->encoding == REDIS_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    } else if (obj->encoding == REDIS_ENCODING_RAW) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        redisPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        unsigned char *p = ziplistIndex(zl,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        while(ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (count == 0) {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                    REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (vstr) {
                if (rioWriteBulkString(r,(char*)vstr,vlen) == 0) return 0;
            } else {
                if (rioWriteBulkLongLong(r,vlong) == 0) return 0;
            }
            p = ziplistNext(zl,p);
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        list *list = o->ptr;
        listNode *ln;
        listIter li;

        listRewind(list,&li);
        while((ln = listNext(&li))) {
            robj *eleobj = listNodeValue(ln);

            if (count == 0) {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                    REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);

    if (o->encoding == REDIS_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval)) {
            if (count == 0) {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                    REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkLongLong(r,llval) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == REDIS_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            robj *eleobj = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                    REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        redisPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = ziplistIndex(zl,0);
        redisAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        redisAssert(sptr != NULL);

        while (eptr != NULL) {
            redisAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                    REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
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
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            robj *eleobj = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                    REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            if (rioWriteBulkDouble(r,*score) == 0) return 0;
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        redisPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 * The 'hi' argument passes a valid Redis hash iterator.
 * The 'what' filed specifies if to write a key or a value and can be
 * either REDIS_HASH_KEY or REDIS_HASH_VALUE.
 *
 * The function returns 0 on error, non-zero on success. */
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr) {
            return rioWriteBulkString(r, (char*)vstr, vlen);
        } else {
            return rioWriteBulkLongLong(r, vll);
        }

    } else if (hi->encoding == REDIS_ENCODING_HT) {
        robj *value;

        hashTypeCurrentFromHashTable(hi, what, &value);
        return rioWriteBulkObject(r, value);
    }

    redisPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR) {
        if (count == 0) {
            int cmd_items = (items > REDIS_AOF_REWRITE_ITEMS_PER_CMD) ?
                REDIS_AOF_REWRITE_ITEMS_PER_CMD : items;

            if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
            if (rioWriteBulkString(r,"HMSET",5) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
        }

        if (rioWriteHashIteratorCursor(r, hi, REDIS_HASH_KEY) == 0) return 0;
        if (rioWriteHashIteratorCursor(r, hi, REDIS_HASH_VALUE) == 0) return 0;
        if (++count == REDIS_AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max REDIS_AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
int rewriteAppendOnlyFile(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    rio aof;
    FILE *fp;
    char tmpfile[256];
    int j;
    long long now = mstime();

#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        rioInitWithPMEMLog(&aof, server.aof_plp_rewr);
    } else {
#endif
        /*  Note that we have to use a different temp name here compared to the
         * one used by rewriteAppendOnlyFileBackground() function. */
        snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
        fp = fopen(tmpfile,"w");
        if (!fp) {
            redisLog(REDIS_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
            return REDIS_ERR;
        }

        rioInitWithFile(&aof,fp);
        if (server.aof_rewrite_incremental_fsync)
            rioSetAutoSync(&aof,REDIS_AOF_AUTOSYNC_BYTES);
#ifdef USE_NVML
     }
#endif

    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }

        /* SELECT the new DB */
        if (rioWrite(&aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(&aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;

            keystr = dictGetKey(de);
            o = dictGetVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* If this key is already expired skip it */
            if (expiretime != -1 && expiretime < now) continue;

            /* Save the key and associated value */
            if (o->type == REDIS_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(&aof,o) == 0) goto werr;
            } else if (o->type == REDIS_LIST) {
                if (rewriteListObject(&aof,&key,o) == 0) goto werr;
            } else if (o->type == REDIS_SET) {
                if (rewriteSetObject(&aof,&key,o) == 0) goto werr;
            } else if (o->type == REDIS_ZSET) {
                if (rewriteSortedSetObject(&aof,&key,o) == 0) goto werr;
            } else if (o->type == REDIS_HASH) {
                if (rewriteHashObject(&aof,&key,o) == 0) goto werr;
            } else {
                redisPanic("Unknown object type");
            }
            /* Save the expire time */
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(&aof,expiretime) == 0) goto werr;
            }
        }
        dictReleaseIterator(di);
        di = NULL;
    }

#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        if (server.aof_state == REDIS_AOF_OFF) {
            closeAppendOnlyFile();
        }
    } else {
#endif
        /* Make sure data will not remain on the OS's output buffers */
        if (fflush(fp) == EOF) goto werr;
        if (fsync(fileno(fp)) == -1) goto werr;
        if (fclose(fp) == EOF) goto werr;

        /* Use RENAME to make sure the DB file is changed atomically only
         * if the generate DB file is ok. */
        if (rename(tmpfile,filename) == -1) {
            redisLog(REDIS_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
            unlink(tmpfile);
            return REDIS_ERR;
        }
#ifdef USE_NVML
     }
#endif
    redisLog(REDIS_NOTICE,"SYNC append only file rewrite performed");
    return REDIS_OK;

werr:
    redisLog(REDIS_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        if (server.aof_state == REDIS_AOF_OFF) {
            closeAppendOnlyFile();
        } else {
            pmemlog_rewind(server.aof_plp_rewr);
        }
    } else {
#endif
        fclose(fp);
        unlink(tmpfile);
#ifdef USE_NVML
    }
#endif
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

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
    long long start;

    if (server.aof_child_pid != -1) return REDIS_ERR;
    start = ustime();
#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        if (server.aof_state == REDIS_AOF_OFF && server.aof_fd == -1) {
            openAppendOnlyFile();
        }

        if (server.aof_nvml_direct == 1) {
            server.aof_nvml_direct_pos = pmemlog_tell(server.aof_plp);
        }
    }
#endif
    if ((childpid = fork()) == 0) {
        char tmpfile[256];

        /* Child */
        closeListeningSockets(0);
        redisSetProcTitle("redis-aof-rewrite");
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "AOF rewrite: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
            exitFromChild(0);
        } else {
            exitFromChild(1);
        }
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            redisLog(REDIS_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,
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
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

void bgrewriteaofCommand(redisClient *c) {
    if (server.aof_child_pid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (server.rdb_child_pid != -1) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == REDIS_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReply(c,shared.err);
    }
}

void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}

/* Update the server.aof_current_size field explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current length, that is much faster. */
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    mstime_t latency;

    latencyStartMonitor(latency);
#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        if (server.aof_plp != NULL)
            server.aof_current_size = pmemlog_tell(server.aof_plp);
    } else
#endif
    if (redis_fstat(server.aof_fd,&sb) == -1) {
        redisLog(REDIS_WARNING,"Unable to obtain the AOF file length. stat: %s",
            strerror(errno));
    } else {
        server.aof_current_size = sb.st_size;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat",latency);
}

#ifdef USE_NVML
/* Copy difference between PMEMLOG pools */
int copyPMEMLogDiff(const void *buf, size_t end, void *arg) {
    buf = buf + server.aof_nvml_direct_pos;
    if (pmemlog_append(server.aof_plp_rewr, buf, end - server.aof_nvml_direct_pos) == -1) {
        redisLog(REDIS_WARNING, "I/O error at applying diff to rewriten AOF: %s, from: %zu to: %zu",
                strerror(errno), server.aof_nvml_direct_pos, end);
        return 1;
    }

    return 0;
}
#endif

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        int newfd, oldfd;
        char tmpfile[256];
        long long now = ustime();
        mstime_t latency;

        redisLog(REDIS_NOTICE,
            "Background AOF rewrite terminated with success");

        /* Flush the differences accumulated by the parent to the
         * rewritten AOF. */
        latencyStartMonitor(latency);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.aof_child_pid);

#ifdef USE_NVML
        if (server.aof_use_nvml == 1) {
            /*size_t real_log_rewr_size; */
            if (server.aof_nvml_direct == 1) {
                pmemlog_walk(server.aof_plp, 0, copyPMEMLogDiff, NULL);
            } else {
                if (aofRewriteBufferWritePMEMlog(server.aof_plp_rewr) == -1) {
                    redisLog(REDIS_WARNING,
                        "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
                    goto cleanup;
                }
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rewrite-diff-write",latency);

            redisLog(REDIS_NOTICE,
                "Parent diff successfully flushed to the rewritten AOF (%lu bytes)", aofRewriteBufferSize());

            /* swap filenames, swap fd's  and plp's */
            latencyStartMonitor(latency);
            snprintf(tmpfile, 256, "aof-rewrite-swap-%d.aof", getpid());
            if (rename (server.aof_filename, tmpfile) == -1) {
                redisLog(REDIS_WARNING, "Redis cannot rename AOF to temporary file: %s",
                        strerror(errno));
                goto cleanup;
            }
            if (rename(server.aof_filename_rewr, server.aof_filename) == -1) {
                redisLog(REDIS_WARNING, "Redis cannot rename AOF rewrite file to AOF: %s",
                        strerror(errno));
                goto cleanup;
            }
            if (rename(tmpfile, server.aof_filename_rewr) == -1) {
                redisLog(REDIS_WARNING, "Redis cannot rename temporary file to AOF rewrite: %s",
                        strerror(errno));
                goto cleanup;
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rename",latency);

            if (server.aof_state != REDIS_AOF_OFF) {
                oldfd = server.aof_fd;
                server.aof_fd = server.aof_fd_rewr;
                server.aof_fd_rewr = oldfd;

                PMEMlogpool *tmp_plp = server.aof_plp;
                server.aof_plp = server.aof_plp_rewr;
                server.aof_plp_rewr = tmp_plp;

                /* compute real size of rewrite file, if it's different, than configured
                 * the reopen file */
                if ((pmemlog_tell(server.aof_plp) + pmemlog_nbyte(server.aof_plp)) !=
                            (pmemlog_tell(server.aof_plp_rewr) + pmemlog_nbyte(server.aof_plp_rewr))) {
                    /* when file will be reopened then will be truncated to the new
                     * size */
                    closeAppendOnlyFileREWR();
                    openAppendOnlyFileREWR();
                } else {
                    /* rewind rewrite pool */
                    pmemlog_rewind(server.aof_plp_rewr);
                }
                server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
                aofUpdateCurrentSize();
                server.aof_rewrite_base_size = server.aof_current_size;
                /* Clear regular AOF buffer since its contents was just written to
                 * the new AOF from the background rewrite buffer. */
                sdsfree(server.aof_buf);
                server.aof_buf = sdsempty();
            }

            /* we don't want to trigger closing aof_fd_rewr in background */
            oldfd = -1;
        } else {
#endif
            newfd = open(tmpfile,O_WRONLY|O_APPEND);
            if (newfd == -1) {
                   redisLog(REDIS_WARNING,
                                "Unable to open the temporary AOF produced by the child: %s", strerror(errno));
                    goto cleanup;
            }

            if (aofRewriteBufferWrite(newfd) == -1) {
                    redisLog(REDIS_WARNING,
                                "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
                    close(newfd);
                    goto cleanup;
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rewrite-diff-write",latency);

                  redisLog(REDIS_NOTICE,
                                "Parent diff successfully flushed to the rewritten AOF (%lu bytes)", aofRewriteBufferSize());

            /* The only remaining thing to do is to rename the temporary file to
             * the configured file and switch the file descriptor used to do AOF
             * writes. We don't want close(2) or rename(2) calls to block the
             * server on old file deletion.
             *
             * There are two possible scenarios:
             *
             * 1) AOF is DISABLED and this was a one time rewrite. The temporary
             * file will be renamed to the configured file. When this file already
             * exists, it will be unlinked, which may block the server.
             *
             * 2) AOF is ENABLED and the rewritten AOF will immediately start
             * receiving writes. After the temporary file is renamed to the
             * configured file, the original AOF file descriptor will be closed.
             * Since this will be the last reference to that file, closing it
             * causes the underlying file to be unlinked, which may block the
             * server.
             *
             * To mitigate the blocking effect of the unlink operation (either
             * caused by rename(2) in scenario 1, or by close(2) in scenario 2), we
             * use a background thread to take care of this. First, we
             * make scenario 1 identical to scenario 2 by opening the target file
             * when it exists. The unlink operation after the rename(2) will then
             * be executed upon calling close(2) for its descriptor. Everything to
             * guarantee atomicity for this switch has already happened by then, so
             * we don't care what the outcome or duration of that close operation
             * is, as long as the file descriptor is released again. */
            if (server.aof_fd == -1) {
                /* AOF disabled */

                /* Don't care if this fails: oldfd will be -1 and we handle that.
                 * One notable case of -1 return is if the old file does
                 * not exist. */
                 oldfd = open(server.aof_filename,O_RDONLY|O_NONBLOCK);
            } else {
                /* AOF enabled */
                oldfd = -1; /* We'll set this to the current AOF filedes later. */
            }

            /* Rename the temporary file. This will not unlink the target file if
             * it exists, because we reference it with "oldfd". */
            latencyStartMonitor(latency);
            if (rename(tmpfile,server.aof_filename) == -1) {
                redisLog(REDIS_WARNING,
                        "Error trying to rename the temporary AOF file: %s", strerror(errno));
                close(newfd);
                if (oldfd != -1) close(oldfd);
                    goto cleanup;
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rename",latency);

            if (server.aof_fd == -1) {
                /* AOF disabled, we don't need to set the AOF file descriptor
                  * to this new file, so we can close it. */
                close(newfd);
            } else {
                /* AOF enabled, replace the old fd with the new one. */
                oldfd = server.aof_fd;
                server.aof_fd = newfd;

                if (server.aof_fsync == AOF_FSYNC_ALWAYS)
                    aof_fsync(newfd);
                else if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
                    aof_background_fsync(newfd);

                server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
                aofUpdateCurrentSize();
                server.aof_rewrite_base_size = server.aof_current_size;

                /* Clear regular AOF buffer since its contents was just written to
                 * the new AOF from the background rewrite buffer. */
                 sdsfree(server.aof_buf);
                 server.aof_buf = sdsempty();
            }
#ifdef USE_NVML
        }
#endif
        server.aof_lastbgrewrite_status = REDIS_OK;

        redisLog(REDIS_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        if (server.aof_state == REDIS_AOF_WAIT_REWRITE)
            server.aof_state = REDIS_AOF_ON;

        /* Asynchronously close the overwritten AOF. */
        if (oldfd != -1) bioCreateBackgroundJob(REDIS_BIO_CLOSE_FILE,(void*)(long)oldfd,NULL,NULL);

        redisLog(REDIS_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = REDIS_ERR;

        redisLog(REDIS_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        server.aof_lastbgrewrite_status = REDIS_ERR;

        redisLog(REDIS_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

cleanup:
    aofRewriteBufferReset();
#ifdef USE_NVML
    if (server.aof_use_nvml == 1) {
        if (server.aof_state == REDIS_AOF_OFF) {
            closeAppendOnlyFile();
        }
    } else
#endif
        aofRemoveTempFile(server.aof_child_pid);

    server.aof_child_pid = -1;
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    if (server.aof_state == REDIS_AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
