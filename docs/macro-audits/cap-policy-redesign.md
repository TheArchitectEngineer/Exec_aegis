# Capability Policy Redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace capd + exec_caps delegation chain with kernel-mediated exec-time capability grants using a two-tier system (service caps granted unconditionally, admin caps requiring an authenticated session).

**Architecture:** The kernel loads a cap policy table from `/etc/aegis/caps.d/` at boot. At execve time, the kernel looks up the exe basename in the table and grants matching caps. Admin-tier caps (DISK_ADMIN, POWER, AUTH, SETUID, CAP_DELEGATE, CAP_QUERY) are only granted if the process is in an authenticated session (a kernel flag set by login/bastion after password verification). capd is eliminated. Vigil is stripped of all cap logic — it becomes pure process supervision.

**Tech Stack:** C kernel code, musl userspace, QEMU test harness.

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `kernel/cap/cap.h` | Modify | Add CAP_TIER_SERVICE/CAP_TIER_ADMIN defines, cap_policy_t struct, cap_policy_load/cap_policy_lookup declarations |
| `kernel/cap/cap_policy.c` | Create | Policy table: load from initrd/ext2, lookup by basename at exec time |
| `kernel/syscall/sys_exec.c` | Modify | Replace exec_caps + baseline with cap_policy_lookup at exec time |
| `kernel/syscall/sys_cap.c` | Modify | Add sys_auth_session (syscall 364), remove sys_cap_grant_exec/sys_cap_grant_runtime |
| `kernel/syscall/syscall.c` | Modify | Wire syscall 364, remove 361/363 |
| `kernel/proc/proc.h` | Modify | Add `uint32_t authenticated;` field, remove `exec_caps[]` array |
| `kernel/proc/proc.c` | Modify | Remove massive init cap grants (replaced by policy) |
| `kernel/core/main.c` | Modify | Call cap_policy_load after VFS init |
| `kernel/fs/initrd.c` | Modify | Add cap policy files to initrd |
| `user/vigil/main.c` | Modify | Strip ALL cap logic (needs_*, exec_caps, cap defines) |
| `user/libauth/auth.c` | Modify | Replace capd_request with sys_auth_session call |
| `user/login/main.c` | Modify | Remove capd request, call sys_auth_session after auth |
| `user/bastion/main.c` | Modify | Remove capd request, call sys_auth_session after auth |
| `user/stsh/caps.c` | Modify | Update cap introspection (no more cap_grant_exec/runtime) |
| `user/capd/` | Delete | No longer needed |
| `user/capd_test/` | Delete | No longer needed |
| `Makefile` | Modify | Remove capd/capd_test targets, add cap_policy.c, add caps.d files to rootfs |
| `kernel/syscall/sys_impl.h` | Modify | Update declarations |
| `tests/expected/boot.txt` | Modify | Remove [CAPD] line if present |

---

## Task 1: Kernel cap policy table + loader

**Files:**
- Create: `kernel/cap/cap_policy.c`
- Modify: `kernel/cap/cap.h`
- Modify: `kernel/core/main.c`
- Modify: `Makefile`

- [ ] **Step 1: Define the policy data structures in cap.h**

Add after the existing CAP_RIGHTS defines:

```c
/* Capability tiers — admin caps require authenticated session */
#define CAP_TIER_SERVICE 0u  /* granted at exec if binary is in policy */
#define CAP_TIER_ADMIN   1u  /* granted at exec only if proc->authenticated */

/* Cap policy entry — one per binary in /etc/aegis/caps.d/ */
#define CAP_POLICY_MAX 32u
#define CAP_POLICY_NAME_MAX 64u

typedef struct {
    char     name[CAP_POLICY_NAME_MAX]; /* binary basename, e.g. "httpd" */
    uint32_t caps[CAP_TABLE_SIZE];      /* kind→rights; 0 = not granted */
    uint32_t tier[CAP_TABLE_SIZE];      /* kind→tier (SERVICE or ADMIN) */
} cap_policy_entry_t;

/* Load policy from /etc/aegis/caps.d/ — called once at boot after VFS init. */
void cap_policy_load(void);

/* Look up policy for a binary by exe_path. Returns NULL if no entry. */
const cap_policy_entry_t *cap_policy_lookup(const char *exe_path);
```

- [ ] **Step 2: Create cap_policy.c**

