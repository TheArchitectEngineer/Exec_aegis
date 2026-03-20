# Phase 3 — Virtual Memory Manager Design Spec

**Date:** 2026-03-19
**Status:** Approved
**Phase:** 3 of N

---

## Goal

Relocate the kernel to the higher-half virtual address `0xFFFFFFFF80000000`,
establish permanent kernel page tables (allocated from the PMM), switch `cr3`
to those tables, and expose `vmm_map_page()` / `vmm_unmap_page()` to the rest
of the kernel. After this phase, `make test` still exits 0 with one additional
line in the expected output.

---

## Scope

- **In scope:** VMA/LMA linker split, boot.asm higher-half trampoline, `-mcmodel=kernel`,
  `vmm_init()` (allocate page tables, identity window, kernel window, switch cr3),
  `vmm_map_page()` / `vmm_unmap_page()`, arch boundary for `cr3` write.
- **Out of scope:** tearing down the identity mapping (Phase 4+), user-space page
  tables, demand paging, TLB shootdown, huge-page API in `vmm_map_page` (only
  `vmm_init` uses huge pages internally), KASLR, NX enforcement (flag exists,
  enforcement deferred until NXE bit is set in EFER).

**Identity mapping kept.** The boot-time identity window `[0 .. 2MB)` is
preserved in Phase 3. `arch_mm_init()` has already consumed `mb_info` before
`vmm_init()` runs, but physical pointers may still be present elsewhere. Tearing
down the identity map is a Phase 4+ concern once the kernel has a virtual address
allocator.

---

## Memory Layout (after Phase 3)

```
0x0000000000000000
  [0 .. 2MB)       → physical [0 .. 2MB)   identity window (kept)
0x0000000000200000
  ...unmapped...
0xFFFFFFFF80000000
  [KERN_VMA .. KERN_VMA+2MB) → physical [0 .. 2MB)   kernel window
  kernel_main lives at 0xFFFFFFFF80100000
0xFFFFFFFFFFFFFFFF
```

Physical layout is unchanged: GRUB loads the kernel image at physical `0x100000`.
The linker assigns each symbol two addresses — a **load address** (physical) and
a **virtual address** (higher-half). The gap is `KERN_VMA = 0xFFFFFFFF80000000`.

---

## Files

| File | Action | Responsibility |
|------|--------|----------------|
| `tools/linker.ld` | Modify | VMA/LMA split; `.multiboot` + `.text.boot` at physical; rest at `KERN_VMA + phys_offset` |
| `kernel/arch/x86_64/boot.asm` | Modify | Add `pdpt_hi` + `pd_hi`; build higher-half mapping; physical far-jump; absolute jump to higher-half before `call kernel_main` |
| `kernel/arch/x86_64/arch.h` | Modify | Add `ARCH_KERNEL_VIRT_BASE`; declare `arch_vmm_load_pml4()` |
| `kernel/arch/x86_64/arch_vmm.c` | Create | `arch_vmm_load_pml4(uint64_t phys)` — only place `cr3` is written |
| `kernel/mm/vmm.h` | Create | Public VMM interface |
| `kernel/mm/vmm.c` | Create | Page table allocator (arch-agnostic); calls `arch_vmm_load_pml4` |
| `kernel/mm/pmm.c` | Modify | Fix `_kernel_end` physical computation (now a virtual address) |
| `kernel/core/main.c` | Modify | Add `vmm_init()` call between `pmm_init()` and `cap_init()` |
| `Makefile` | Modify | Add `-mcmodel=kernel`; add `arch_vmm.c` to `ARCH_SRCS`; add `vmm.c` to `MM_SRCS` |
| `tests/expected/boot.txt` | Modify | Add `[VMM] OK` line (RED before implementation) |
| `.claude/CLAUDE.md` | Modify | Mark VMM in-progress; document VMA/LMA split and identity-mapping decision |

---

## Section 1: Linker Script

```ld
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_BASE = 0x100000;
KERN_VMA  = 0xFFFFFFFF80000000;

SECTIONS
{
    . = PHYS_BASE;

    /* Boot trampoline: VMA = LMA = physical.
     * 32-bit setup code and early 64-bit trampoline run here before
     * jumping to the higher-half virtual address. */
    .multiboot : { KEEP(*(.multiboot)) }
    .text.boot  : { *(.text.boot) }

    /* Switch to higher-half virtual addresses.
     * AT(...) sets the LMA (load/physical address) for each section. */
    . += KERN_VMA;

    .text   : AT(ADDR(.text)   - KERN_VMA) { *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KERN_VMA) { *(.rodata .rodata.*) }
    .data   : AT(ADDR(.data)   - KERN_VMA) { *(.data .data.*) }

    .bss : AT(ADDR(.bss) - KERN_VMA) {
        *(COMMON)
        *(.bss .bss.*)
    }
    _kernel_end = .;   /* higher-half virtual address */
}
```

`_kernel_end` is now a higher-half virtual address. `pmm.c` computes the
physical kernel end as `_kernel_end - ARCH_KERNEL_VIRT_BASE`.

---

## Section 2: boot.asm Changes

