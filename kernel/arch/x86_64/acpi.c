/* acpi.c — ACPI table parser (MCFG + MADT)
 *
 * Phase 19 scope: locate MCFG to get PCIe ECAM base, locate MADT for
 * future interrupt routing. No AML interpreter. No power management.
 *
 * Phase 20 fix: phys_to_virt() now uses kva_alloc_pages + vmm_map_page
 * to access ACPI tables at any physical address, not just the first 4MB.
 * kva_init() runs before acpi_init() so this is safe.
 */
#include "acpi.h"
#include "arch.h"
#include "printk.h"
#include "vmm.h"
#include "kva.h"
#include <stdint.h>
#include <stddef.h>

uint64_t g_mcfg_base      = 0;
uint8_t  g_mcfg_start_bus = 0;
uint8_t  g_mcfg_end_bus   = 0;
int      g_madt_found     = 0;

/* -----------------------------------------------------------------------
 * Single-page KVA window for temporary ACPI table access.
 *
 * We allocate one 4KB KVA page once and remap it for each physical page
 * we need to read.  This avoids allocating a new KVA page for every table
 * access while keeping the code simple (no multi-page spanning).
 *
 * The window is only valid during acpi_init(); after that it is abandoned
 * (KVA is a bump allocator — no free path).
 * ----------------------------------------------------------------------- */
static void    *s_win_va  = NULL;   /* KVA of the window page */
static uint64_t s_win_phys = (uint64_t)-1; /* currently mapped phys page */

/* Map phys page (aligned) into the window; invalidate TLB.
 *
 * kva_alloc_pages maps the window VA to a PMM frame initially.  Before we
 * can install our own physical page we must clear that existing PTE, because
 * vmm_map_page panics on double-map.  This applies both to the initial PMM
 * frame (first call) and any subsequent remap (later calls).
 * SAFETY: s_win_va is always mapped after kva_alloc_pages (first call) or
 * after a previous vmm_map_page call (subsequent); vmm_unmap_page succeeds. */
static void
win_map(uint64_t phys_page)
{
    if (s_win_phys == phys_page)
        return;     /* already mapped — nothing to do */
    /* Clear the existing PTE (initial PMM frame or previous physical page). */
    vmm_unmap_page((uint64_t)(uintptr_t)s_win_va);
    /* SAFETY: PTE is now absent; vmm_map_page installs phys_page safely.
     * Flags 0x03 = Present|Write (kernel-only, cached). */
    vmm_map_page((uint64_t)(uintptr_t)s_win_va, phys_page, 0x03);
    s_win_phys = phys_page;
}

/* Read a byte from any physical address using the window. */
static uint8_t
phys_read8(uint64_t phys)
{
    uint64_t page   = phys & ~(uint64_t)0xFFF;
    uint64_t offset = phys &  (uint64_t)0xFFF;
    win_map(page);
    /* SAFETY: s_win_va is mapped to page via win_map(); offset is within
     * the page (< 4096); the cast to uint8_t* and dereference is safe. */
    return ((const uint8_t *)s_win_va)[offset];
}

/* Read a 4-byte little-endian uint32 from any physical address. */
static uint32_t
phys_read32(uint64_t phys)
{
    uint32_t v = 0;
    uint32_t i;
    for (i = 0; i < 4; i++)
        v |= ((uint32_t)phys_read8(phys + i)) << (i * 8);
    return v;
}

/* Read an 8-byte little-endian uint64 from any physical address. */
static uint64_t
phys_read64(uint64_t phys)
{
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < 8; i++)
        v |= ((uint64_t)phys_read8(phys + i)) << (i * 8);
    return v;
}

/* Read `len` bytes from physical address phys into dst. */
static void
phys_read_bytes(uint64_t phys, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < len; i++)
        d[i] = phys_read8(phys + i);
}

static int acpi_checksum_phys(uint64_t phys, uint32_t len)
{
    uint8_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum += phys_read8(phys + i);
    return sum == 0;
}

