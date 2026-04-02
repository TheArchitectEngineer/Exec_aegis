# Audit 8: Userspace Hardening

## Priority: MEDIUM

Defense in depth. Even with a correct kernel, weak userspace makes exploitation
easier. This audit reviews what mitigations exist and what's missing.

## Files to review

| File | Focus |
|------|-------|
| `user/pwn/pwn.c` | Existing exploit test binary — is it comprehensive? |
| `user/login/` | Authentication, /etc/shadow access, password handling |
| `user/bastion/main.c` | Graphical login — same auth code path? |
| `user/vigil/main.c` | Service supervisor — privilege escalation vector? |
| `user/capd/main.c` | Capability broker — policy enforcement |
| `user/installer/main.c` | Disk access — runs with DISK_ADMIN |
| `user/shell/main.c` | Custom shell — input parsing, command injection |
| `user/lumen/` | Compositor — framebuffer access, PTY handling |

## Checklist

### Binary hardening
- [ ] Stack canaries enabled (-fstack-protector-strong)?
- [ ] ASLR status (currently absent — is this documented?)
- [ ] W^X enforced for all user pages (NX bit set correctly)?
- [ ] No executable stack segments in ELF headers
- [ ] PIE (position-independent executables) for dynamic binaries?

### Authentication
- [ ] /etc/shadow not world-readable (capability-gated?)
- [ ] Password comparison is constant-time (no timing oracle)
- [ ] Failed login rate limiting or lockout?
- [ ] Login clears sensitive memory after use (password buffers)

### Input validation in userspace
- [ ] Shell doesn't have command injection via metacharacters
- [ ] Installer validates disk paths before writing
- [ ] DHCP client validates server responses
- [ ] httpd validates HTTP request parsing

### Privilege minimization
- [ ] Services drop unnecessary capabilities after startup?
- [ ] capd policy files are restrictive (principle of least privilege)?
- [ ] Vigil's exec_caps are minimal per service?

## Output format

Same as Audit 1.