### 2a. Physical address macro

All `.bss` labels (page tables, stack) now have higher-half virtual addresses.
In 32-bit mode we need their physical addresses. NASM evaluates constant
expressions at assemble time:

```nasm
KERN_VMA equ 0xFFFFFFFF80000000

; Physical address of a label (result fits in 32 bits):
mov eax, (pml4_table - KERN_VMA)
```

### 2b. Two new page tables

```nasm
section .bss
align 4096
pml4_table: resb 4096
align 4096
pdpt_lo:    resb 4096   ; identity (was pdpt_table)
align 4096
pdpt_hi:    resb 4096   ; higher-half (new)
align 4096
pd_lo:      resb 4096   ; identity (was pd_table)
align 4096
pd_hi:      resb 4096   ; higher-half (new)
```

Page table wiring (all entries use physical addresses of the target table):

```
pml4[0]      → pdpt_lo    (present | writable)
pml4[511]    → pdpt_hi    (present | writable)
pdpt_lo[0]   → pd_lo      (present | writable)
pdpt_hi[510] → pd_hi      (present | writable)   ← index 510 = bits 38:30 of KERN_VMA
pd_lo[0]     → 0x0        (present | writable | PS)   2MB identity huge page
pd_hi[0]     → 0x0        (present | writable | PS)   2MB kernel window huge page
```

PDPT index 510: bits 38:30 of `0xFFFFFFFF80000000` = `0b111111110` = 510.

### 2c. Jump sequence

The 32-bit code up through `CR0.PG` enable is unchanged in logic; it uses
`(label - KERN_VMA)` wherever a physical address is needed.

After paging is enabled:

```nasm
    ; Far jump to 64-bit code selector — offset is 32-bit, must be physical.
    ; .long_mode_phys is in .text.boot (VMA = LMA), so its address IS physical.
    jmp 0x08:.long_mode_phys

bits 64
.long_mode_phys:
    ; Executing at physical address, identity-mapped. Safe.
    ; Load higher-half virtual address and jump.
    mov rax, .long_mode_high
    jmp rax

.long_mode_high:
    ; Executing at 0xFFFFFFFF80xxxxxx — higher half is live.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, boot_stack_top   ; higher-half virtual address (correct)
    call kernel_main           ; higher-half virtual address (correct)
.hang:
    hlt
    jmp .hang
```

`.long_mode_phys` must be in `.text.boot` so its VMA equals its LMA (physical).
The segment register loads and stack setup move to `.long_mode_high` since they
must execute at the correct virtual address.

---

## Section 3: arch.h Additions

```c
/* Virtual base address of the kernel image.
 * pmm.c uses this to convert _kernel_end (virtual) to a physical address. */
#define ARCH_KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL

/* Load the physical address of a PML4 into CR3.
 * Implemented in arch_vmm.c — the only place in the codebase that writes CR3.
 * Flushes the TLB entirely (CR3 reload). */
void arch_vmm_load_pml4(uint64_t phys);
```

---

## Section 4: arch_vmm.c

```c
#include "arch.h"
#include <stdint.h>

void arch_vmm_load_pml4(uint64_t phys)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
}
```

Single function. All `cr3` writes in the kernel go through this. No other file
touches `cr3` directly.

---

## Section 5: VMM Public Interface (vmm.h)

```c
#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Page-table entry flag bits (x86-64 PTE format). */
#define VMM_FLAG_PRESENT   (1UL << 0)
#define VMM_FLAG_WRITABLE  (1UL << 1)
#define VMM_FLAG_USER      (1UL << 2)
#define VMM_FLAG_NX        (1UL << 63)

/* Initialise the virtual memory manager.
 * Requires pmm_init() to have been called first.
 * Allocates permanent kernel page tables from the PMM, switches cr3,
 * and prints [VMM] OK. Panics on failure. */
void vmm_init(void);

/* Map one 4KB page: virt → phys with given flags.
 * Allocates intermediate page tables from PMM as needed.
 * Panics if virt is already mapped or phys is not page-aligned. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap one 4KB page. Panics if not mapped.
 * Does not free intermediate tables (deferred to a future reclaim pass). */
void vmm_unmap_page(uint64_t virt);

#endif /* VMM_H */
```

---

## Section 6: vmm.c Structure

```c
#include "vmm.h"
#include "pmm.h"
#include "arch.h"     /* arch_vmm_load_pml4, ARCH_KERNEL_VIRT_BASE */
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

#define PTE_PRESENT  VMM_FLAG_PRESENT
#define PTE_WRITABLE VMM_FLAG_WRITABLE
#define PTE_PS       (1UL << 7)   /* huge page (PD level) */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL
```

### vmm_init() sequence

