#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096UL

/* Initialise the physical memory manager.
 * Requires arch_mm_init() to have been called first.
 * Prints [PMM] OK on success; panics on failure. */
void pmm_init(void);

/* Allocate one 4KB physical page.
 * Returns the physical address of the page, or 0 on OOM.
 * Address 0 is always reserved, so 0 is unambiguous as an error sentinel.
 *
 * NOTE: single-page (4KB) allocation only. Multi-page contiguous allocation
 * is deferred to a future buddy-allocator upgrade. The upgrade will replace
 * the internals of pmm.c without changing this header. */
uint64_t pmm_alloc_page(void);

/* Free a page previously returned by pmm_alloc_page().
 * addr must be PAGE_SIZE-aligned. Panics on double-free or bad address. */
void pmm_free_page(uint64_t addr);

/* pmm_total_pages — return total managed physical pages. */
uint64_t pmm_total_pages(void);

/* pmm_free_pages — return count of currently free physical pages.
 * Scans the bitmap; O(n) where n = PMM_MAX_PAGES/8. */
uint64_t pmm_free_pages(void);

#endif /* PMM_H */
