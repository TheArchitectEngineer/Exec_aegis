# Phase 26: Full POSIX Socket API + epoll Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Expose the Phase 25 protocol stack to user processes via a complete POSIX socket API, including blocking operations, non-blocking mode, `poll`, `select`, and `epoll`.

**Architecture:** Sockets are VFS file descriptors — `sys_socket()` allocates a `vfs_file_t` slot in `proc->fds[]` backed by a socket-specific `vfs_ops_t`. A kernel `sock_t` table (64 entries) holds per-socket state. `sched_block`/`sched_wake` provide blocking semantics. epoll uses a separate event table (8 instances, 64 watches each).

**Tech Stack:** C, existing VFS fd machinery, existing `sched_block`/`sched_wake`, musl-compatible ABI.

---

## Constraints and Non-Negotiables

- Only `AF_INET` (IPv4) in v1. `AF_UNIX` and `AF_INET6` return `EAFNOSUPPORT`.
- Only `SOCK_STREAM` (TCP) and `SOCK_DGRAM` (UDP) in v1. `SOCK_RAW` returns `EPROTONOSUPPORT` (reserved for future ICMP raw sockets).
- `sock_t` table: 64 entries. `epoll_fd_t` table: 8 instances, 64 watches per instance.
- All socket operations go through `copy_from_user`/`copy_to_user` for pointer arguments (SMAP safety).
- `sockaddr_in` layout must match musl's `struct sockaddr_in` exactly (verified with `_Static_assert`).
- Capability gate: `sys_socket()` requires a new `CAP_KIND_NET_SOCKET` capability. `proc_spawn` grants it to all user processes in Phase 26 (same pattern as `CAP_KIND_VFS_OPEN`).
- `sys_netcfg` (for DHCP daemon) is capability-gated with `CAP_KIND_NET_ADMIN`. Only granted to the DHCP daemon process explicitly.
- `make test` unaffected — socket syscalls added to dispatch table with no boot-time side effects.

---

## File Layout

| File | Action | Purpose |
|------|--------|---------|
| `kernel/net/socket.h` | Create | `sock_t` struct, socket table API |
| `kernel/net/socket.c` | Create | Socket table, `sock_alloc`, `sock_get`, blocking/wake |
| `kernel/net/epoll.h` + `epoll.c` | Create | epoll instance table, `epoll_notify()` |
|  `kernel/syscall/sys_socket.c` | Create | All socket syscall implementations |
| `kernel/syscall/syscall.c` | Modify | Dispatch new syscall numbers |
| `kernel/cap/src/lib.rs` | Modify | Add `CAP_KIND_NET_SOCKET`, `CAP_KIND_NET_ADMIN` |
| `kernel/proc/proc.c` | Modify | Grant `CAP_KIND_NET_SOCKET` in `proc_spawn` |
| `kernel/net/tcp.c` | Modify | Call `epoll_notify()` + `sched_wake()` on data/connect events |
| `kernel/net/udp.c` | Modify | Call `epoll_notify()` + `sched_wake()` on received datagrams |
| `tests/test_socket.py` | Create | Boot q35 + virtio-net + SLIRP; DHCP; HTTP server; verify response |
| `tests/run_tests.sh` | Modify | Add `test_socket.py` |

---

## Syscall Numbers

Follow Linux x86-64 ABI (musl uses these):

| Syscall | Number | Notes |
|---------|--------|-------|
| `socket` | 41 | |
| `connect` | 42 | |
| `accept` | 43 | |
| `sendto` | 44 | covers `send` (NULL addr) |
| `recvfrom` | 45 | covers `recv` (NULL addr) |
| `sendmsg` | 46 | |
| `recvmsg` | 47 | |
| `shutdown` | 48 | |
| `bind` | 49 | |
| `listen` | 50 | |
| `getsockname` | 51 | |
| `getpeername` | 52 | |
| `socketpair` | 53 | AF_INET SOCK_STREAM loopback pair |
| `setsockopt` | 54 | SO_REUSEADDR, SO_RCVTIMEO, SO_SNDTIMEO, TCP_NODELAY |
| `getsockopt` | 55 | mirrors setsockopt options |
| `select` | 23 | already reserved; implement now |
| `poll` | 7 | already reserved; implement now |
| `epoll_create1` | 291 | |
| `epoll_ctl` | 233 | |
| `epoll_wait` | 232 | |
| `sys_netcfg` | 500 | Aegis-specific; sets IP/mask/gw; CAP_KIND_NET_ADMIN |