```c
/* cap_policy.c — kernel capability policy table.
 * Loaded from /etc/aegis/caps.d/ at boot. Each file is named after a
 * binary (e.g., "httpd") and contains lines like:
 *   service NET_SOCKET
 *   admin AUTH SETUID
 * The kernel grants service-tier caps at exec unconditionally.
 * Admin-tier caps are granted only if proc->authenticated == 1. */
#include "cap.h"
#include "vfs.h"
#include "printk.h"
#include <stdint.h>

static cap_policy_entry_t s_policies[CAP_POLICY_MAX];
static uint32_t s_policy_count = 0;

/* Map cap name string to CAP_KIND_* constant. */
static uint32_t
cap_name_to_kind(const char *name, uint32_t len)
{
    /* Compare first few chars for speed — all names are unique in prefix */
    if (len == 3 && name[0] == 'I' && name[1] == 'P' && name[2] == 'C')
        return CAP_KIND_IPC;
    if (len == 2 && name[0] == 'F' && name[1] == 'B')
        return CAP_KIND_FB;
    /* Full table for the rest: */
    static const struct { const char *s; uint32_t k; } tbl[] = {
        { "VFS_OPEN",       CAP_KIND_VFS_OPEN },
        { "VFS_WRITE",      CAP_KIND_VFS_WRITE },
        { "VFS_READ",       CAP_KIND_VFS_READ },
        { "AUTH",           CAP_KIND_AUTH },
        { "CAP_GRANT",      CAP_KIND_CAP_GRANT },
        { "SETUID",         CAP_KIND_SETUID },
        { "NET_SOCKET",     CAP_KIND_NET_SOCKET },
        { "NET_ADMIN",      CAP_KIND_NET_ADMIN },
        { "THREAD_CREATE",  CAP_KIND_THREAD_CREATE },
        { "PROC_READ",      CAP_KIND_PROC_READ },
        { "DISK_ADMIN",     CAP_KIND_DISK_ADMIN },
        { "CAP_DELEGATE",   CAP_KIND_CAP_DELEGATE },
        { "CAP_QUERY",      CAP_KIND_CAP_QUERY },
        { "POWER",          CAP_KIND_POWER },
    };
    for (uint32_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        const char *s = tbl[i].s;
        uint32_t j;
        for (j = 0; j < len && s[j] && name[j] == s[j]; j++) {}
        if (j == len && s[j] == '\0') return tbl[i].k;
    }
    return CAP_KIND_NULL;
}

/* Parse one policy file. Format:
 *   service NET_SOCKET NET_ADMIN
 *   admin AUTH SETUID
 * One tier keyword per line, followed by space-separated cap names. */
static void
parse_policy(cap_policy_entry_t *e, const char *buf, uint32_t len)
{
    uint32_t pos = 0;
    while (pos < len) {
        /* Skip whitespace/newlines */
        while (pos < len && (buf[pos] == ' ' || buf[pos] == '\n' || buf[pos] == '\r'))
            pos++;
        if (pos >= len) break;

        /* Parse tier keyword */
        uint32_t tier;
        if (buf[pos] == 's') { tier = CAP_TIER_SERVICE; }
        else if (buf[pos] == 'a') { tier = CAP_TIER_ADMIN; }
        else { /* skip unknown line */ while (pos < len && buf[pos] != '\n') pos++; continue; }

        /* Skip to first space after tier keyword */
        while (pos < len && buf[pos] != ' ' && buf[pos] != '\n') pos++;

        /* Parse cap names on this line */
        while (pos < len && buf[pos] != '\n') {
            while (pos < len && buf[pos] == ' ') pos++;
            if (pos >= len || buf[pos] == '\n') break;
            uint32_t start = pos;
            while (pos < len && buf[pos] != ' ' && buf[pos] != '\n') pos++;
            uint32_t kind = cap_name_to_kind(buf + start, pos - start);
            if (kind != CAP_KIND_NULL && kind < CAP_TABLE_SIZE) {
                e->caps[kind] = CAP_RIGHTS_READ | CAP_RIGHTS_WRITE | CAP_RIGHTS_EXEC;
                e->tier[kind] = tier;
            }
        }
    }
}

void
cap_policy_load(void)
{
    /* Scan /etc/aegis/caps.d/ via getdents-style iteration.
     * Each file is a policy for the binary matching its filename. */
    vfs_file_t dir;
    if (vfs_open("/etc/aegis/caps.d", 0, &dir) != 0) {
        printk("[CAP] no policy dir /etc/aegis/caps.d — using baseline only\n");
        return;
    }

    char name[256];
    uint8_t dtype;
    uint64_t idx = 0;
    while (dir.ops->readdir &&
           dir.ops->readdir(dir.priv, idx, name, &dtype) == 0) {
        idx++;
        if (name[0] == '.') continue;  /* skip . and .. */
        if (s_policy_count >= CAP_POLICY_MAX) break;

        /* Read the policy file */
        char path[320];
        uint32_t plen = 0;
        const char *prefix = "/etc/aegis/caps.d/";
        while (prefix[plen]) { path[plen] = prefix[plen]; plen++; }
        uint32_t nlen = 0;
        while (name[nlen] && plen + nlen < sizeof(path) - 1) {
            path[plen + nlen] = name[nlen]; nlen++;
        }
        path[plen + nlen] = '\0';

        vfs_file_t f;
        if (vfs_open(path, 0, &f) != 0) continue;
        char buf[512];
        int n = f.ops->read(f.priv, buf, 0, sizeof(buf) - 1);
        if (f.ops->close) f.ops->close(f.priv);
        if (n <= 0) continue;
        buf[n] = '\0';

        cap_policy_entry_t *e = &s_policies[s_policy_count];
        __builtin_memset(e, 0, sizeof(*e));
        /* Copy basename */
        uint32_t ni = 0;
        while (ni < CAP_POLICY_NAME_MAX - 1 && name[ni]) {
            e->name[ni] = name[ni]; ni++;
        }
        e->name[ni] = '\0';

        parse_policy(e, buf, (uint32_t)n);
        s_policy_count++;
        printk("[CAP] policy: %s\n", e->name);
    }
    if (dir.ops->close) dir.ops->close(dir.priv);
    printk("[CAP] OK: loaded %u policies\n", (unsigned)s_policy_count);
}

const cap_policy_entry_t *
cap_policy_lookup(const char *exe_path)
{
    /* Extract basename from path */
    const char *base = exe_path;
    const char *p = exe_path;
    while (*p) { if (*p == '/') base = p + 1; p++; }

    for (uint32_t i = 0; i < s_policy_count; i++) {
        const char *a = s_policies[i].name;
        const char *b = base;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return &s_policies[i];
    }
    return (const cap_policy_entry_t *)0;
}
```

