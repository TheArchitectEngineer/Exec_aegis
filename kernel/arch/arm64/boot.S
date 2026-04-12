/*
 * boot.S — ARM64 kernel entry (TTBR1 higher-half boot).
 *
 * QEMU virt loads us at physical 0x40200000 (both via `-kernel ELF`
 * thanks to the ELF PhysAddr header, and via `-kernel Image` via
 * QEMU's fixed 2MB arm64 Image load offset). The linker LMA is set
 * to 0x40200000 to match — see linker.ld. KERN_VA_OFFSET stays at
 * 0xFFFF000000000000 so the `PA + KERN_VA_OFFSET` convention used
 * throughout the kernel continues to work.
 *
 * Linux arm64 boot protocol contract:
 *   x0 = DTB physical address
 *   x1 = x2 = x3 = 0 (reserved)
 *   MMU off, caches off, EL1 or EL2
 *   PC = image base
 *
 * The first 64 bytes of .text.boot ARE the Linux arm64 Image header:
 *   0x00  code0       = `b _kernel_entry`  (branch over the header)
 *   0x04  code1       = 0
 *   0x08  text_offset = 0
 *   0x10  image_size  = 0  (0 tells the loader to use whatever it
 *                           computes from file size; modern QEMU and
 *                           Linux accept this, and tools/gen-arm64-
 *                           image.py stamps a real value after
 *                           flattening the ELF if needed)
 *   0x18  flags       = 2  (4K, little-endian)
 *   0x20  res2        = 0
 *   0x28  res3        = 0
 *   0x30  res4        = 0
 *   0x38  magic       = 0x644D5241 ("ARM\x64")
 *   0x3c  res5        = 0
 *
 * ENTRY(_start) in linker.ld points at _start, which IS the code0
 * branch. QEMU's `-kernel ELF` path jumps to the ELF entry (_start =
 * image base), executes the branch, and lands at _kernel_entry. The
 * `-kernel Image` path does the same: firmware jumps to image base,
 * executes the branch, lands at _kernel_entry.
 *
 * tools/gen-arm64-image.py runs `aarch64-elf-objcopy -O binary` to
 * flatten the ELF; the resulting .img already contains the header in
 * its first 64 bytes, so the Python script only needs to stamp the
 * `image_size` field (and can also re-verify magic/code0 as a sanity
 * check).
 */

/* VA offset: high VA - physical addr */
#define VA_OFF 0xFFFF000000000000

    .section .text.boot
    .globl _start
_start:
    /* ── Linux arm64 Image header (64 bytes) ────────────────────────
     * code0: `b _kernel_entry` — imm26 = 16 words = 0x10.
     * opcode bits 31:26 = 0b000101, imm = word offset = (64/4) = 16.
     * Assembled: `b .+64` = 0x14000010.  We emit the raw encoding via
     * .word rather than `b _kernel_entry` so the offset is locked in
     * at assemble time regardless of how the rest of .text.boot
     * expands. Double-checked with
     *     echo 'b .+64' | aarch64-elf-as - -o /tmp/b.o \
     *       && aarch64-elf-objdump -d /tmp/b.o
     * which prints `0: 14000010  b 40 <.text+0x40>`.
     */
    .word   0x14000010          /* 0x00  code0: b _kernel_entry */
    .word   0x00000000          /* 0x04  code1 (reserved) */
    .quad   0                   /* 0x08  text_offset */
    .quad   0                   /* 0x10  image_size (0 = use file size) */
    .quad   0x0000000000000002  /* 0x18  flags: page=4K, LE */
    .quad   0                   /* 0x20  res2 */
    .quad   0                   /* 0x28  res3 */
    .quad   0                   /* 0x30  res4 */
    .word   0x644D5241          /* 0x38  magic: "ARM\x64" */
    .word   0                   /* 0x3c  res5 (PE header offset) */

    .globl _kernel_entry
_kernel_entry:
    mov     x19, x0             /* save DTB pointer */

    /*
     * Drop from EL2 to EL1 if firmware handed us EL2 (Pi 4/5 default).
     * QEMU virt with cortex-a72 starts at EL1 so this falls through.
     * x19 (DTB) is preserved across the eret.
     */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    .Lat_el1
    mov     x0, #(1 << 31)      /* HCR_EL2.RW = EL1 is AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5          /* SPSR_EL2: D|A|I|F set, EL1h */
    msr     spsr_el2, x0
    adr     x0, .Lat_el1
    msr     elr_el2, x0
    eret
.Lat_el1:
    msr     daifset, #0xf       /* mask all exceptions */
    msr     spsel, #1           /* use SP_EL1 */

    /* Enable FP/SIMD */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /* Zero BSS (physical addresses — MMU off).
     * __bss_start/__bss_end are high VAs; subtract VA_OFF to get PA. */
    mov     x3, #0xFFFF
    lsl     x3, x3, #48         /* x3 = 0xFFFF000000000000 (VA offset) */
    ldr     x1, =__bss_start
    sub     x1, x1, x3          /* VA → PA */
    ldr     x2, =__bss_end
    sub     x2, x2, x3
