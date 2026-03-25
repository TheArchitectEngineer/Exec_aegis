/* kernel/net/socket.c — socket table */
#include "socket.h"
#include "proc.h"
#include "vfs.h"
#include "printk.h"
#include <stdint.h>

static sock_t s_socks[SOCK_TABLE_SIZE];  /* zero-initialized by C runtime */

/* ── Socket VFS ops ─────────────────────────────────────────────────────── */

static int  sock_vfs_read(void *priv, void *buf, uint64_t off, uint64_t len);
static int  sock_vfs_write(void *priv, const void *buf, uint64_t len);
static void sock_vfs_close(void *priv);
static void sock_vfs_dup(void *priv);
static int  sock_vfs_stat(void *priv, k_stat_t *st);

static const vfs_ops_t s_sock_ops = {
    .read    = sock_vfs_read,
    .write   = sock_vfs_write,
    .close   = sock_vfs_close,
    .readdir = (void *)0,
    .dup     = sock_vfs_dup,
    .stat    = sock_vfs_stat,
};

static int sock_vfs_read(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)buf; (void)off; (void)len;
    return -38;  /* ENOSYS — use recvfrom */
}

static int sock_vfs_write(void *priv, const void *buf, uint64_t len)
{
    (void)priv; (void)buf; (void)len;
    return -38;  /* ENOSYS — use sendto */
}

static void sock_vfs_close(void *priv)
{
    uint32_t sock_id = (uint32_t)(uintptr_t)priv;
    sock_free(sock_id);
}

static void sock_vfs_dup(void *priv)
{
    (void)priv;  /* sockets have no refcount — each fd is independent */
}

static int sock_vfs_stat(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode = 0140666;  /* S_IFSOCK | 0666 */
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int sock_alloc(uint8_t type)
{
    uint32_t i;
    for (i = 0; i < SOCK_TABLE_SIZE; i++) {
        if (s_socks[i].state == SOCK_FREE) {
            __builtin_memset(&s_socks[i], 0, sizeof(s_socks[i]));
            s_socks[i].state       = SOCK_CREATED;
            s_socks[i].type        = type;
            s_socks[i].tcp_conn_id = SOCK_NONE;
            s_socks[i].epoll_id    = SOCK_NONE;
            return (int)i;
        }
    }
    return -1;
}

sock_t *sock_get(uint32_t sock_id)
{
    if (sock_id >= SOCK_TABLE_SIZE) return (sock_t *)0;
    if (s_socks[sock_id].state == SOCK_FREE) return (sock_t *)0;
    return &s_socks[sock_id];
}

void sock_free(uint32_t sock_id)
{
    if (sock_id >= SOCK_TABLE_SIZE) return;
    s_socks[sock_id].state = SOCK_FREE;
    s_socks[sock_id].waiter_task = (aegis_task_t *)0;
}

void sock_wake(uint32_t sock_id)
{
    sock_t *s = sock_get(sock_id);
    if (!s) return;
    if (s->waiter_task) {
        sched_wake(s->waiter_task);
        s->waiter_task = (aegis_task_t *)0;
    }
}

int sock_open_fd(uint32_t sock_id, aegis_process_t *proc)
{
    uint32_t fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (!proc->fds[fd].ops) {
            proc->fds[fd].ops    = &s_sock_ops;
            proc->fds[fd].priv   = (void *)(uintptr_t)sock_id;
            proc->fds[fd].offset = 0;
            proc->fds[fd].size   = 0;
            proc->fds[fd].flags  = 0;
            proc->fds[fd]._pad   = 0;
            return (int)fd;
        }
    }
    return -1;  /* EMFILE */
}

uint32_t sock_id_from_fd(int fd, aegis_process_t *proc)
{
    if (fd < 0 || (uint32_t)fd >= PROC_MAX_FDS) return SOCK_NONE;
    if (!proc->fds[fd].ops) return SOCK_NONE;
    if (proc->fds[fd].ops != &s_sock_ops) return SOCK_NONE;
    return (uint32_t)(uintptr_t)proc->fds[fd].priv;
}
