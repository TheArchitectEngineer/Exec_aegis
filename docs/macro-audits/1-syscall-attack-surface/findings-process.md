# Audit: sys_process.c — Process Lifecycle Syscalls

**Date:** 2026-04-01 | **Auditor:** Agent A (Opus)

## CRITICAL

### C1 — execve returns error after point-of-no-return
**File:** sys_process.c:1067-1072
**Bug class:** use-after-free / control-flow hijack
**Description:** After PNR at line 925 (vmm_free_user_pages destroys old address space), failures in stack building use `goto done` which returns errno via SYSRET — to unmapped memory. Triggerable with 64 argv entries of 255 bytes overflowing the 16KB user stack.
**Impact:** Kernel returns to destroyed address space. Panic (DoS) or potential code execution if old VAs are remapped by a racing CLONE_VM thread.
**Fix:** All `goto done` after PNR must become `sched_exit(); __builtin_unreachable();`.

### C2 — execve does not reset signal handlers or mask
**File:** sys_process.c:928-973
**Bug class:** privilege escalation
**Description:** POSIX requires sigaction reset to SIG_DFL and clearing caught-signal pending bits on exec. Current code resets brk, mmap_base, fs_base, VMAs, capabilities — but never touches signal_mask or sigactions[]. A stale handler address from the old binary persists into the new binary.
**Impact:** Attacker installs handler at address X, execs a privileged binary that maps code at X, triggers signal → jumps to X with new binary's caps.
**Fix:** After capability reset (line 973): `proc->pending_signals = 0; proc->signal_mask = 0; memset(proc->sigactions, 0, sizeof(proc->sigactions));`

## HIGH

### H1 — sys_spawn skips execute permission check
**File:** sys_process.c:1580-1601
**Description:** sys_execve checks ext2_check_perm for X_OK. sys_spawn opens via vfs_open without any permission check.
**Fix:** Add ext2_check_perm in sys_spawn ext2 path.

### H2 — sys_set_tid_address accepts kernel addresses
**File:** sys_process.c:171-178
**Description:** Stored in clear_child_tid, later written via vmm_write_user_bytes on exit + futex_wake_addr. No validation.
**Fix:** Validate with user_ptr_valid before storing.

### H3 — CLONE_PARENT_SETTID/CHILD_SETTID write to unchecked pointers
**File:** sys_process.c:432-445
**Description:** ptid and ctid passed to vmm_write_user_bytes without user_ptr_valid.
**Fix:** Validate both pointers before use.

### H4 — CLONE_SETTLS accepts kernel addresses (FS.base info leak)
**File:** sys_process.c:319-320
**Description:** sys_arch_prctl rejects kernel addrs for ARCH_SET_FS but clone CLONE_SETTLS does not. Child gets FS.base in kernel space → user-mode FS-prefixed reads leak kernel memory.
**Fix:** `if ((cl & CLONE_SETTLS) && tls >= 0xFFFF800000000000ULL) return -EFAULT;`

### H5 — sys_cap_grant_runtime rights escalation
**File:** sys_process.c:1448
**Description:** cap_check with rights=0 matches any slot regardless of rights. Caller with read-only can grant read-write.
**Fix:** `cap_check(caller->caps, CAP_TABLE_SIZE, kind, rights)` — match the granted rights.

### H6 — sys_cap_grant_exec does not verify caller holds the cap
**File:** sys_process.c:1407-1421
**Description:** Only checks CAP_KIND_CAP_GRANT, not that caller holds the kind being registered. Capability forgery.
**Fix:** Add cap_check for the kind being granted.

## MEDIUM

### M1 — exit_group modifies task list without sched_lock (SMP race)
### M2 — waitpid zombie reap without sched_lock (SMP double-free)
### M3 — fork s_fork_count race (SMP process limit bypass)
### M4 — argv copy byte-by-byte TOCTOU (CLONE_VM thread can unmap)
### M5 — sys_spawn kstack leak on fd_table_alloc failure
### M6 — execve O_CLOEXEC close ordering after PNR

## LOW

### L1 — Missing is_user guards on getppid, setsid, setuid, etc.
### L2 — sys_reboot inline asm port I/O violates arch isolation
### L3 — sys_spawn sp_va underflow not checked
### L4 — exit_group zombie not unlinked from run queue
### L5 — sys_cap_grant_exec accepts undefined kind values 17-63
