# Structural Audit — Codebase Layout Review

**Date:** 2026-04-01
**Scope:** All kernel/ and user/ source files (excluding arm64 binary blobs)
**Goal:** Identify structural issues that impede auditing before deep audits begin.

---

## Codebase Summary

```
Kernel (x86_64):  ~32,200 LOC across ~90 source files
User programs:    ~21,400 LOC across ~50 programs
Rust (cap/):           110 LOC (1 file)
```

---

## Finding 1: sys_process.c is a 2059-line junk drawer

**Severity: HIGH — blocks clean auditing of the most security-critical code**

Contains **25 syscalls** spanning 6 unrelated concerns:

| Concern | Syscalls | ~LOC |
|---------|----------|------|
| Process lifecycle | exit, exit_group, fork, clone, waitpid | ~600 |
| Exec | execve, spawn | ~700 |
| Identity/info | getpid, gettid, getppid, setuid, setgid, umask, uname, getrlimit, arch_prctl, set_tid_address, set_robust_list | ~200 |
| Session/group | setsid, setpgid, getpgrp, getpgid | ~100 |
| Capabilities | cap_grant_exec, cap_grant_runtime, cap_query | ~100 |
| System | reboot | ~50 |

`sys_spawn` alone is ~500 lines — it deserves its own file.

**Recommendation:** Split into 4 files:

| New file | Contents | ~LOC |
|----------|----------|------|
| `sys_process.c` | exit, exit_group, fork, clone, waitpid | ~600 |
| `sys_exec.c` | execve, spawn | ~700 |
| `sys_identity.c` | getpid/tid/ppid, setuid/gid, umask, uname, getrlimit, arch_prctl, set_tid_address, set_robust_list, setsid, setpgid, getpgrp, getpgid, reboot | ~350 |
| `sys_cap.c` | cap_grant_exec, cap_grant_runtime, cap_query | ~100 |

**Blast radius:** Makefile USERSPACE_SRCS list. No external API changes — these are static functions dispatched from syscall.c by number.

---

## Finding 2: sys_file.c is a 1194-line grab-bag with misplaced syscalls

**Severity: HIGH**

Contains **33 syscalls** mixing file ops, directory ops, permissions, time, and identity:

| Concern | Syscalls | Belongs in |
|---------|----------|-----------|
| File I/O | open, openat, lseek | sys_file.c (keep) |
| Directory | getdents64, getcwd, chdir, mkdir, unlink, rename | sys_file.c (keep) |
| Stat | stat, fstat, lstat, access | sys_file.c (keep) |
| Pipe/dup | pipe2, dup, dup2 | sys_file.c (keep) |
| ioctl/fcntl | ioctl, fcntl | sys_file.c (keep) |
| Symlinks + perms | symlink, readlink, chmod, fchmod, chown, fchown, lchown | sys_file.c (keep) |
| **Identity** | **getuid, geteuid, getgid, getegid** | **sys_identity.c** |
| **Time** | **nanosleep, clock_gettime, clock_settime** | **sys_time.c** |
| **FS-wide** | **sync** | **sys_file.c** (fine here) |

**Recommendation:** Extract identity (4 trivial functions → sys_identity.c) and time (3 functions → sys_time.c). The remaining ~1050 LOC is still large but coherent — all file/directory operations.

---

## Finding 3: sys_socket.c contains orphaned memfd/ftruncate

**Severity: MEDIUM**

`sys_memfd_create` (line 1008) and `sys_ftruncate` (line 1041) are memory/fd operations with no relation to sockets. They ended up here because they were added alongside IPC work.

**Recommendation:** Move to `sys_memory.c` (currently 438 LOC — total would be ~500, still reasonable).

---

## Finding 4: vfs.c has 93 ext2 references — ext2 adapter code is embedded

**Severity: MEDIUM — complicates filesystem auditing**

`vfs.c` (590 LOC) contains:
- `ext2_fd_priv_t` struct definition
- `s_ext2_pool[32]` static pool
- `ext2_pool_alloc()` / `ext2_pool_free()`
- `ext2_vfs_read_fn()` / `ext2_vfs_write_fn()` / `ext2_vfs_close_fn()`
- `ext2_vfs_dup_fn()` / `ext2_vfs_readdir_fn()` / `ext2_vfs_stat_fn()`

The ext2 family already has 3 files (ext2.c, ext2_cache.c, ext2_dir.c). The VFS adapter code naturally belongs in `ext2_vfs.c`.

**Recommendation:** Extract ~150 LOC of ext2-specific code from vfs.c into `kernel/fs/ext2_vfs.c`. VFS becomes a clean dispatcher (~440 LOC). The ext2 subsystem becomes 4 files with clear responsibilities:

| File | Responsibility |
|------|---------------|
| ext2.c | Core: mount, read, write, alloc, create, unlink, mkdir, rename |
| ext2_dir.c | Directory entry manipulation |
| ext2_cache.c | Block cache (16-slot LRU) |
| ext2_vfs.c (new) | VFS adapter: fd pool, read/write/stat/close callbacks |

---

## Finding 5: fb.c mixes framebuffer driver with panic screen and boot splash

**Severity: LOW-MEDIUM**

`fb.c` (915 LOC) contains three distinct subsystems:
- **Framebuffer text driver** (~400 LOC): init, putchar, scroll, ANSI CSI parsing
- **Panic bluescreen** (~250 LOC): logo rendering, register dump, backtrace display
- **Boot splash** (~50 LOC): centered logo blit

Plus it `#include`s `logo_panic.h` (137KB) and `logo_boot.h` (538KB).

**Recommendation:** Extract panic rendering into `kernel/drivers/panic_screen.c`. The boot splash can stay (tiny). This makes `fb.c` a clean ~600 LOC text-mode driver.

---

## Finding 6: terminus20.h is duplicated across 3 locations

**Severity: LOW**

| Path | Size | MD5 |
|------|------|-----|
| kernel/drivers/terminus20.h | 519 lines | 8adc52e6... |
| user/glyph/terminus20.h | 519 lines | 8adc52e6... (identical) |
| user/fb_test/terminus20.h | 521 lines | 3ee86673... (slightly different) |

Kernel and glyph copies are byte-identical.

**Recommendation:** Move to a shared location (e.g., `include/terminus20.h` or `assets/terminus20.h`) and update includes. The fb_test copy with 2 extra lines should be investigated — likely a stale version.

---

## Finding 7: Logo data headers are massive blobs masquerading as headers

**Severity: LOW (cosmetic, but 675KB of binary data in source tree)**

| File | Lines | Bytes on disk |
|------|-------|--------------|
| kernel/drivers/logo_boot.h | 117 | 538KB |
| kernel/drivers/logo_panic.h | 1282 | 137KB |

These are RGBA pixel arrays. They work fine as `#include`d headers but bloat the source tree. The boot logo alone is 538KB of hex-encoded pixel data.

**Recommendation:** Keep as-is for now. Alternative: convert to binary assets and embed via objcopy (like user ELFs). Low priority — they work, they just look ugly.

---

## Finding 8: sys_impl.h is a kitchen-sink include

**Severity: LOW (build friction, not code quality)**

Every syscall .c file includes `sys_impl.h`, which pulls in 20+ headers:
```c
#include "sched.h"
#include "proc.h"
#include "signal.h"
#include "kbd.h"
#include "vfs.h"
#include "pipe.h"
#include "initrd.h"
#include "vmm.h"
#include "pmm.h"
... (20 more)
```

Touching any of these 20 headers recompiles all 10 syscall files.

**Recommendation:** Not urgent. The real fix is `-MMD` header dependency tracking in the Makefile (tracked in audit 10). Once that's in, the kitchen-sink include just adds ~100ms to incremental builds. Can be split later if it becomes annoying.

---

## Recommended Restructuring Order

