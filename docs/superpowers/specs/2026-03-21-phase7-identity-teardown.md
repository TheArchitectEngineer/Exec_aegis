# Phase 7: Identity Map Teardown Design

## Goal

Eliminate every remaining physical-address-as-pointer cast in the kernel, then
remove the `[0..4MB) → [0..4MB)` identity mapping from the master PML4.
After this phase, a kernel access to any low physical address causes a page
fault rather than silently succeeding.

---

## Background

The identity map has survived since Phase 2 because several subsystems cast
PMM physical addresses directly to pointers:

| Site | Cast | Object |
|------|------|--------|
| `kernel/sched/sched.c:44` | `(aegis_task_t *)(uintptr_t)tcb_phys` | TCB |
| `kernel/sched/sched.c:65` | `(uint8_t *)(uintptr_t)p` | kernel task stack pages |
| `kernel/proc/proc.c:69` | `(aegis_process_t *)(uintptr_t)tcb_phys` | PCB |
| `kernel/elf/elf.c:91` | `(uint8_t *)(uintptr_t)first_phys` | ELF segment write buffer |

Phase 6 eliminated every cast in `vmm.c`. These four are the remaining targets.

The fix for all four is to give each allocation a proper higher-half virtual
address via a new kernel VA bump allocator (`kva`). Once no physical-address
casts remain, `pml4[0]` can be cleared and the TLB flushed.

**Not identity-map dependencies** (confirmed safe, no change needed):
- `syscall.c`: `(const char *)(uintptr_t)arg2` — user virtual address already
  mapped in the loaded user PML4, not a physical address
- `pmm.c`: VA→physical arithmetic on `_kernel_end`, not a dereference
- `arch_syscall.c`, `gdt.c`: kernel symbol addresses cast to `uint64_t` for
  MSR/GDTR writes — higher-half VAs, not physical addresses

---

## Architecture

### New Module: `kernel/mm/kva.c` + `kernel/mm/kva.h`

A bump allocator over the virtual range beginning at
`ARCH_KERNEL_VIRT_BASE + 0x800000` (`pd_hi[4]`, currently NULL in the
master PML4). This is 2MB above the window slot (`pd_hi[3]`) and well clear
of the kernel image (`pd_hi[0..1]`) and KSTACK area (`pd_hi[2]`).

Public API:
```c
void  kva_init(void);
void *kva_alloc_pages(uint64_t n);
uint64_t kva_page_phys(void *va);
```

**`kva_init()`** sets the cursor `s_kva_next = ARCH_KERNEL_VIRT_BASE + 0x800000UL`
and prints `[KVA] OK: kernel virtual allocator active`.

**`kva_alloc_pages(n)`** — for each of `n` pages:
1. `phys = pmm_alloc_page()` — panics on OOM
2. `vmm_map_page(s_kva_next, phys, VMM_FLAG_WRITABLE)`
3. Advance `s_kva_next += 4096`

Returns the base VA as `void *`. The window must be active (i.e., `vmm_init()`
must have been called). Call order in `main.c` guarantees this.

**`kva_page_phys(va)`** — walks the master PML4 via `vmm_window_map` to
recover the physical address of a kva-mapped page. Used by `elf_load` to
obtain `first_phys` for `vmm_map_user_page` after writing segment data through
the kernel VA. Returns the physical address (4KB-aligned).

**Why kva pages are visible in user PML4s:**
`vmm_create_user_pml4` copies PML4 entries 256–511 from the master PML4, but
the intermediate tables (`pdpt_hi`, `pd_hi`) are *shared* — both master and
user PML4s point to the same physical `pd_hi` page. When `kva_alloc_pages`
installs a new PT page into `pd_hi[4]` via `vmm_map_page`, that entry is
immediately visible in all existing and future user PML4s. No explicit
propagation required.

**No free path** — Phase 7 scope. Kernel tasks (task_kbd, task_heartbeat) never
exit. The user process exits via `sched_exit`, which currently does not free
TCB/stack pages (pre-existing leak). Resource reclamation is deferred to a
future phase when a proper slab or free-list is introduced.

---

## Changes per File

### `kernel/mm/kva.c` (new)

```c
#include "kva.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include "arch.h"
#include <stdint.h>

#define KVA_BASE (ARCH_KERNEL_VIRT_BASE + 0x800000UL)

static uint64_t s_kva_next;

void
kva_init(void)
{
    s_kva_next = KVA_BASE;
    printk("[KVA] OK: kernel virtual allocator active\n");
}

void *
kva_alloc_pages(uint64_t n)
{
    uint64_t base = s_kva_next;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            printk("[KVA] FAIL: out of memory\n");
            for (;;) {}
        }
        vmm_map_page(s_kva_next, phys, VMM_FLAG_WRITABLE);
        s_kva_next += 4096UL;
    }
    return (void *)base;
}

uint64_t
kva_page_phys(void *va)
{
    return vmm_phys_of((uint64_t)(uintptr_t)va);
}
```

`vmm_phys_of` is a new internal function in `vmm.c` that performs a 4-level
page-table walk via the window and returns the physical address for a mapped
virtual address. Declared in `vmm.h`.

### `kernel/mm/vmm.h`

Add declaration:
```c
uint64_t vmm_phys_of(uint64_t virt);
void     vmm_teardown_identity(void);
```

### `kernel/mm/vmm.c`

**`vmm_phys_of(virt)`** — 4-level walk using the walk-overwrite window pattern,
returns `PTE_ADDR(pt[pt_idx])`. Panics if any level is not present.

**`vmm_teardown_identity()`**:
```c
void
vmm_teardown_identity(void)
{
    uint64_t *pml4 = vmm_window_map(s_pml4_phys);
    pml4[0] = 0;
    vmm_window_unmap();
    arch_vmm_load_pml4(s_pml4_phys);   /* full TLB flush via CR3 reload */
    printk("[VMM] OK: identity map removed\n");
}
```

