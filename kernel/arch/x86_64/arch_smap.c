#include "arch.h"
#include "printk.h"
#include <stdint.h>

int arch_smap_enabled = 0;

static int
cpuid_smap_supported(void)
{
    uint32_t eax, ebx, ecx, edx;
    /* CPUID leaf 7, subleaf 0, EBX bit 20 = SMAP.
     * cpuid overwrites all four registers; declare them all as outputs so
     * the compiler does not assume EAX/ECX retain their input values.
     * "0"(7) places leaf 7 in EAX (same register as output operand 0);
     * "2"(0) places subleaf 0 in ECX (same register as output operand 2). */
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "0"(7), "2"(0)
    );
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> 20) & 1;
}

void
arch_smap_init(void)
{
    if (!cpuid_smap_supported()) {
        printk("[SMAP] WARN: not supported by CPU\n");
        return;
    }
    /* Set CR4.SMAP (bit 21 = 0x200000) */
    __asm__ volatile (
        "mov %%cr4, %%rax\n"
        "or $0x200000, %%rax\n"
        "mov %%rax, %%cr4\n"
        : : : "rax"
    );
    arch_smap_enabled = 1;
    printk("[SMAP] OK: supervisor access prevention active\n");
}