- [ ] **Step 3: Add cap_policy.c to Makefile**

In the Makefile, find `CORE_SRCS` and add `kernel/cap/cap_policy.c`:

```
CORE_SRCS = \
    kernel/core/main.c \
    kernel/core/printk.c \
    kernel/core/random.c \
    kernel/cap/cap_policy.c
```

- [ ] **Step 4: Call cap_policy_load from main.c**

In `kernel/core/main.c`, after `vfs_init()` and `console_init()` but before `acpi_init()`, add:

```c
    cap_policy_load();      /* load /etc/aegis/caps.d/ cap policies         */
```

- [ ] **Step 5: Commit**

```bash
git add kernel/cap/cap.h kernel/cap/cap_policy.c kernel/core/main.c Makefile
git commit -m "feat: kernel cap policy table — load /etc/aegis/caps.d/ at boot"
```

---

## Task 2: Add authenticated session flag + sys_auth_session syscall

**Files:**
- Modify: `kernel/proc/proc.h`
- Modify: `kernel/syscall/sys_cap.c`
- Modify: `kernel/syscall/syscall.c`
- Modify: `kernel/syscall/sys_impl.h`

- [ ] **Step 1: Add authenticated field to aegis_process_t**

In `kernel/proc/proc.h`, add after `uint32_t stop_signum;`:

```c
    uint32_t      authenticated;  /* 1 = in authenticated session (set by login/bastion) */
```

- [ ] **Step 2: Implement sys_auth_session in sys_cap.c**

Add to `kernel/syscall/sys_cap.c`:

