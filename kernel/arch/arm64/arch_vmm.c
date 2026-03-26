/*
 * arch_vmm.c — ARM64 virtual memory arch primitives.
 *
 * Provides arch_vmm_load_pml4 (TTBR0_EL1) and arch_vmm_invlpg (TLBI)
 * for the shared kernel/mm/vmm.c.
 */

#include <stdint.h>

void
arch_vmm_load_pml4(uint64_t phys)
{
    /* Kernel page tables go into TTBR1 (upper half).
     * TTBR0 (lower half) is switched per-process for user space. */
    __asm__ volatile(
        "msr ttbr1_el1, %0\n"
        "dsb sy\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        : : "r"(phys) : "memory"
    );
}

void
arch_vmm_load_user_ttbr0(uint64_t phys)
{
    /* User page tables go into TTBR0 (lower half). */
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "dsb sy\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        : : "r"(phys) : "memory"
    );
}

void
arch_vmm_invlpg(uint64_t virt)
{
    /* Full TLB invalidate — TLBI VAE1 has issues with TTBR1 addresses
     * (bit 43 set in the page number). Use VMALLE1 for correctness. */
    (void)virt;
    __asm__ volatile(
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        : : : "memory"
    );
}