Execute these in order. Each is independent and testable with `make test`.

### Step 1: Split sys_process.c → sys_exec.c + sys_identity.c + sys_cap.c

**Why first:** sys_process.c is the #1 audit target (2059 LOC, fork+exec+clone) and the current structure makes it impossible to audit exec separately from fork.

**Files touched:** sys_process.c (split), new sys_exec.c, new sys_identity.c, new sys_cap.c, Makefile (USERSPACE_SRCS), sys_impl.h (maybe, for forward declarations).

**Verification:** `make test` — no API changes, just moving code between files.

### Step 2: Extract time + identity syscalls from sys_file.c

**Why second:** Misplaced syscalls confuse auditors ("why is getuid in a file called sys_file?").

**Files touched:** sys_file.c (remove ~100 LOC), sys_identity.c (add identity fns), new sys_time.c (nanosleep, clock_gettime, clock_settime), Makefile.

### Step 3: Move memfd/ftruncate from sys_socket.c to sys_memory.c

**Why third:** Quick win, ~80 LOC move.

**Files touched:** sys_socket.c, sys_memory.c, Makefile (no change — same TU list).

### Step 4: Extract ext2 VFS adapter from vfs.c

**Why fourth:** Cleans up the VFS/ext2 boundary before filesystem audit.

**Files touched:** vfs.c (remove ~150 LOC), new ext2_vfs.c, Makefile (FS_SRCS).

### Step 5: Extract panic_screen.c from fb.c

**Why fifth:** Separates the display driver from the panic handler.

**Files touched:** fb.c (remove ~250 LOC), new panic_screen.c, Makefile (DRIVER_SRCS), fb.h (expose render helpers).

### Step 6: Deduplicate terminus20.h

**Why last:** Lowest impact, requires build system changes.

---

## What NOT to restructure

These files are large but cohesive — splitting would hurt more than help:

| File | LOC | Why keep together |
|------|-----|-------------------|
| ext2.c | 1177 | Already split into 3 files. Core operations are tightly coupled. |
| xhci.c | 996 | One device, one state machine. Split would create artificial seams. |
| vmm.c | 942 | Window allocator + page table ops are tightly coupled. |
| procfs.c | 807 | All gen_* functions share the same allocator pattern. |
| unix_socket.c | 673 | Single protocol implementation. |
| tcp.c | 642 | Single protocol, single state machine. |
| sched.c | 572 | Single run queue, all operations interdependent. |
| proc.c | 575 | Process creation + init. Tightly coupled with ELF loader. |
| acpi.c | 560 | ACPI table parsing. One subsystem. |

---

---

## Part 2: Directory Structure Issues

---

## Finding 9: kernel/fs/ is a 31-file dumping ground with 8 unrelated subsystems

**Severity: HIGH — this is the biggest structural problem**

```
kernel/fs/   (16 .c, 15 .h)
├── VFS layer:      vfs.c/h, fd_table.c/h
├── ext2:           ext2.c/h, ext2_cache.c, ext2_dir.c, ext2_internal.h
├── initrd:         initrd.c/h
├── ramfs:          ramfs.c/h
├── procfs:         procfs.c/h
├── TTY/PTY:        tty.c/h, pty.c/h                    ← NOT a filesystem
├── Pipes:          pipe.c/h                             ← IPC mechanism
├── Block devices:  blkdev.c/h, gpt.c/h                 ← storage layer
├── Devices:        console.c/h, kbd_vfs.c/h, memfd.c/h ← device files
```

TTY/PTY is the worst offender: it's a **terminal line discipline subsystem** — 934 LOC of line editing, session management, signal delivery, SIGTTIN/SIGTTOU. It happens to expose VFS read/write callbacks, but so does `/dev/null`. On Linux, TTY is `drivers/tty/` — its own top-level subsystem.