```c
/*
 * sys_auth_session — syscall 364
 *
 * Called by login/bastion after successful password verification.
 * Sets the authenticated flag on the current process. This flag is
 * inherited across fork and preserved across exec. The kernel uses
 * it at exec time to decide whether admin-tier caps are granted.
 *
 * Requires CAP_KIND_AUTH — only login/bastion hold this.
 */
uint64_t
sys_auth_session(void)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (!proc->task.is_user)
        return (uint64_t)-(int64_t)1;
    if (cap_check(proc->caps, CAP_TABLE_SIZE, CAP_KIND_AUTH, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;
    proc->authenticated = 1;
    return 0;
}
```

- [ ] **Step 3: Wire syscall 364 in syscall.c**

Add case to the dispatch switch:

```c
    case 364: return sys_auth_session();
```

- [ ] **Step 4: Declare in sys_impl.h**

Add declaration:

```c
uint64_t sys_auth_session(void);
```

- [ ] **Step 5: Ensure authenticated flag is inherited by fork/clone and preserved by exec**

In `kernel/syscall/sys_process.c` — check the fork/clone paths copy `authenticated`. In `sys_exec.c` — ensure the exec path does NOT clear `authenticated` (it should survive exec since the user already proved identity).

The fork path copies all of `aegis_process_t` via field-by-field copy. Add:

```c
child->authenticated = parent->authenticated;
```

in both `sys_fork` and `sys_clone` (near the other scalar field copies).

The exec path resets caps but should NOT reset `authenticated`. Verify no line clears it.

- [ ] **Step 6: Commit**

```bash
git add kernel/proc/proc.h kernel/syscall/sys_cap.c kernel/syscall/syscall.c kernel/syscall/sys_impl.h kernel/syscall/sys_process.c
git commit -m "feat: sys_auth_session (364) — authenticated session flag for admin caps"
```

---

## Task 3: Replace exec baseline + exec_caps with policy lookup

**Files:**
- Modify: `kernel/syscall/sys_exec.c`

- [ ] **Step 1: Replace the execve cap reset block**

Find the block at ~line 166-198 (the baseline grants + exec_caps application). Replace with:

```c
    /* Reset capability table and apply policy-based grants.
     * Baseline caps are always granted. Policy caps are looked up by
     * binary name. Admin-tier policy caps require proc->authenticated. */
    {
        uint32_t ci;
        for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
            proc->caps[ci].kind   = CAP_KIND_NULL;
            proc->caps[ci].rights = 0;
        }
        /* Baseline — every binary gets these */
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_OPEN,  CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_READ,  CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_IPC,       CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_PROC_READ, CAP_RIGHTS_READ);
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ);

        /* Policy lookup — grant caps based on binary name + auth state */
        const cap_policy_entry_t *pol = cap_policy_lookup(proc->exe_path);
        if (pol) {
            for (ci = 1; ci < CAP_TABLE_SIZE; ci++) {
                if (pol->caps[ci] == 0) continue;
                if (pol->tier[ci] == CAP_TIER_ADMIN && !proc->authenticated)
                    continue;  /* admin cap but not authenticated — skip */
                cap_grant(proc->caps, CAP_TABLE_SIZE, ci, pol->caps[ci]);
            }
        }
    }
```

Note: removed NET_SOCKET and FB from the baseline. They're now policy-only.
Note: PROC_READ now has READ only (not WRITE). WRITE (for sys_kill) is policy-only.

- [ ] **Step 2: Do the same for sys_spawn's cap block**

Find the sys_spawn cap block (~line 873-943). Replace with the same pattern:

```c
        /* Capabilities: baseline + policy lookup */
        {
            uint32_t ci;
            for (ci = 0; ci < CAP_TABLE_SIZE; ci++) {
                child->caps[ci].kind   = CAP_KIND_NULL;
                child->caps[ci].rights = 0;
            }
            cap_grant(child->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_OPEN,  CAP_RIGHTS_READ);
            cap_grant(child->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE);
            cap_grant(child->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_READ,  CAP_RIGHTS_READ);
            cap_grant(child->caps, CAP_TABLE_SIZE, CAP_KIND_IPC,       CAP_RIGHTS_READ);
            cap_grant(child->caps, CAP_TABLE_SIZE, CAP_KIND_PROC_READ, CAP_RIGHTS_READ);
            cap_grant(child->caps, CAP_TABLE_SIZE, CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ);

            const cap_policy_entry_t *pol = cap_policy_lookup(child->exe_path);
            if (pol) {
                for (ci = 1; ci < CAP_TABLE_SIZE; ci++) {
                    if (pol->caps[ci] == 0) continue;
                    if (pol->tier[ci] == CAP_TIER_ADMIN && !parent->authenticated)
                        continue;
                    cap_grant(child->caps, CAP_TABLE_SIZE, ci, pol->caps[ci]);
                }
            }

            /* cap_mask restriction still applies if provided */
            /* (existing cap_mask code stays — it restricts, not grants) */
        }
```