```
1. pml4_phys = pmm_alloc_page(); zero the page.

2. Map identity window [0 .. 2MB):
   - alloc pdpt, pd
   - pml4[0]    = pdpt_phys | PRESENT | WRITABLE
   - pdpt[0]    = pd_phys   | PRESENT | WRITABLE
   - pd[0]      = 0x0       | PRESENT | WRITABLE | PS   (2MB huge page)

3. Map kernel window [KERN_VMA .. KERN_VMA+2MB):
   - alloc pdpt_hi, pd_hi
   - pml4[511]  = pdpt_hi_phys | PRESENT | WRITABLE
   - pdpt_hi[510] = pd_hi_phys | PRESENT | WRITABLE
   - pd_hi[0]   = 0x0          | PRESENT | WRITABLE | PS

4. arch_vmm_load_pml4(pml4_phys);

5. printk("[VMM] OK: kernel mapped to 0xFFFFFFFF80000000\n");
```

Step 3 uses the same 2MB huge-page approach as the identity window —
the kernel fits within the first 2MB of physical space, so one PD entry suffices.
`vmm_map_page` (4KB granularity) is used by callers for arbitrary future mappings.

### vmm_map_page() walk

Walk PML4 → PDPT → PD → PT. At each level: if entry not present, allocate a
new page from PMM (zeroed), install it. At PT level: if entry already present,
panic (double-map). Set the PT entry to `(phys & PTE_ADDR_MASK) | flags | PRESENT`.

### vmm_unmap_page()

Walk to PT. If entry not present, panic. Clear the entry. Issue `invlpg` via
inline asm (only `cr3`/`invlpg` operations live in arch_vmm.c — add
`arch_vmm_invlpg(uint64_t virt)` to arch.h and arch_vmm.c).

---

## Section 7: pmm.c Fix

`_kernel_end` is now a higher-half virtual address. The kernel reservation in
`pmm_init()` changes from:

```c
/* OLD (Phase 2) */
pmm_reserve_region(ARCH_KERNEL_PHYS_BASE,
    (uint64_t)(uintptr_t)_kernel_end - ARCH_KERNEL_PHYS_BASE);
```

to:

```c
/* NEW (Phase 3) */
pmm_reserve_region(ARCH_KERNEL_PHYS_BASE,
    (uint64_t)(uintptr_t)_kernel_end - ARCH_KERNEL_VIRT_BASE - ARCH_KERNEL_PHYS_BASE);
```

`(uintptr_t)_kernel_end - ARCH_KERNEL_VIRT_BASE` = physical end of kernel image.
Subtract `ARCH_KERNEL_PHYS_BASE` to get the length from the kernel load address.

---

## Section 8: main.c Boot Sequence

```c
arch_init();           /* serial + VGA                              */
arch_mm_init(mb_info); /* parse multiboot2 memory map              */
                       /* NOTE: mb_info is physical; arch_mm_init  */
                       /* consumes it before vmm_init runs.         */
pmm_init();            /* bitmap allocator — [PMM] OK              */
vmm_init();            /* page tables, cr3 switch — [VMM] OK       */
cap_init();            /* capability stub — [CAP] OK               */
printk("[AEGIS] System halted.\n");
arch_debug_exit(0x01);
```

---

## Section 9: Test Harness

### tests/expected/boot.txt (after Phase 3)

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[AEGIS] System halted.
```

### TDD order

1. Add VMM line to `boot.txt` → `make test` fails (RED)
2. Implement all changes → `make test` passes (GREEN)
3. Review → refactor if needed

---

## Section 10: Makefile Changes

```makefile
ARCH_SRCS = \
    kernel/arch/x86_64/arch.c \
    kernel/arch/x86_64/arch_exit.c \
    kernel/arch/x86_64/arch_mm.c \
    kernel/arch/x86_64/arch_vmm.c \
    kernel/arch/x86_64/serial.c \
    kernel/arch/x86_64/vga.c

MM_SRCS = \
    kernel/mm/pmm.c \
    kernel/mm/vmm.c
```

Add `-mcmodel=kernel` to CFLAGS. This tells GCC that all code and data reside
in the top 2GB of the virtual address space (`0xFFFFFFFF00000000` and above),
allowing it to generate more efficient RIP-relative addressing without
relocations that assume a low-address kernel.

---

## Decisions Log

| Question | Decision | Reason |
|----------|----------|--------|
| Higher-half relocation now vs. deferred | Now (Phase 3) | Doing it later means updating more code; every subsequent phase assumes KERN_VMA |
| Identity mapping kept or torn down | Kept in Phase 3 | `arch_mm_init` consumed `mb_info` but other physical refs may exist; tear-down deferred to Phase 4+ |
| Huge pages for kernel window | Yes (2MB, PD level) | Kernel fits in first 2MB; avoids allocating PT pages for the kernel window itself |
| `cr3` write location | `arch_vmm.c` only | Keeps `kernel/mm/` arch-agnostic; same pattern as arch boundary for PMM |
| `invlpg` location | `arch_vmm.c` (`arch_vmm_invlpg`) | Same reason — arch-specific instruction |
| `vmm_map_page` huge-page support | Not in public API | YAGNI; `vmm_init` handles huge pages internally; callers use 4KB API |
| Freeing intermediate tables on unmap | Deferred | Requires a reclaim pass; out of scope for Phase 3 |
| NX enforcement | Flag defined, not enforced | NXE bit in EFER not yet set; add in the phase that sets up proper segment/privilege separation |