**Recommendation:** Move TTY/PTY to `kernel/tty/`. It's consumed by:
- `kernel/arch/x86_64/kbd.c` — keyboard ISR delivers to tty
- `kernel/syscall/sys_process.c` — session leader exit sends SIGHUP via tty
- `kernel/syscall/sys_file.c` — ioctl TIOCGPTN etc.

Everything else can stay in `kernel/fs/` — they all implement VFS interfaces and the directory isn't too large without tty/pty (27 files → 23).

---

## Finding 10: 75+ relative `../` includes despite flat -I path

**Severity: MEDIUM — inconsistency creates confusion and fragile paths**

The Makefile sets up 12 `-I` include paths:
```
-Ikernel/arch/x86_64  -Ikernel/core   -Ikernel/cap
-Ikernel/mm            -Ikernel/sched  -Ikernel/proc
-Ikernel/syscall       -Ikernel/elf    -Ikernel/fs
-Ikernel/signal        -Ikernel/drivers -Ikernel/net
```

This means `#include "proc.h"` works from ANY source file. Yet 75+ includes use `../` relative paths:
```c
/* In kernel/net/socket.c: */
#include "../mm/uaccess.h"     /* works, but so does #include "uaccess.h" */
#include "../core/spinlock.h"  /* same — flat path works */

/* In kernel/core/main.c: */
#include "../proc/proc.h"      /* flat "proc.h" works */
#include "../fs/ext2.h"        /* flat "ext2.h" works */
#include "../drivers/xhci.h"   /* flat "xhci.h" works */
```

Some files use flat includes, others use relative. No pattern to which convention is used where.

**Recommendation:** Standardize on flat includes (they already work). Remove all `../` relative paths. This is a mechanical find-and-replace with zero semantic change. Makes refactoring (moving files between dirs) much easier.

---

## Finding 11: kernel/elf/ is an orphaned single-file directory

**Severity: LOW**

`kernel/elf/` contains exactly 2 files: `elf.c` (165 LOC) and `elf.h` (82 LOC). It's an ELF loader called only by `proc.c` and `sys_process.c`.

This is too small and too narrowly-scoped to justify its own directory. The ELF loader is part of process creation.

**Recommendation:** Merge into `kernel/proc/`. The proc directory becomes `proc.c`, `proc.h`, `elf.c`, `elf.h` — all process creation code in one place.

---

## Finding 12: signal.c is in kernel/signal/ but compiled as CORE_SRCS

**Severity: LOW — cosmetic but confusing**

The Makefile lists `kernel/signal/signal.c` under `CORE_SRCS`, not in its own `SIGNAL_SRCS` variable. The directory exists, the `-I` path exists, but the build system treats it as "core."

**Recommendation:** Add `SIGNAL_SRCS = kernel/signal/signal.c` and reference it in ALL_OBJS. Trivial Makefile change.

---

## Finding 13: Flat -I namespace has no encapsulation

**Severity: LOW (awareness, not action)**

Every header name must be globally unique across all 12 directories. There's no compiler-enforced boundary — any `.c` file can `#include` any `.h` from any subsystem. `proc.h` is included by 18 files across 8 directories.

