/* smp.c — SMP initialization and per-CPU data */
#include "smp.h"
#include "arch.h"
#include "../core/printk.h"

percpu_t g_percpu[MAX_CPUS];
uint32_t g_cpu_count = 1;

void
smp_percpu_init_bsp(void)
{
    percpu_t *bsp = &g_percpu[0];
    __builtin_memset(bsp, 0, sizeof(*bsp));
    bsp->self     = bsp;
    bsp->cpu_id   = 0;
    bsp->lapic_id = 0;

    arch_set_gs_base((uint64_t)(uintptr_t)bsp);
    arch_write_kernel_gs_base((uint64_t)(uintptr_t)bsp);

    printk("[SMP] OK: per-CPU data initialized\n");
}
