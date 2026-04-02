# Audit 6: Capability Model Enforcement

## Priority: HIGH

The defining security feature of Aegis. The Rust cap code is small (110 LOC),
but enforcement is scattered across every syscall. A single missing check = bypass.

## Files to review

| File | Focus |
|------|-------|
| `kernel/cap/src/lib.rs` | Core cap_grant/cap_check/cap_revoke logic |
| `kernel/syscall/*.c` (all) | Every cap_check call site — are any missing? |
| `kernel/proc/proc.c` | Capability grants at process creation |
| `user/vigil/main.c` | exec_caps — what capabilities does vigil grant? |
| `user/capd/main.c` | Runtime cap delegation — policy enforcement |
| `kernel/fs/vfs.c` | VFS_OPEN, VFS_READ, VFS_WRITE checks |

## Checklist

### Completeness
- [ ] Every syscall that accesses a resource has a cap_check
- [ ] sys_open checks VFS_OPEN
- [ ] sys_read checks VFS_READ
- [ ] sys_write checks VFS_WRITE
- [ ] sys_mmap has no cap bypass for executable pages
- [ ] sys_fb_map checks CAP_KIND_FB
- [ ] sys_blkdev_io checks DISK_ADMIN
- [ ] sys_cap_grant checks that granter holds the cap being granted
- [ ] sys_spawn cap_mask correctly restricts child capabilities

### Baseline grants (too broad?)
- [ ] What does every exec'd binary get? List all baseline caps.
- [ ] AUTH capability in baseline — can every binary read /etc/shadow?
- [ ] DISK_ADMIN in baseline — can every binary write raw disk?
- [ ] THREAD_CREATE in baseline — should sandboxed processes get this?

### Forgery resistance
- [ ] Capability tokens cannot be guessed or manufactured from userspace
- [ ] No integer overflow in cap table indexing
- [ ] Cap table bounds checked (CAP_TABLE_SIZE = 16)

### Delegation chain
- [ ] capd policy files correctly restrict what gets granted
- [ ] No circular delegation (A grants to B who grants back to A)
- [ ] Cap revocation absence documented — restart is the only reset

## Output format

Same as Audit 1.