This is normal for a small kernel (Linux does the same thing with its flat include hierarchy). It only becomes a problem if:
- Two headers get the same name (hasn't happened)
- You want to enforce that, say, net/ never includes fs/ (no mechanism for this)

**Recommendation:** Don't change. Document the include convention. A small kernel doesn't need include firewalls.

---

## Revised Restructuring Order

Incorporating directory changes with file-level changes:

### Phase A: Include standardization (do first, reduces noise in all later diffs)

**Step A1:** Replace all 75+ `../` relative includes with flat includes.
Mechanical sed, zero semantic change. `make test` verifies.

### Phase B: Directory moves

**Step B1:** Move `kernel/fs/tty.c`, `tty.h`, `pty.c`, `pty.h` → `kernel/tty/`.
Add `-Ikernel/tty` to CFLAGS. Add `TTY_SRCS` to Makefile. Update 4-6 includes.

**Step B2:** Move `kernel/elf/elf.c`, `elf.h` → `kernel/proc/`.
Remove `-Ikernel/elf`. Update 2-3 includes.

**Step B3:** Fix SIGNAL_SRCS in Makefile (move signal.c out of CORE_SRCS).

### Phase C: File-level splits (from Part 1)

Steps 1-6 from Part 1 findings (sys_process split, etc.).

---

## Post-restructuring target state

```
kernel/
├── arch/x86_64/        (unchanged — all x86 code)
├── cap/                (unchanged — Rust capability system)
├── core/               main.c, printk.c/h, random.c/h, spinlock.h
├── drivers/
│   ├── fb.c            framebuffer text driver (~600 LOC, trimmed)
│   ├── panic_screen.c  panic bluescreen rendering (~250 LOC)  ← NEW
│   ├── nvme.c, xhci.c, virtio_net.c, usb_hid.c, usb_mouse.c, ramdisk.c
│   └── (data: logo_boot.h, logo_panic.h, terminus20.h)
├── fs/
│   ├── vfs.c           pure VFS dispatch (~440 LOC, trimmed)
│   ├── ext2_vfs.c      ext2 VFS adapter (~150 LOC)  ← NEW
│   ├── ext2.c, ext2_cache.c, ext2_dir.c   (unchanged)
│   ├── initrd.c, ramfs.c, procfs.c        (unchanged)
│   ├── pipe.c, memfd.c, console.c, kbd_vfs.c, blkdev.c, gpt.c
│   └── fd_table.c
├── mm/                 (unchanged — pmm, vmm, kva, vma, uaccess)
├── net/                (unchanged — eth, ip, tcp, udp, socket, unix_socket, epoll)
├── proc/
│   ├── proc.c/h        (unchanged)
│   └── elf.c/h         ← MOVED from kernel/elf/
├── sched/              (unchanged — sched.c/h)
├── signal/             (unchanged — signal.c/h, now with own SIGNAL_SRCS)
├── syscall/
│   ├── syscall.c       dispatch table (201 LOC)
│   ├── sys_process.c   exit, fork, clone, waitpid (~600 LOC)
│   ├── sys_exec.c      execve, spawn (~700 LOC)  ← NEW
│   ├── sys_identity.c  getpid, setuid, setsid, umask, uname, ... (~350 LOC)  ← NEW
│   ├── sys_cap.c       cap_grant_*, cap_query (~100 LOC)  ← NEW
│   ├── sys_file.c      open, read, write, stat, dir ops (~1000 LOC, trimmed)
│   ├── sys_time.c      nanosleep, clock_* (~100 LOC)  ← NEW
│   ├── sys_memory.c    mmap, munmap, mprotect, brk, memfd, ftruncate (~520 LOC)
│   ├── sys_socket.c    socket ops only (~970 LOC, trimmed)
│   ├── sys_signal.c, sys_disk.c, sys_io.c, sys_random.c, futex.c (unchanged)
│   └── sys_impl.h, syscall.h, syscall_util.h
└── tty/                ← NEW directory
    ├── tty.c/h         terminal line discipline
    └── pty.c/h         pseudo-terminal pairs
```

### Change summary

| Change type | Count | Details |
|-------------|-------|---------|
| New files | 6 | sys_exec.c, sys_identity.c, sys_cap.c, sys_time.c, ext2_vfs.c, panic_screen.c |
| Trimmed files | 5 | sys_process.c, sys_file.c, sys_socket.c, vfs.c, fb.c |
| Moved files | 3 | elf.c/h → proc/, tty.c/h + pty.c/h → tty/ |
| Deleted dirs | 1 | kernel/elf/ |
| New dirs | 1 | kernel/tty/ |
| Makefile | 1 | USERSPACE_SRCS, FS_SRCS, DRIVER_SRCS, TTY_SRCS, SIGNAL_SRCS, -I flags |
| Include fixes | ~75 | Remove all `../` relative includes → flat names |

Zero API changes. Every step independently testable with `make test`.
