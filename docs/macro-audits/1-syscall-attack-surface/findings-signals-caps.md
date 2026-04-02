# Audit: Signals, Capabilities, Futex, Dispatch

**Date:** 2026-04-01 | **Auditor:** Agent D (Opus)

## CRITICAL

### C1 — sys_write passes raw user pointer to VFS write ops (TOCTOU)
**File:** sys_io.c:50-54
**Description:** Validates user buffer then passes raw pointer. CLONE_VM thread can munmap between validation and write op's dereference (STAC window). sys_writev correctly uses staging buffer. sys_write does not.
**Fix:** Copy to kernel staging buffer before calling write op, matching sys_writev pattern.

### C2 — SYSRET signal delivery does not save RAX (syscall return value lost)
**File:** signal.c:370-375
**Description:** C5 callee-saved registers are FIXED. But gregs[REG_RAX] is never set in SYSRET path — left as 0 from memset. After sigreturn, syscall return value is always 0. open() returning fd=3 becomes fd=0 after signal handler.
**Fix:** Pass rax to signal_deliver_sysret and save in gregs[REG_RAX].

## HIGH

### H1 — Futex address not alignment-checked
**File:** futex.c:43-44
**Description:** Misaligned futex address can trigger split-lock #AC exception (kernel panic on real hardware).
**Fix:** `if (addr & 0x3) return -EINVAL;`

### H2 — Execve baseline grants excessive capabilities
**File:** proc.c:374-481, sys_process.c:949-956
**Description:** Every exec'd binary gets FB, NET_SOCKET, THREAD_CREATE, PROC_READ+WRITE (= can kill any process). proc_spawn gives init ALL 15 caps including DISK_ADMIN.
**Fix:** Reduce baseline to VFS_OPEN+VFS_READ+VFS_WRITE. Everything else via capd.

### H3 — signal_deliver_sysret doesn't switch CR3 for copy_to_user
**File:** signal.c:391
**Description:** iretq path explicitly switches to user PML4 for copy_to_user. SYSRET path does not. Relies on implicit invariant that user PML4 remains loaded during syscall.
**Fix:** Add explicit CR3 switch or assertion.

## MEDIUM

### M1 — signal_send_pid/pgrp traverse task list without lock from ISR (SMP race)
### M2 — sys_kill: any process can signal any other (PROC_READ+WRITE too broad)
### M3 — sys_setfg allows fg_pgrp hijack without session leader check
### M4 — sigactions[0] (signal 0) reachable via __builtin_ctzll

## LOW

### L1 — Futex WAIT lost-wakeup between value read and pool registration (SMP)
### L2 — sys_rt_sigaction incorrectly blocks catching SIGCONT
### L3 — cap_grant_exec accepts undefined kind values 17-63

## Positive Findings

- C5 (callee-saved registers) is **FIXED**
- CAP_KIND_POWER correctly excluded from baseline
- sigreturn validates canonical RIP and sanitizes RFLAGS
- Signal handler addresses validated as user-space
- cap_check bounds-checks kind via Rust clamping
- Dispatch table returns -ENOSYS for unknown syscall numbers
- Futex pool exhaustion returns -ENOMEM, not panic
