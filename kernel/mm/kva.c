#include "kva.h"
#include "vmm.h"
#include "pmm.h"
#include "printk.h"
#include "arch.h"
#include "spinlock.h"
#include "fb.h"
#include <stdint.h>
#include <stddef.h>

/* KVA_BASE: start of the bump-allocated kernel VA range.
 * Each architecture defines ARCH_KVA_BASE in arch.h to place this past
 * the kernel image, window allocator, and any other fixed-VA regions.
 * x86-64: pd_hi[4] = VIRT_BASE + 0x800000
 * ARM64:  L2[5]    = VIRT_BASE + 0xA00000 */
#ifdef ARCH_KVA_BASE
#define KVA_BASE ARCH_KVA_BASE
#else
#define KVA_BASE (ARCH_KERNEL_VIRT_BASE + 0x800000UL)
#endif

static uint64_t s_kva_next;
static spinlock_t kva_lock = SPINLOCK_INIT;

/* ---- VA freelist: recycle freed kva ranges ----
 * Best-fit search over a fixed-size array.  Freed VA ranges are inserted;
 * kva_alloc_pages checks the freelist before bumping s_kva_next.
 * Adjacent entries are coalesced on insert. */
#define KVA_FREE_MAX 128

typedef struct {
    uint64_t va;
    uint64_t npages;
} kva_free_t;

static kva_free_t s_free[KVA_FREE_MAX];
static int        s_nfree;

void
kva_init(void)
{
    s_kva_next = KVA_BASE;
    s_nfree = 0;
    printk("[KVA] OK: kernel virtual allocator active\n");
}

/* Try to allocate from the freelist.  Returns VA or 0 on miss. */
static uint64_t
freelist_alloc(uint64_t n)
{
    int best = -1;
    uint64_t best_size = (uint64_t)-1;
    for (int i = 0; i < s_nfree; i++) {
        if (s_free[i].npages >= n && s_free[i].npages < best_size) {
            best = i;
            best_size = s_free[i].npages;
            if (best_size == n) break;  /* exact fit */
        }
    }
    if (best < 0) return 0;

    uint64_t va = s_free[best].va;
    if (s_free[best].npages == n) {
        /* Exact fit — remove entry */
        s_free[best] = s_free[--s_nfree];
    } else {
        /* Split — shrink entry */
        s_free[best].va     += n * 4096UL;
        s_free[best].npages -= n;
    }
    return va;
}

/* Insert a freed range, coalescing with neighbors. */
static void
freelist_insert(uint64_t va, uint64_t n)
{
    /* Try coalescing with existing entries */
    for (int i = 0; i < s_nfree; i++) {
        uint64_t end = s_free[i].va + s_free[i].npages * 4096UL;
        if (end == va) {
            /* Merge: free[i] directly precedes this range */
            s_free[i].npages += n;
            /* Check if the merged entry now touches another entry */
            uint64_t new_end = s_free[i].va + s_free[i].npages * 4096UL;
            for (int j = 0; j < s_nfree; j++) {
                if (j != i && s_free[j].va == new_end) {
                    s_free[i].npages += s_free[j].npages;
                    s_free[j] = s_free[--s_nfree];
                    break;
                }
            }
            return;
        }
        if (va + n * 4096UL == s_free[i].va) {
            /* Merge: this range directly precedes free[i] */
            s_free[i].va = va;
            s_free[i].npages += n;
            return;
        }
    }
    /* No coalescing — insert new entry */
    if (s_nfree < KVA_FREE_MAX) {
        s_free[s_nfree].va     = va;
        s_free[s_nfree].npages = n;
        s_nfree++;
    }
    /* else: freelist full, VA is leaked. Acceptable at 128 entries. */
}

void *
kva_alloc_pages(uint64_t n)
{
    if (n == 0) return NULL;

    /* Try freelist first (under kva_lock), fall back to bump. */
    irqflags_t fl = spin_lock_irqsave(&kva_lock);
    uint64_t base = freelist_alloc(n);
    if (!base) {
        base = s_kva_next;
        s_kva_next += n * 4096UL;
    }
    spin_unlock_irqrestore(&kva_lock, fl);

    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            printk("[KVA] FAIL: PMM exhausted (kva_next=0x%x, free=%u)\n",
                   (unsigned)(base >> 12), (unsigned)pmm_free_pages());
            panic_halt("[KVA] FAIL: out of memory");
        }
        vmm_map_page(base + i * 4096UL, phys, VMM_FLAG_WRITABLE);
    }
    return (void *)base;
}

void *
kva_map_phys_pages(uint64_t phys_base, uint32_t num_pages)
{
    if (num_pages == 0) return NULL;

    irqflags_t fl = spin_lock_irqsave(&kva_lock);
    uint64_t base = s_kva_next;
    s_kva_next += (uint64_t)num_pages * 4096UL;
    spin_unlock_irqrestore(&kva_lock, fl);

    uint32_t i;
    for (i = 0; i < num_pages; i++) {
        vmm_map_page(base + (uint64_t)i * 4096UL,
                     phys_base + (uint64_t)i * 4096UL,
                     VMM_FLAG_WRITABLE);
    }
    return (void *)base;
}

uint64_t
kva_page_phys(void *va)
{
    return vmm_phys_of((uint64_t)(uintptr_t)va);
}

void
kva_free_pages(void *va, uint64_t n)
{
    uint64_t addr = (uint64_t)(uintptr_t)va;
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint64_t page_va = addr + i * 4096UL;
        uint64_t phys    = vmm_phys_of(page_va);
        vmm_unmap_page(page_va);
        pmm_free_page(phys);
    }

    /* Return VA range to freelist for reuse */
    irqflags_t fl = spin_lock_irqsave(&kva_lock);
    freelist_insert(addr, n);
    spin_unlock_irqrestore(&kva_lock, fl);
}
