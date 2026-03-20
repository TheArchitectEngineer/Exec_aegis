#include "gdt.h"
#include "tss.h"
#include "printk.h"
#include <stdint.h>

/* 7 slots: null + kernel code + kernel data + user data + user code + TSS(low+high) */
#define GDT_ENTRIES 7

typedef struct {
	uint16_t limit_lo;
	uint16_t base_lo;
	uint8_t  base_mi;
	uint8_t  access;
	uint8_t  gran;
	uint8_t  base_hi;
} __attribute__((packed)) gdt_desc_t;

typedef struct {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed, aligned(2))) gdtr_t;

static gdt_desc_t s_gdt[GDT_ENTRIES];

/*
 * 64-bit code descriptor: L=1 (long mode), D=0.
 * access = 0x9A = P|DPL<<5|S|Type(execute/read). DPL in bits 6:5.
 * gran   = 0x20 = L=1.
 */
static gdt_desc_t
make_code64(int dpl)
{
	gdt_desc_t d = {0};
	d.access = (uint8_t)(0x9A | ((dpl & 3) << 5));
	d.gran   = 0x20;
	return d;
}

/*
 * 64-bit data descriptor.
 * access = 0x92 = P|DPL<<5|S|Type(read/write).
 * gran   = 0x00 (no L bit for data).
 */
static gdt_desc_t
make_data(int dpl)
{
	gdt_desc_t d = {0};
	d.access = (uint8_t)(0x92 | ((dpl & 3) << 5));
	return d;
}

/*
 * Install a 16-byte 64-bit TSS descriptor at GDT indices [idx, idx+1].
 * type=0x89: Present, DPL=0, system descriptor (S=0), Type=9 (64-bit TSS avail).
 */
static void
gdt_set_tss(int idx, uint64_t base, uint32_t limit)
{
	s_gdt[idx].limit_lo = (uint16_t)(limit & 0xFFFF);
	s_gdt[idx].base_lo  = (uint16_t)(base & 0xFFFF);
	s_gdt[idx].base_mi  = (uint8_t)((base >> 16) & 0xFF);
	s_gdt[idx].access   = 0x89;
	s_gdt[idx].gran     = (uint8_t)((limit >> 16) & 0x0F);
	s_gdt[idx].base_hi  = (uint8_t)((base >> 24) & 0xFF);
	/* Second 8 bytes: upper 32 bits of base, rest zeroed */
	uint32_t *hi = (uint32_t *)&s_gdt[idx + 1];
	hi[0] = (uint32_t)(base >> 32);
	hi[1] = 0;
}

void
arch_gdt_init(void)
{
	s_gdt[0] = (gdt_desc_t){0};          /* null */
	s_gdt[1] = make_code64(0);           /* 0x08 kernel code DPL=0 */
	s_gdt[2] = make_data(0);             /* 0x10 kernel data DPL=0 */
	s_gdt[3] = make_data(3);             /* 0x18 user data  DPL=3 — MUST be index 3 */
	s_gdt[4] = make_code64(3);           /* 0x20 user code  DPL=3 — MUST be index 4 */

	aegis_tss_t *tss = arch_tss_get();
	gdt_set_tss(5, (uint64_t)(uintptr_t)tss, sizeof(aegis_tss_t) - 1);

	gdtr_t gdtr;
	gdtr.limit = (uint16_t)(sizeof(s_gdt) - 1);
	gdtr.base  = (uint64_t)(uintptr_t)s_gdt;

	__asm__ volatile (
		"lgdt %0\n\t"
		/* Far return to reload CS=0x08 */
		"pushq $0x08\n\t"
		"leaq  1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		/* Reload data segment registers to kernel data (0x10) */
		"movw $0x10, %%ax\n\t"
		"movw %%ax,  %%ds\n\t"
		"movw %%ax,  %%es\n\t"
		"movw %%ax,  %%fs\n\t"
		"movw %%ax,  %%gs\n\t"
		"movw %%ax,  %%ss\n\t"
		/* Load TSS selector */
		"movw $0x28, %%ax\n\t"
		"ltr  %%ax\n\t"
		: : "m"(gdtr) : "rax", "memory"
	);

	printk("[GDT] OK: ring 3 descriptors installed\n");
}
