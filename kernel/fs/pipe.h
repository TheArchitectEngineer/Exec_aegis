#ifndef PIPE_H
#define PIPE_H

#include "vfs.h"
#include "sched.h"
#include "spinlock.h"
#include "../sched/waitq.h"
#include <stdint.h>

/*
 * PIPE_BUF_SIZE is chosen so that sizeof(pipe_t) == 4096 exactly.
 *
 * Layout:
 *   buf[PIPE_BUF_SIZE]          = PIPE_BUF_SIZE bytes
 *   read_pos/write_pos/count    = 3 * 4 = 12 bytes
 *   read_refs/write_refs        = 2 * 4 =  8 bytes   (total uint32 = 20 bytes)
 *   lock (spinlock_t)           = 4 bytes             (fills alignment gap)
 *   reader_waiting (pointer)    = 8 bytes
 *   writer_waiting (pointer)    = 8 bytes             (total ptrs = 16 bytes)
 *   read_waiters  (waitq_t)     = 16 bytes (8 ptr + 4 spinlock + 4 pad)
 *   write_waiters (waitq_t)     = 16 bytes
 *
 * 4024 + 20 + 4 + 16 + 32 = 4096 = one kva page.
 * Verified by _Static_assert in pipe.c.
 */
#define PIPE_BUF_SIZE 4024

typedef struct {
    uint8_t          buf[PIPE_BUF_SIZE];
    uint32_t         read_pos;
    uint32_t         write_pos;
    uint32_t         count;           /* bytes currently buffered */
    uint32_t         read_refs;       /* number of open read-end fds */
    uint32_t         write_refs;      /* number of open write-end fds */
    spinlock_t       lock;            /* per-pipe lock for SMP safety */
    aegis_task_t    *reader_waiting;  /* task blocked on empty pipe */
    aegis_task_t    *writer_waiting;  /* task blocked on full pipe */
    /* Wake queues for sys_poll / sys_epoll_wait waiters on each end.
     * read_waiters: pollers on the read end (wake when data arrives or
     *   write end closes — POLLIN / POLLHUP).
     * write_waiters: pollers on the write end (wake when buffer drains
     *   or read end closes — POLLOUT / POLLHUP). */
    waitq_t          read_waiters;
    waitq_t          write_waiters;
} pipe_t;

/*
 * g_pipe_read_ops / g_pipe_write_ops — installed by sys_pipe2 into fds[].
 * Defined in pipe.c; declared here for use in syscall.c.
 */
extern const vfs_ops_t g_pipe_read_ops;
extern const vfs_ops_t g_pipe_write_ops;

#endif /* PIPE_H */