`fcntl` (72) already exists — extend to handle `F_GETFL`/`F_SETFL` with `O_NONBLOCK` for socket fds.

---

## Socket Object (`sock_t`)

```c
#define SOCK_TABLE_SIZE 64

typedef enum {
    SOCK_FREE, SOCK_CREATED, SOCK_BOUND, SOCK_LISTENING,
    SOCK_CONNECTING, SOCK_CONNECTED, SOCK_CLOSED
} sock_state_t;

typedef struct {
    sock_state_t state;
    uint8_t      type;         /* SOCK_STREAM or SOCK_DGRAM */
    uint8_t      nonblocking;  /* set by fcntl O_NONBLOCK */
    ip4_addr_t   local_ip;
    uint16_t     local_port;
    ip4_addr_t   remote_ip;
    uint16_t     remote_port;
    uint32_t     tcp_conn_id;  /* index into tcp_conn table; UINT32_MAX if none */
    /* accept queue for LISTEN sockets: ring of completed conn ids */
    uint32_t     accept_queue[8];
    uint8_t      accept_head, accept_tail;
    /* blocking waiter: one task waiting on recv/accept/connect */
    uint32_t     waiter_pid;   /* 0 = none */
    uint32_t     epoll_id;     /* epoll instance watching this socket; UINT32_MAX = none */
    uint64_t     epoll_events; /* EPOLLIN | EPOLLOUT mask */
    uint8_t      reuseaddr;
} sock_t;

static sock_t s_socks[SOCK_TABLE_SIZE];
```

---

## Blocking Model

When `recv()` / `accept()` / `connect()` would block:
1. Set `sock->waiter_pid = current->pid`
2. Call `sched_block(current)` — process removed from run queue
3. When TCP layer delivers data/connection: call `sock_wake(sock_id)` which calls `sched_wake(sock->waiter_pid)`
4. Process re-enters syscall path, checks condition again (spurious-wake safe)

For `O_NONBLOCK`: skip steps 1–4, return `-EAGAIN` immediately.

For `SO_RCVTIMEO` / `SO_SNDTIMEO`: use a PIT-tick deadline. If `arch_get_ticks() >= deadline` before wakeup, return `-ETIMEDOUT`.

---

## epoll Design

```c
#define EPOLL_MAX_INSTANCES 8
#define EPOLL_MAX_WATCHES   64

typedef struct {
    uint32_t fd;
    uint64_t events;   /* EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLHUP, EPOLLET */
    uint64_t data;     /* user data (epoll_data_t union) */
} epoll_watch_t;

typedef struct {
    uint8_t       in_use;
    epoll_watch_t watches[EPOLL_MAX_WATCHES];
    uint8_t       nwatches;
    uint32_t      waiter_pid;
    /* ready list: indices into watches[] that are ready */
    uint8_t       ready[EPOLL_MAX_WATCHES];
    uint8_t       nready;
} epoll_fd_t;

static epoll_fd_t s_epoll[EPOLL_MAX_INSTANCES];
```

`epoll_notify(sock_id, events)` — called by TCP/UDP layers when data arrives or connection completes. Finds all epoll instances watching `sock_id`, marks them ready, wakes any blocked `epoll_wait`.

`epoll_wait` with `timeout == 0`: non-blocking check. `timeout == -1`: block indefinitely. `timeout > 0`: block with PIT-tick deadline.

Edge-triggered (`EPOLLET`) is supported: ready flag is cleared on each `epoll_wait` return, only re-set when new data arrives.

---

## Key Syscall Implementations

### `sys_socket(domain, type, proto)`

1. Verify `domain == AF_INET`, `type == SOCK_STREAM || SOCK_DGRAM`
2. Check `CAP_KIND_NET_SOCKET` capability
3. Allocate `sock_t` slot
4. Allocate `vfs_file_t` fd slot with socket `vfs_ops_t`
5. Return fd

### `sys_bind(fd, addr, addrlen)`

1. `copy_from_user` → `struct sockaddr_in`
2. Validate port not already bound (scan UDP/TCP binding tables)
3. Store `local_ip`, `local_port` in `sock_t`
4. Register in TCP/UDP layer binding table

### `sys_listen(fd, backlog)`

1. Set `sock->state = SOCK_LISTENING`
2. Register in TCP layer as LISTEN entry
3. `backlog` clamped to 8 (accept queue size)

### `sys_accept(fd, addr, addrlen)`

1. If accept queue non-empty: dequeue completed conn, fill `addr`, return new fd
2. If empty and blocking: `sched_block()`, retry on wake
3. If empty and `O_NONBLOCK`: return `-EAGAIN`

### `sys_netcfg(op, arg1, arg2, arg3)`

```c
/* op 0: set IP config */
/* arg1 = ip (network byte order), arg2 = mask, arg3 = gateway */
```
Requires `CAP_KIND_NET_ADMIN`. Calls `net_set_config(ip, mask, gw)`. Prints `[NET] configured: %u.%u.%u.%u/%u gw %u.%u.%u.%u`.

### `socketpair(domain, type, proto, sv[2])`

Creates two connected SOCK_STREAM sockets via loopback (`127.0.0.1`). Allocates two sock_t entries and two fd slots. Used by shell pipes-over-network and IPC patterns.

---

## `sockaddr_in` Layout Verification

```c
#include <stdint.h>
typedef struct {
    uint16_t sin_family;   /* AF_INET = 2 */
    uint16_t sin_port;     /* network byte order */
    uint32_t sin_addr;     /* network byte order */
    uint8_t  sin_zero[8];  /* padding */
} k_sockaddr_in_t;

_Static_assert(sizeof(k_sockaddr_in_t) == 16,
    "k_sockaddr_in_t must be 16 bytes (matches musl struct sockaddr_in)");
```

---

## Testing

### `tests/test_socket.py`

1. Boot with `-machine q35 -device virtio-net-pci,disable-legacy=on -netdev user,id=n0,hostfwd=tcp::8080-:80`
2. Wait for shell prompt
3. Type `dhcp` (blocks until IP assigned from QEMU SLIRP DHCP server → `10.0.2.15`)
4. Type `httpd &` (minimal HTTP server binary, binds port 80, responds to GET /)
5. From Python test: `requests.get("http://localhost:8080/")` → verify 200 response
6. Verify `[NET] configured: 10.0.2.15/24` in serial output

`httpd` binary: shipped in ext2 `/bin/httpd`. Minimal — `socket`/`bind`/`listen`/`accept`/`recv`/`send`/`close` loop. Responds with `HTTP/1.0 200 OK\r\n\r\nHello from Aegis\r\n`.

---

## Forward-Looking Constraints

**`AF_UNIX` sockets deferred.** `socketpair` is implemented via TCP loopback as a workaround. True Unix domain sockets require a separate namespace and are v2.0 work.

**`epoll` max 8 instances, 64 watches.** Sufficient for a simple server. nginx-style event loops with thousands of connections require larger tables or dynamic allocation.

**`select`/`poll` linear scan.** `poll(fds, nfds, timeout)` scans all `nfds` entries. For nfds > 100, performance degrades. `epoll` is the recommended path for high-connection servers.

**Single waiter per socket.** `sock_t.waiter_pid` holds one blocked task. Multiple threads blocking on the same socket fd are not supported (only one will be woken). Multi-threaded `accept` requires a wait queue list — v2.0 work.

**`SO_REUSEPORT` not implemented.** `SO_REUSEADDR` is implemented (allows rebind while in TIME_WAIT). `SO_REUSEPORT` (multiple sockets on same port) is deferred.

**No `sendfile`.** File-to-socket zero-copy requires VFS integration with the network stack. Deferred.

**`sys_netcfg` is Aegis-specific.** musl programs call `ioctl(SIOCSIFADDR)` to configure network interfaces. A future phase may implement the `ioctl` socket interface so standard tools (`ifconfig`, `ip`) work without modification.
