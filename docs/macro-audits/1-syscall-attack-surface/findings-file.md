# Audit: sys_file.c — File Operation Syscalls

**Date:** 2026-04-01 | **Auditor:** Agent B (Opus)

## HIGH

### H1 — sys_chdir kernel panic via unmapped page read
**File:** sys_file.c:286-293
**Description:** Validates only first byte, then copies up to 255 bytes without per-byte user_ptr_valid. Pointer near end of mapped page → kernel #PF (no extable recovery).
**Fix:** Add user_ptr_valid per byte in the loop, matching sys_open pattern.

### H2 — stat_copy_path silently truncates without ENAMETOOLONG
**File:** sys_file.c:162-176
**Description:** 256+ byte paths silently truncated to 255. Stat targets wrong (prefix) path.
**Fix:** Return -ENAMETOOLONG if loop exits without null byte.

### H3 — copy_path_from_user silently truncates (same bug)
**File:** sys_file.c:778-792
**Description:** Used by mkdir, unlink, rename, symlink, readlink, chmod, chown, lchown. All silently operate on truncated paths.
**Fix:** Same — return -ENAMETOOLONG on truncation.

### H4 — sys_chmod/chown lack file ownership check
**File:** sys_file.c:1089-1104, 1134-1149
**Description:** chmod checks CAP_KIND_VFS_WRITE (held by all processes). Does not check file ownership. Any process can chmod any ext2 file.
**Fix:** Check proc->uid matches file i_uid before allowing chmod/chown.

## MEDIUM

### M1 — sys_openat ignores dirfd (latent sandbox bypass)
**File:** sys_file.c:153-158
**Fix:** Reject non-AT_FDCWD dirfd with relative paths.

### M2 — sys_access doesn't resolve relative paths against cwd
### M3 — sys_nanosleep negative tv_sec causes infinite sleep
### M4 — mkdir/unlink/rename skip DAC when ext2_lookup_parent fails
### M5 — /etc/shadow gate is string-only, symlink bypass depends on vfs_open inode check
### M6 — TIOCSPGRP allows fg_pgrp hijack without session check

## LOW

### L1 — sys_lseek SEEK_END treats empty files as non-seekable (uses size==0 check)
### L2 — sys_sync has no capability check (DoS via I/O storm)
### L3 — sys_fcntl F_DUPFD minor ABI difference vs Linux