- [ ] **Step 3: Commit**

```bash
git add kernel/syscall/sys_exec.c
git commit -m "feat: exec-time cap grants from kernel policy table"
```

---

## Task 4: Remove exec_caps from proc.h and all users

**Files:**
- Modify: `kernel/proc/proc.h`
- Modify: `kernel/syscall/sys_cap.c`
- Modify: `kernel/syscall/sys_process.c`
- Modify: `kernel/syscall/syscall.c`
- Modify: `kernel/syscall/sys_impl.h`

- [ ] **Step 1: Remove exec_caps field from aegis_process_t**

In `kernel/proc/proc.h`, delete the line:
```c
    cap_slot_t    exec_caps[CAP_TABLE_SIZE]; /* caps to grant post-execve; zeroed after apply */
```

- [ ] **Step 2: Remove sys_cap_grant_exec and sys_cap_grant_runtime from sys_cap.c**

Delete both functions entirely. They're no longer needed — the kernel policy table replaces them.

- [ ] **Step 3: Remove syscall 361 and 363 from syscall.c dispatch**

Remove the `case 361:` and `case 363:` entries. Add comments noting they're retired:
```c
    /* 361: sys_cap_grant_exec — RETIRED (replaced by kernel cap policy) */
    /* 363: sys_cap_grant_runtime — RETIRED (replaced by kernel cap policy) */
```

- [ ] **Step 4: Remove declarations from sys_impl.h**

Remove `sys_cap_grant_exec` and `sys_cap_grant_runtime` declarations.

- [ ] **Step 5: Remove all exec_caps references from sys_process.c**

Search for `exec_caps` in sys_process.c (fork, clone paths that copy it). Remove all:
```c
child->exec_caps[ci] = ...
```

- [ ] **Step 6: Commit**

```bash
git add kernel/proc/proc.h kernel/syscall/sys_cap.c kernel/syscall/syscall.c kernel/syscall/sys_impl.h kernel/syscall/sys_process.c
git commit -m "refactor: remove exec_caps — replaced by kernel cap policy"
```

---

## Task 5: Simplify proc_spawn_init (init only needs minimal caps)

**Files:**
- Modify: `kernel/proc/proc.c`

- [ ] **Step 1: Replace the init cap grants**