.Lzero_bss:
    cmp     x1, x2
    b.ge    .Lbss_done
    str     xzr, [x1], #8
    b       .Lzero_bss
.Lbss_done:

    /* Boot stack (physical) */
    ldr     x1, =boot_stack_top
    sub     x1, x1, x3          /* VA → PA */
    mov     sp, x1

    /* Build page tables inline (can't call C — it's linked at high VA).
     * x3 = VA_OFF = 0xFFFF000000000000 (already set above).
     *
     * TTBR0 identity map: boot_l0[0] → boot_l1
     *   boot_l1[0] → device 1GB, boot_l1[1] → RAM 1GB
     * TTBR1 kernel map: kern_l0[0] → kern_l1
     *   kern_l1[0] → device 1GB, kern_l1[1] → RAM 1GB */

    /* Load physical addresses of page tables */
    ldr     x10, =boot_l0
    sub     x10, x10, x3        /* boot_l0 PA */
    ldr     x11, =boot_l1
    sub     x11, x11, x3        /* boot_l1 PA */
    ldr     x12, =kern_l0
    sub     x12, x12, x3        /* kern_l0 PA */
    ldr     x13, =kern_l1
    sub     x13, x13, x3        /* kern_l1 PA */

    /* Zero all four tables (512 × 8 = 4096 bytes each) */
    mov     x14, #512
    mov     x15, x10
1:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    1b
    mov     x14, #512
    mov     x15, x11
2:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    2b
    mov     x14, #512
    mov     x15, x12
3:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    3b
    mov     x14, #512
    mov     x15, x13
4:  str     xzr, [x15], #8
    subs    x14, x14, #1
    b.ne    4b

    /* PTE constants */
    .set PTE_V, (1 << 0)       /* Valid */
    .set PTE_T, (1 << 1)       /* Table descriptor */
    .set PTE_AF, (1 << 10)     /* Access Flag */
    .set PTE_SH, (3 << 8)      /* Inner Shareable */
    .set PTE_DEV, (1 << 2)     /* AttrIndx=1 (Device) */

    /* TTBR0: boot_l0[0] → boot_l1 (table) */
    mov     x14, #(PTE_V | PTE_T)
    orr     x14, x14, x11      /* boot_l1 PA | valid | table */
    str     x14, [x10]          /* boot_l0[0] */

    /* boot_l1[0] = device 1GB block at PA 0 */
    mov     x14, #(PTE_V | PTE_AF | PTE_DEV)
    str     x14, [x11]          /* boot_l1[0] = 0 | device */

    /* boot_l1[1] = RAM 1GB block at PA 0x40000000 */
    mov     x14, #(PTE_V | PTE_AF | PTE_SH)
    orr     x14, x14, #0x40000000
    str     x14, [x11, #8]      /* boot_l1[1] */

    /* TTBR1: kern_l0[0] → kern_l1 (table) */
    mov     x14, #(PTE_V | PTE_T)
    orr     x14, x14, x13      /* kern_l1 PA */
    str     x14, [x12]          /* kern_l0[0] */

    /* kern_l1[0] = device 1GB at PA 0 */
    mov     x14, #(PTE_V | PTE_AF | PTE_DEV)
    str     x14, [x13]          /* kern_l1[0] */

    /* kern_l1[1] = RAM 1GB at PA 0x40000000 */
    mov     x14, #(PTE_V | PTE_AF | PTE_SH)
    orr     x14, x14, #0x40000000
    str     x14, [x13, #8]      /* kern_l1[1] */

    /* ── MMU configuration ── */

    /* MAIR: Attr0=Normal WB, Attr1=Device-nGnRnE */
    ldr     x1, =0x00000000000000FF
    msr     mair_el1, x1

    /* TCR: T0SZ=16 (48-bit user), T1SZ=16 (48-bit kernel),
     * TG0=4KB, TG1=4KB, Inner Shareable, WB Allocate */
    ldr     x1, =(16 | (0b11 << 12) | (0b01 << 10) | (0b01 << 8) | \
                  (16 << 16) | (0b10 << 30) | (0b11 << 28) | (0b01 << 26) | (0b01 << 24))
    msr     tcr_el1, x1

    /* TTBR0: identity map (boot_l0 PA in x10) */
    msr     ttbr0_el1, x10

    /* TTBR1: kernel map (kern_l0 PA in x12) */
    msr     ttbr1_el1, x12

    dsb     sy
    isb

    /* Enable MMU + caches */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)
    orr     x1, x1, #(1 << 2)
    orr     x1, x1, #(1 << 12)
    msr     sctlr_el1, x1
    isb

    /* Jump to high VA (TTBR1 range) */
    ldr     x2, =.Lhigh
    br      x2

.Lhigh:
    /* Set up kernel stack */
    ldr     x1, =boot_stack_top
    mov     sp, x1

    /* Call kernel_main */
    mov     x0, x19
    ldr     x2, =kernel_main
    blr     x2

.Lhang:
    wfi
    b       .Lhang

    /* Boot stack */
    .section .bss
    .align 16
boot_stack_bottom:
    .space 16384
    .globl boot_stack_top
boot_stack_top:
