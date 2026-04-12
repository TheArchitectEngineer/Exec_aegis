# VFS Poll Generalization + Rune Text Editor Port

**Date:** 2026-04-11
**Phase:** 47-prereq (infrastructure for GUI installer and beyond)
**Status:** Design approved

---

## Problem

Aegis's `sys_poll` and `epoll_wait` only work on socket file descriptors. Pipe, PTY, and console fds are invisible to both syscalls. This blocks any userspace program that uses `poll()`/`epoll()` on stdin or pipes — which includes every program built with crossterm/mio (Rust TUI ecosystem), and will block TinySSH (Phase 50) which must multiplex sockets + PTYs.

**Immediate goal:** Port the [rune](../../../rune/) text editor (Rust, crossterm+ratatui, ~6700 LOC) to run on Aegis. Rune already builds for `x86_64-unknown-linux-musl` and has zero native C dependencies. The only blocker is poll/epoll on TTY fds.

---

## Design

### 1. VFS poll callback

Add a `.poll` field to `vfs_ops_t` in `kernel/fs/vfs.h`:

```c
/* poll -- report readiness events for this fd.
 * Returns a bitmask of POLLIN/POLLOUT/POLLHUP/POLLERR.
 * NULL = fd does not support polling (permissive default: POLLIN|POLLOUT). */
uint16_t (*poll)(void *priv);
```