static void parse_mcfg(uint64_t hdr_phys)
{
    uint32_t length = phys_read32(hdr_phys + 4);
    /* MCFG header is acpi_sdt_header_t (36 bytes) + 8-byte reserved = 44
     * bytes before the first allocation entry. */
    uint64_t p   = hdr_phys + sizeof(acpi_mcfg_t);
    uint64_t end = hdr_phys + length;

    while (p + sizeof(acpi_mcfg_alloc_t) <= end) {
        uint64_t base    = phys_read64(p + 0);
        uint16_t segment = (uint16_t)phys_read32(p + 8);   /* only 16 bits */
        uint8_t  sbus    = phys_read8(p + 10);
        uint8_t  ebus    = phys_read8(p + 11);

        if (segment == 0 && g_mcfg_base == 0) {
            g_mcfg_base      = base;
            g_mcfg_start_bus = sbus;
            g_mcfg_end_bus   = ebus;
        }
        p += sizeof(acpi_mcfg_alloc_t);
    }
}

static void scan_table(uint64_t phys)
{
    char sig[4];
    uint32_t length;

    if (phys == 0)
        return;

    /* Read signature (4 bytes) */
    phys_read_bytes(phys, sig, 4);

    /* Read length field at offset 4 */
    length = phys_read32(phys + 4);
    if (length < 36 || length > 65536)
        return;   /* sanity check */

    if (!acpi_checksum_phys(phys, length))
        return;

    if (__builtin_memcmp(sig, "MCFG", 4) == 0)
        parse_mcfg(phys);
    else if (__builtin_memcmp(sig, "APIC", 4) == 0)
        g_madt_found = 1;
}

void acpi_init(void)
{
    uint64_t rsdp_phys = arch_get_rsdp_phys();

    if (rsdp_phys == 0) {
        printk("[ACPI] OK: MADT parsed, no MCFG (legacy machine)\n");
        return;
    }

    /* Allocate a single KVA window page used for all physical reads.
     * SAFETY: kva_alloc_pages(1) returns a valid kernel VA backed by a PMM
     * page; we immediately remap it via vmm_map_page for each physical page
     * we need to access.  The page is abandoned after acpi_init returns
     * (bump allocator — no free path).  This is one leaked PMM frame; at
     * one-time ACPI init cost this is acceptable. */
    s_win_va   = kva_alloc_pages(1);
    s_win_phys = (uint64_t)-1;

    {
        char rsdp_sig[8];
        uint8_t  rsdp_rev;
        uint32_t xsdt_lo, xsdt_hi;
        uint64_t xsdt_phys;
        uint32_t rsdt_phys32;

        phys_read_bytes(rsdp_phys, rsdp_sig, 8);
        if (__builtin_memcmp(rsdp_sig, "RSD PTR ", 8) != 0) {
            printk("[ACPI] FAIL: invalid RSDP signature\n");
            return;
        }

        rsdp_rev = phys_read8(rsdp_phys + 15);   /* revision field */

        if (rsdp_rev >= 2) {
            /* ACPI 2.0+: XSDT address at offset 24 (8 bytes) */
            xsdt_lo  = phys_read32(rsdp_phys + 24);
            xsdt_hi  = phys_read32(rsdp_phys + 28);
            xsdt_phys = ((uint64_t)xsdt_hi << 32) | xsdt_lo;

            if (xsdt_phys != 0) {
                uint32_t xsdt_len = phys_read32(xsdt_phys + 4);
                uint32_t count    = 0;
                uint64_t ep;

                if (xsdt_len >= 36)
                    count = (xsdt_len - 36) / 8;

                ep = xsdt_phys + 36;   /* entries start after SDT header */
                {
                    uint32_t i;
                    for (i = 0; i < count; i++) {
                        uint64_t entry = phys_read64(ep);
                        scan_table(entry);
                        ep += 8;
                    }
                }
            }
        } else {
            /* ACPI 1.0: RSDT address at offset 16 (4 bytes) */
            rsdt_phys32 = phys_read32(rsdp_phys + 16);

            if (rsdt_phys32 != 0) {
                uint64_t rsdt_phys = (uint64_t)rsdt_phys32;
                uint32_t rsdt_len  = phys_read32(rsdt_phys + 4);
                uint32_t count     = 0;
                uint64_t ep;

                if (rsdt_len >= 36)
                    count = (rsdt_len - 36) / 4;

                ep = rsdt_phys + 36;
                {
                    uint32_t i;
                    for (i = 0; i < count; i++) {
                        uint64_t entry = (uint64_t)phys_read32(ep);
                        scan_table(entry);
                        ep += 4;
                    }
                }
            }
        }
    }

    if (g_mcfg_base != 0)
        printk("[ACPI] OK: MCFG+MADT parsed\n");
    else
        printk("[ACPI] OK: MADT parsed, no MCFG (legacy machine)\n");
}