Clearing `pml4[0]` removes the entire low 512GB range in one write. CR3 reload
is used instead of `invlpg` — the identity window covers two 2MB huge pages,
and a full TLB flush is both simpler and more complete. The `pdpt_lo` and
`pd_lo` pages remain allocated but are now unreachable dead weight; they will
be freed when a kernel page-table free path exists.

### `kernel/sched/sched.c`

In `sched_spawn`, replace physical-cast allocations:

```c
/* Before (identity-map dependent): */
uint64_t tcb_phys = pmm_alloc_page();
aegis_task_t *task = (aegis_task_t *)(uintptr_t)tcb_phys;
...
uint64_t p = pmm_alloc_page();
stack = (uint8_t *)(uintptr_t)p;

/* After: */
aegis_task_t *task  = kva_alloc_pages(1);
uint8_t      *stack = kva_alloc_pages(STACK_PAGES);
```

The `STACK_PAGES`-page loop that relied on sequential physical allocation is
replaced by `kva_alloc_pages(STACK_PAGES)`, which maps `STACK_PAGES`
individually-allocated PMM pages to consecutive virtual addresses. Physical
contiguity is no longer assumed or required.

Update `sched_exit` comment: TCBs are now in the kernel higher-half (kva
range), accessible from any CR3 including user PML4s. The unconditional master
PML4 switch at the top of `sched_exit` is retained as a defensive measure
(ensures we're always on a stack that's identity-free and fully mapped) but
its original reason (TCBs in identity-mapped low memory) no longer applies.

### `kernel/proc/proc.c`

In `proc_spawn`, replace physical-cast TCB allocation and fixed KSTACK_VA:

```c
/* Before: */
#define KSTACK_VA 0xFFFFFFFF80400000ULL
...
uint64_t tcb_phys = pmm_alloc_page();
aegis_process_t *proc = (aegis_process_t *)(uintptr_t)tcb_phys;
...
uint64_t kstack_phys = pmm_alloc_page();
vmm_map_page(KSTACK_VA, kstack_phys, VMM_FLAG_WRITABLE);
uint8_t *kstack = (uint8_t *)KSTACK_VA;

/* After: */
aegis_process_t *proc   = kva_alloc_pages(1);
uint8_t         *kstack = kva_alloc_pages(1);
```

`#define KSTACK_VA` is deleted. Each call to `proc_spawn` gets a distinct
kernel stack VA from the bump allocator. `proc->task.stack_base` and
`proc->task.kernel_stack_top` are set from the returned pointer as before.
`arch_set_kernel_stack(proc->task.kernel_stack_top)` correctly wires TSS.RSP0
to the per-process value.

### `kernel/elf/elf.c`

In `elf_load`, replace the physical-cast segment write:

```c
/* Before: */
uint8_t *dst = (uint8_t *)(uintptr_t)first_phys;

/* After: */
uint8_t *dst = kva_alloc_pages(page_count);
```

File bytes are copied and BSS is zeroed through `dst` (kernel VA). Physical
addresses for `vmm_map_user_page` are recovered via `kva_page_phys`:

```c
for (j = 0; j < page_count; j++) {
    vmm_map_user_page(pml4_phys,
                      ph->p_vaddr + j * 4096UL,
                      kva_page_phys(dst + j * 4096UL),
                      map_flags);
}
```

### `kernel/core/main.c`

Call order:
```c
vmm_init();               /* window active */
kva_init();               /* bump allocator online */
...
sched_spawn(task_kbd);
sched_spawn(task_heartbeat);
proc_spawn_init();
vmm_teardown_identity();  /* pml4[0] = 0, CR3 reload */
sched_start();
```

Add `#include "../mm/kva.h"`.

---

## Files Changed

| File | Change |
|------|--------|
| `kernel/mm/kva.c` | New — bump allocator |
| `kernel/mm/kva.h` | New — `kva_init`, `kva_alloc_pages`, `kva_page_phys` |
| `kernel/mm/vmm.c` | Add `vmm_phys_of`, `vmm_teardown_identity` |
| `kernel/mm/vmm.h` | Declare `vmm_phys_of`, `vmm_teardown_identity` |
| `kernel/sched/sched.c` | Replace physical casts with `kva_alloc_pages` |
| `kernel/proc/proc.c` | Replace physical casts + delete `KSTACK_VA` |
| `kernel/elf/elf.c` | Replace physical cast + `kva_page_phys` for user mapping |
| `kernel/core/main.c` | Wire `kva_init` + `vmm_teardown_identity` |
| `Makefile` | Add `kva.c` to `MM_SRCS` |
| `tests/expected/boot.txt` | Add `[KVA] OK` and `[VMM] OK: identity map removed` lines |
| `.claude/CLAUDE.md` | Update build status table |

---

## Test Oracle

`tests/expected/boot.txt` gains two lines after the mapped-window line:

```
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[VMM] OK: identity map removed
[CAP] OK: capability subsystem reserved
...
```

---

## Success Criteria

1. `make test` exits 0.
2. `grep -rn '(uintptr_t)' kernel/ --include='*.c'` produces zero
   physical-address dereferences. Remaining hits must all be one of:
   - VA→integer casts for MSRs/GDTR (arch_syscall.c, gdt.c)
   - VA→physical arithmetic without dereference (pmm.c `_kernel_end`)
   - The `vmm_init` bootstrap casts (already have `// SAFETY:` comments)
   - The `alloc_table_early` bootstrap cast (already has `// SAFETY:` comment)
3. `pml4[0]` is zero in the running page tables (identity map absent).
