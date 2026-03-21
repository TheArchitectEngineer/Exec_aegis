# Phase 8: SMAP + User Pointer Validation Design

## Goal

Close the user-pointer security hole in `sys_write`: add a bounds check that
returns EFAULT for kernel-space addresses, enable Supervisor Mode Access
Prevention (SMAP) so accidental kernel→user dereferences fault at the hardware
level, and wrap the intentional user-memory access with `stac`/`clac` so the
syscall path continues to work correctly under SMAP.

---

## Background

`sys_write` currently dereferences `arg2` (a user-supplied virtual address)
without any validation. A malicious process can pass a kernel-space address and
read arbitrary kernel memory through `printk`. The comment in `syscall.c`
documents this as a known Phase 5 security debt.

Two complementary protections are needed:

- **Bounds check** — software enforcement; catches intentional attacks.
  Returns EFAULT (`-14`, Linux convention, required for future musl port).
- **SMAP** — hardware enforcement; catches accidental kernel→user dereferences
  (bugs, not attacks). When `CR4.SMAP=1`, any ring-0 access to a user-mode
  page (U/S=1 in PTE) causes a #PF unless `RFLAGS.AC=1`. The `stac`
  instruction sets AC (re-enables access); `clac` clears it.

SMAP has been present since Intel Broadwell (2014) and AMD Zen (2017). We
target modern hardware and VMs, but the design detects and skips gracefully on
older CPUs rather than panicking — the bounds check remains effective either way.

**Why `stac`/`clac` are required:** enabling SMAP without them causes an
immediate #PF on the first `s[i]` dereference in `sys_write`, since `s` points
into a user-mode page. The window between `stac` and `clac` is safe because
`SFMASK` keeps `IF=0` during syscall dispatch — no interrupt can fire inside
that window and leave AC set.

---

## Architecture

### New Module: `kernel/arch/x86_64/arch_smap.c`

```c
#include "printk.h"
#include <stdint.h>

static int
cpuid_smap_supported(void)
{
    uint32_t ebx;
    /* CPUID leaf 7, subleaf 0, EBX bit 20 = SMAP */
    __asm__ volatile (
        "cpuid"
        : "=b"(ebx)
        : "a"(7), "c"(0)
        : "edx"
    );
    return (ebx >> 20) & 1;
}

void
arch_smap_init(void)
{
    if (!cpuid_smap_supported()) {
        printk("[SMAP] WARN: not supported by CPU\n");
        return;
    }
    /* Set CR4.SMAP (bit 21) */
    __asm__ volatile (
        "mov %%cr4, %%rax\n"
        "or $0x200000, %%rax\n"
        "mov %%rax, %%cr4\n"
        : : : "rax"
    );
    printk("[SMAP] OK: supervisor access prevention active\n");
}
```

### Additions to `kernel/arch/x86_64/arch.h`

Declaration:
```c
/* arch_smap_init — detect SMAP via CPUID and enable CR4.SMAP if supported.
 * Prints [SMAP] OK or [SMAP] WARN. Must be called after arch_syscall_init(). */
void arch_smap_init(void);
```

Static inline macros (placed near the syscall section of arch.h):
```c
/* arch_stac — set RFLAGS.AC, temporarily permitting ring-0 access to
 * user-mode pages under SMAP. Use only to bracket intentional user-memory
 * accesses; always pair with arch_clac(). No-op if SMAP is not enabled. */
static inline void arch_stac(void) { __asm__ volatile("stac" ::: "memory"); }

/* arch_clac — clear RFLAGS.AC, re-enabling SMAP protection.
 * Must be called after every arch_stac(). */
static inline void arch_clac(void) { __asm__ volatile("clac" ::: "memory"); }
```

The `"memory"` clobber is a compiler barrier ensuring memory accesses inside
the stac/clac window are not reordered outside it.

### `kernel/syscall/syscall.c`

Add `user_ptr_valid` as a static inline above `sys_write`:

```c
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFUL

/* user_ptr_valid — return 1 if [addr, addr+len) lies entirely within the
 * canonical user address space, 0 otherwise.
 * Handles len=0 (always valid), len overflow, and kernel-address attacks. */
static inline int
user_ptr_valid(uint64_t addr, uint64_t len)
{
    return len <= USER_ADDR_MAX && addr <= USER_ADDR_MAX - len;
}
```

Updated `sys_write`:

```c
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;   /* EFAULT */
    const char *s = (const char *)(uintptr_t)arg2;
    uint64_t i;
    /* stac/clac bracket the intentional user-memory read.
     * IF=0 (SFMASK) ensures no interrupt fires with AC set. */
    arch_stac();
    for (i = 0; i < arg3; i++)
        printk("%c", s[i]);
    arch_clac();
    return arg3;
}
```

The `(const char *)(uintptr_t)arg2` cast is safe here: `user_ptr_valid` has
confirmed `arg2` is a canonical user-space address, and `stac` ensures SMAP
permits the access.

### `kernel/core/main.c`

Call `arch_smap_init()` immediately after `arch_syscall_init()`:

```c
    arch_syscall_init();        /* SYSCALL/SYSRET — [SYSCALL] OK             */
    arch_smap_init();           /* SMAP detect + enable — [SMAP] OK/WARN     */
```

### `Makefile`

Add `arch_smap.c` to `ARCH_SRCS`:
```makefile
ARCH_SRCS = \
    ...
    kernel/arch/x86_64/arch_smap.c
```

Add `-cpu Broadwell` to the QEMU test invocation so SMAP is always present
and the boot.txt oracle is deterministic across QEMU versions:
```makefile
QEMU_FLAGS = ... -cpu Broadwell ...
```

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/arch/x86_64/arch_smap.c` | New — CPUID check + CR4.SMAP enable |
| `kernel/arch/x86_64/arch.h` | Add `arch_smap_init` declaration; add `arch_stac`/`arch_clac` static inlines |
| `kernel/syscall/syscall.c` | Add `user_ptr_valid`; EFAULT return; `arch_stac`/`arch_clac` around user access |
| `kernel/core/main.c` | Call `arch_smap_init()` after `arch_syscall_init()` |
| `Makefile` | Add `arch_smap.c` to `ARCH_SRCS`; add `-cpu Broadwell` to QEMU flags |
| `tests/expected/boot.txt` | Add `[SMAP] OK: supervisor access prevention active` after `[SYSCALL] OK` |
| `.claude/CLAUDE.md` | Update build status table |

---

## Test Oracle

`tests/expected/boot.txt` gains one line after `[SYSCALL] OK`:

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[CAP] OK: capability subsystem reserved
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SMAP] OK: supervisor access prevention active
[VMM] OK: identity map removed
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```

---

## Success Criteria

1. `make test` exits 0.
2. `grep -rn 'stac\|clac' kernel/` shows exactly the two definitions in
   `arch.h` and their two call sites in `syscall.c`.
3. `user_ptr_valid` returns 0 for `addr=0xFFFFFFFF80000000, len=1` (kernel
   address) and 1 for `addr=0x400000, len=4096` (valid user address).