Init (vigil) no longer needs ALL caps. It needs only what IT uses:
- VFS_OPEN, VFS_READ, VFS_WRITE (file access for service configs)
- CAP_GRANT (to... actually, exec_caps are gone, so vigil doesn't need this either)
- PROC_READ (to monitor child processes — waitpid)

Actually, with exec_caps eliminated and policy-based grants, vigil needs almost nothing beyond the baseline. The only extra cap vigil needs is PROC_READ with WRITE (for sending signals to services via kill).

Replace the init_caps table with:

```c
    /* Init gets baseline + PROC_READ with WRITE (to manage services via kill).
     * All other caps come from the policy table at exec time. */
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_OPEN,  CAP_RIGHTS_READ);
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_WRITE, CAP_RIGHTS_WRITE);
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_VFS_READ,  CAP_RIGHTS_READ);
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_IPC,       CAP_RIGHTS_READ);
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_PROC_READ,
              CAP_RIGHTS_READ | CAP_RIGHTS_WRITE);
    cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_THREAD_CREATE, CAP_RIGHTS_READ);
```

- [ ] **Step 2: Commit**

```bash
git add kernel/proc/proc.c
git commit -m "refactor: init gets minimal caps — policy table grants the rest"
```

---

## Task 6: Strip vigil of all cap logic

**Files:**
- Modify: `user/vigil/main.c`

- [ ] **Step 1: Remove all cap-related fields and code from vigil**

Delete from `service_t`:
- `needs_auth`, `needs_net_socket`, `needs_net_admin`, `needs_disk_admin`, `needs_fb`
- `needs_cap_grant`, `needs_cap_delegate`, `needs_cap_query`, `needs_setuid`, `needs_thread_create`

Delete the SVC_CAP_* defines at the top.

Delete the caps file parsing in `register_service` (the `strstr(caps_buf, "AUTH")` block).

Delete the entire exec_caps grant block in `start_service` (the 10 `syscall(361, ...)` calls).

Vigil becomes: read run file, read policy file (respawn/oneshot), read mode file, fork+exec. That's it.

- [ ] **Step 2: Commit**

```bash
git add user/vigil/main.c
git commit -m "refactor: strip vigil of all cap logic — pure process supervision"
```

---

## Task 7: Update login + bastion + libauth

**Files:**
- Modify: `user/libauth/auth.c`
- Modify: `user/login/main.c`
- Modify: `user/bastion/main.c`

- [ ] **Step 1: Replace capd_request with sys_auth_session in libauth**

Delete the entire `capd_request` function (socket connect, write kind, read response).
Delete `auth_request_caps` function.

Add a new function:

```c
void
auth_elevate_session(void)
{
    /* Tell the kernel this is an authenticated session.
     * The kernel will grant admin-tier caps at the next execve
     * based on the binary's policy entry. */
    syscall(364);  /* sys_auth_session */
}
```

- [ ] **Step 2: Update login to call auth_elevate_session**

In `user/login/main.c`, replace `auth_request_caps()` with `auth_elevate_session()`.

Login's flow becomes:
1. Read username + password
2. Verify via auth_check
3. Call auth_elevate_session() — marks this session as authenticated
4. setuid/setgid
5. exec shell — kernel grants admin caps from policy because authenticated=1

- [ ] **Step 3: Update bastion to call auth_elevate_session**

In `user/bastion/main.c`, replace `auth_request_caps()` with `auth_elevate_session()`.

Bastion no longer needs AUTH/CAP_GRANT/etc. from capd — it gets them from the policy table because it's in the login→bastion policy entry. Wait — bastion IS the login screen. It gets AUTH from the policy table at its own exec time (service tier), then after auth it calls auth_elevate_session so the spawned shell gets admin caps.

Actually, bastion needs AUTH to read /etc/shadow BEFORE authentication. So bastion's policy must list AUTH as service-tier (not admin):

```
service AUTH FB
admin CAP_GRANT CAP_DELEGATE CAP_QUERY SETUID
```

- [ ] **Step 4: Remove auth.h capd-related declarations**

Remove `auth_request_caps` declaration. Add `auth_elevate_session` declaration.

- [ ] **Step 5: Commit**

```bash
git add user/libauth/auth.c user/login/main.c user/bastion/main.c
git commit -m "feat: login/bastion use sys_auth_session instead of capd"
```

---

## Task 8: Create policy files + update initrd + Makefile

**Files:**
- Modify: `kernel/fs/initrd.c`
- Modify: `Makefile`

- [ ] **Step 1: Add policy files to initrd**

In `kernel/fs/initrd.c`, add static policy file content:

```c
static const char s_cap_login[] = "service AUTH SETUID\n";
static const char s_cap_bastion[] = "service AUTH FB SETUID\n";
static const char s_cap_httpd[] = "service NET_SOCKET VFS_READ\n";
static const char s_cap_dhcp[] = "service NET_SOCKET NET_ADMIN\n";
static const char s_cap_stsh[] = "admin DISK_ADMIN POWER CAP_DELEGATE CAP_QUERY\n";
static const char s_cap_lumen[] = "service FB THREAD_CREATE\n";
static const char s_cap_installer[] = "admin DISK_ADMIN\n";
```

Add these as initrd entries under `/etc/aegis/caps.d/`:
```c
    { "/etc/aegis/caps.d/login",     ... },
    { "/etc/aegis/caps.d/bastion",   ... },
    { "/etc/aegis/caps.d/httpd",     ... },
    { "/etc/aegis/caps.d/dhcp",      ... },
    { "/etc/aegis/caps.d/stsh",      ... },
    { "/etc/aegis/caps.d/lumen",     ... },
    { "/etc/aegis/caps.d/installer", ... },
```

Also add the directory entries for `/etc/aegis/` and `/etc/aegis/caps.d/` to the initrd directory listing.

- [ ] **Step 2: Remove capd from Makefile**

Remove:
- `user/capd/capd.elf` and `user/capd_test/capd_test.elf` build targets
- capd/capd_test from the rootfs e2cp commands
- capd vigil service creation (the printf block for /etc/vigil/services/capd/)
- capd policy files creation (/etc/aegis/capd.d/ block)
- References in ROOTFS_DEPS

- [ ] **Step 3: Add caps.d policy files to ext2 rootfs in Makefile**

Replace the old capd policy block with caps.d files:

```makefile
	# Capability policy files — /etc/aegis/caps.d/<binary>
	printf 'mkdir /etc/aegis\nmkdir /etc/aegis/caps.d\n' \
	    | $(DEBUGFS) -w $(ROOTFS) > /dev/null 2>&1
	printf 'service AUTH SETUID\n' > /tmp/aegis-cap-login
	printf 'service AUTH FB SETUID\n' > /tmp/aegis-cap-bastion
	printf 'service NET_SOCKET\n' > /tmp/aegis-cap-httpd
	printf 'service NET_SOCKET NET_ADMIN\n' > /tmp/aegis-cap-dhcp
	printf 'admin DISK_ADMIN POWER CAP_DELEGATE CAP_QUERY\n' > /tmp/aegis-cap-stsh
	printf 'service FB THREAD_CREATE\n' > /tmp/aegis-cap-lumen
	printf 'admin DISK_ADMIN\n' > /tmp/aegis-cap-installer
	printf 'write /tmp/aegis-cap-login /etc/aegis/caps.d/login\nwrite /tmp/aegis-cap-bastion /etc/aegis/caps.d/bastion\nwrite /tmp/aegis-cap-httpd /etc/aegis/caps.d/httpd\nwrite /tmp/aegis-cap-dhcp /etc/aegis/caps.d/dhcp\nwrite /tmp/aegis-cap-stsh /etc/aegis/caps.d/stsh\nwrite /tmp/aegis-cap-lumen /etc/aegis/caps.d/lumen\nwrite /tmp/aegis-cap-installer /etc/aegis/caps.d/installer\n' \
	    | $(DEBUGFS) -w $(ROOTFS) > /dev/null 2>&1
	rm -f /tmp/aegis-cap-*
```

- [ ] **Step 4: Remove vigil caps files from Makefile**

Remove the old `/etc/vigil/services/*/caps` file creation blocks. Vigil no longer reads caps files.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/initrd.c Makefile
git commit -m "feat: /etc/aegis/caps.d/ policy files in initrd + ext2, remove capd"
```

---

## Task 9: Delete capd + capd_test + update stsh

**Files:**
- Delete: `user/capd/` (entire directory)
- Delete: `user/capd_test/` (entire directory)
- Modify: `user/stsh/caps.c`

- [ ] **Step 1: Delete capd and capd_test**

```bash
rm -rf user/capd user/capd_test
```

- [ ] **Step 2: Update stsh caps builtin**

The `caps` builtin in stsh introspects capabilities. Update it to reflect that exec_caps and cap_grant_exec/runtime are gone. The `grant` builtin (if it exists) should be removed or updated to note that runtime cap granting is no longer supported.

The `sandbox` builtin (sys_spawn with cap_mask) still works — cap_mask restricts the policy-granted caps at spawn time.

- [ ] **Step 3: Update tests**

If `tests/expected/boot.txt` contains a `[CAPD]` line, remove it. The boot output will now show `[CAP] policy: ...` lines instead.

Update `boot.txt` to reflect the new `[CAP] OK: loaded N policies` line.

- [ ] **Step 4: Commit**

```bash
git add -A user/capd/ user/capd_test/ user/stsh/caps.c tests/expected/boot.txt
git commit -m "refactor: delete capd + capd_test, update stsh + boot oracle"
```

---

## Task 10: Update CLAUDE.md + documentation

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update capability model description**

Replace the capability system description to document the new two-tier model:
- Service-tier caps: granted at exec time from /etc/aegis/caps.d/ policy
- Admin-tier caps: same, but only if proc->authenticated (set by login/bastion after auth)
- Baseline: VFS_OPEN, VFS_READ, VFS_WRITE, IPC, PROC_READ, THREAD_CREATE
- capd: eliminated
- exec_caps: eliminated
- sys_cap_grant_exec/runtime: retired (syscalls 361/363)
- sys_auth_session: new (syscall 364)

- [ ] **Step 2: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for kernel cap policy redesign"
```