Position: after `.stat` in the struct. All existing `vfs_ops_t` tables gain `.poll = (void *)0` (no behavioral change for callers that don't use poll).

**Implementations:**

| fd type | POLLIN | POLLOUT | POLLHUP | POLLERR |
|---------|--------|---------|---------|---------|
| Pipe read end | `count > 0` OR `write_refs == 0` | never | `write_refs == 0` | never |
| Pipe write end | never | `count < PIPE_BUF_SIZE` OR `read_refs == 0` | never | `read_refs == 0` |
| PTY master | `output_buf` has data OR `!slave_open` | always | `!slave_open` | never |
| PTY slave | `input_buf` has data OR `!master_open` | always | `!master_open` | never |
| Console/kbd | `kbd_has_data()` (new, non-destructive `s_head != s_tail` check) | always | never | never |

### 2. sys_poll generalization

Current `sys_poll` (in `kernel/syscall/sys_socket.c`) checks `sock_id_from_fd()` and only handles sockets. New logic for each pollfd:

1. Try `sock_id_from_fd()` — if socket, use existing socket poll logic (unchanged)
2. Otherwise, look up `proc->fd_table->fds[fd]`:
   - If fd valid and `ops->poll != NULL`: call `ops->poll(priv)`, mask against requested events
   - If fd valid and `ops->poll == NULL`: return `POLLIN|POLLOUT` (permissive default, matches Linux for unsupported fds)
   - If fd invalid (`ops == NULL`): set `revents = POLLNVAL`

New define: `#define POLLNVAL 0x0020` (Linux value).

Blocking/wake mechanism unchanged: `g_poll_waiter` + `sched_block()` + PIT tick wake.

### 3. epoll_wait generalization

`epoll_wait_impl` currently relies on `epoll_notify()` from socket code paths (TCP/UDP ISR context). We add an active-poll fallback for VFS fds.

In the `epoll_wait` blocking loop, before checking `ep->nready` and sleeping:

1. For each watch entry, look up the fd in the current process's fd table
2. If the fd has a VFS `.poll` callback, call it
3. If the result matches watched events, add to the ready list (same format as `epoll_notify` would produce)

Socket-driven `epoll_notify` continues working as-is. The VFS poll check runs every wake cycle.

**PIT wake for epoll:** The existing `g_poll_waiter` mechanism wakes `sys_poll` blockers. We add the same for epoll: the PIT handler also wakes `ep->waiter_task` if set. Implementation: a simple `g_epoll_poll_active` flag checked in the PIT ISR, which iterates active epoll instances and wakes their waiters. Alternatively, epoll_wait sets `g_poll_waiter` as a secondary waiter (simpler, reuses existing path).

**Chosen approach:** `epoll_wait_impl` sets `g_poll_waiter` before blocking, same as `sys_poll`. The PIT handler already wakes `g_poll_waiter` — no PIT changes needed. After wake, `epoll_wait` runs its VFS poll sweep before re-checking the socket-driven ready list.

### 4. Kernel boot self-test

A `poll_test()` function called from `kernel_main` after pipe/PTY/VFS init:

1. Create a pipe via `pipe_alloc()` (kernel-internal API)
2. Call pipe read end's `.poll()` — expect 0 (no POLLIN, pipe empty)
3. Write data to pipe
4. Call pipe read end's `.poll()` — expect POLLIN
5. Close write end
6. Call pipe read end's `.poll()` — expect POLLIN|POLLHUP

Output: `[POLL] OK: vfs poll pipe` on success, `[POLL] FAIL: <reason>` on failure.

Added to `tests/expected/boot.txt`.

### 5. Userspace test binary

`user/bin/polltest/main.c` — musl-linked static binary. Tests:

1. **Pipe poll:** `pipe()` + `poll(read_fd, POLLIN, timeout=0)` — expect 0 ready. Write data. Poll again — expect POLLIN. Close write end. Poll — expect POLLIN|POLLHUP.
2. **Epoll on pipe:** `epoll_create1()` + `epoll_ctl(ADD, read_fd, EPOLLIN)` + `epoll_wait(timeout=0)` after write — expect 1 event with EPOLLIN.
3. **TTY poll:** `poll(STDIN_FILENO, POLLIN, timeout=0)` — expect 0 ready (no input queued). Validates TTY poll path doesn't crash or return POLLNVAL.
4. **POLLNVAL:** `poll(fd=99, POLLIN, timeout=0)` — expect POLLNVAL in revents.

Prints `[POLLTEST] PASS: <name>` / `[POLLTEST] FAIL: <name>` for each sub-test. Exits 0 if all pass, 1 if any fail.

### 6. Rust integration test

`tests/tests/poll_test.rs` — uses `AegisHarness` with `aegis_pc()` preset:

1. Boot QEMU
2. Assert boot oracle passes (includes `[POLL] OK: vfs poll pipe`)
3. Run `/bin/polltest` via shell
4. Assert output contains all `[POLLTEST] PASS:` lines
5. Assert exit code 0

### 7. Rune port

Once poll/epoll works on TTY fds:

1. Build: `cargo build --release --target x86_64-unknown-linux-musl` in `../rune/`
2. Copy `target/x86_64-unknown-linux-musl/release/rune` to rootfs
3. Add to `rootfs.manifest`: `rune /bin/rune 755`
4. No caps.d entry needed — rune only needs baseline caps (VFS_OPEN, VFS_READ, VFS_WRITE)
5. No source changes expected — standard musl static binary using Linux epoll/termios ABI

If runtime issues arise, patch rune with `#[cfg(target_os = "aegis")]` guards.

---

## Files modified

| File | Change |
|------|--------|
| `kernel/fs/vfs.h` | Add `.poll` to `vfs_ops_t` |
| `kernel/fs/pipe.c` | Implement `.poll` for pipe read/write ops |
| `kernel/fs/pipe.h` | (no change — poll is in vfs_ops_t) |
| `kernel/tty/pty.c` | Implement `.poll` for PTY master and slave ops |
| `kernel/tty/tty.h` | (no change) |
| `kernel/fs/console.c` | Implement `.poll` for console write (POLLOUT always) |
| `kernel/fs/kbd_vfs.c` | Implement `.poll` for kbd read (`kbd_has_data()`) |
| `kernel/arch/x86_64/kbd.c` | Add `kbd_has_data()` — non-destructive ring buffer check |
| `kernel/arch/x86_64/kbd.h` | Declare `kbd_has_data()` |
| `kernel/syscall/sys_socket.c` | Generalize `sys_poll` to check VFS `.poll`, add POLLNVAL |
| `kernel/net/epoll.c` | Add VFS poll sweep in `epoll_wait_impl` loop |
| `kernel/core/kernel_main.c` (or test location) | Add `poll_test()` boot self-test |
| `tests/expected/boot.txt` | Add `[POLL] OK: vfs poll pipe` |
| `user/bin/polltest/main.c` | New userspace test binary |
| `user/bin/polltest/Makefile` | Build rules for polltest |
| `rootfs.manifest` | Add polltest and rune |
| `tests/tests/poll_test.rs` | New Rust integration test |

## Files NOT modified

- `kernel/net/epoll.h` — no struct changes needed; `epoll_wait_impl` signature unchanged
- `kernel/tty/tty.c` — poll callbacks are on the PTY/console ops, not the tty line discipline
- Rune source code — expected zero changes

---

## Non-goals

- Event-driven epoll notifications for VFS fds (future optimization if needed)
- `select()` implementation (stays ENOSYS — poll/epoll cover all use cases)
- VMIN/VTIME interaction with poll (VMIN=0 already works for non-blocking reads; poll reports data availability independently)
- Rune headless test (interactive TUI — not testable without a real terminal)
