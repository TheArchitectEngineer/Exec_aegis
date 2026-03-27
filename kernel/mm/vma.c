/* kernel/mm/vma.c — per-process Virtual Memory Area tracking */

#include "vma.h"
#include "../proc/proc.h"
#include "kva.h"

#define PAGE_SIZE 4096u
#define VMA_CAPACITY (PAGE_SIZE / sizeof(vma_entry_t))  /* 170 */

/* ── helpers ─────────────────────────────────────────────────────────── */

static void vma_shift_right(vma_entry_t *table, uint32_t count, uint32_t idx) {
    uint32_t i = count;
    while (i > idx) {
        table[i] = table[i - 1];
        i--;
    }
}

static void vma_shift_left(vma_entry_t *table, uint32_t count, uint32_t idx) {
    uint32_t i = idx;
    while (i + 1 < count) {
        table[i] = table[i + 1];
        i++;
    }
}

static int vma_can_merge(const vma_entry_t *a, const vma_entry_t *b) {
    return a->prot == b->prot &&
           a->type == b->type &&
           (a->base + a->len) == b->base;
}

/* ── public API ──────────────────────────────────────────────────────── */

void vma_init(struct aegis_process *proc) {
    vma_entry_t *table = (vma_entry_t *)kva_alloc_pages(1);
    proc->vma_table    = table;
    proc->vma_count    = 0;
    proc->vma_capacity = (uint32_t)VMA_CAPACITY;
    proc->vma_refcount = 1;
}

void vma_insert(struct aegis_process *proc,
                uint64_t base, uint64_t len, uint32_t prot, uint8_t type) {
    vma_entry_t *table = proc->vma_table;
    uint32_t count = proc->vma_count;

    if (!table || len == 0)
        return;

    /* Find insertion point: first entry with base > our base */
    uint32_t idx = 0;
    while (idx < count && table[idx].base < base)
        idx++;

    /* Try merging with the previous entry */
    if (idx > 0 && table[idx - 1].prot == prot &&
        table[idx - 1].type == type &&
        (table[idx - 1].base + table[idx - 1].len) == base) {
        /* Extend previous entry */
        table[idx - 1].len += len;

        /* Check if we can also merge with the next entry */
        if (idx < count && vma_can_merge(&table[idx - 1], &table[idx])) {
            table[idx - 1].len += table[idx].len;
            vma_shift_left(table, count, idx);
            proc->vma_count = count - 1;
        }
        return;
    }

    /* Try merging with the next entry */
    if (idx < count && table[idx].prot == prot &&
        table[idx].type == type &&
        (base + len) == table[idx].base) {
        table[idx].base = base;
        table[idx].len += len;
        return;
    }

    /* No merge possible — insert a new entry */
    if (count >= proc->vma_capacity)
        return;  /* table full; silently drop */

    vma_shift_right(table, count, idx);
    table[idx].base = base;
    table[idx].len  = len;
    table[idx].prot = prot;
    table[idx].type = type;
    table[idx]._pad[0] = 0;
    table[idx]._pad[1] = 0;
    table[idx]._pad[2] = 0;
    proc->vma_count = count + 1;
}

void vma_remove(struct aegis_process *proc, uint64_t base, uint64_t len) {
    vma_entry_t *table = proc->vma_table;
    uint32_t count = proc->vma_count;
    uint64_t end = base + len;

    if (!table || len == 0)
        return;

    uint32_t i = 0;
    while (i < count) {
        vma_entry_t *e = &table[i];
        uint64_t e_end = e->base + e->len;

        /* No overlap */
        if (e_end <= base || e->base >= end) {
            i++;
            continue;
        }

        /* Fully contained — remove entire entry */
        if (e->base >= base && e_end <= end) {
            vma_shift_left(table, count, i);
            count--;
            continue;  /* don't increment i — next entry shifted into slot */
        }

        /* Remove range cuts the beginning of this entry */
        if (e->base >= base && e->base < end) {
            uint64_t trim = end - e->base;
            e->base += trim;
            e->len  -= trim;
            i++;
            continue;
        }

        /* Remove range cuts the end of this entry */
        if (e_end > base && e_end <= end) {
            e->len = base - e->base;
            i++;
            continue;
        }

        /* Remove range is entirely within this entry — split */
        if (e->base < base && e_end > end) {
            if (count >= proc->vma_capacity) {
                i++;
                continue;  /* no room for split; skip */
            }
            /* Create right fragment */
            vma_shift_right(table, count, i + 1);
            count++;
            table[i + 1].base = end;
            table[i + 1].len  = e_end - end;
            table[i + 1].prot = e->prot;
            table[i + 1].type = e->type;
            table[i + 1]._pad[0] = 0;
            table[i + 1]._pad[1] = 0;
            table[i + 1]._pad[2] = 0;
            /* Truncate left fragment */
            e->len = base - e->base;
            i += 2;  /* skip both fragments */
            continue;
        }

        i++;
    }

    proc->vma_count = count;
}

void vma_update_prot(struct aegis_process *proc,
                     uint64_t base, uint64_t len, uint32_t new_prot) {
    vma_entry_t *table = proc->vma_table;
    uint32_t count = proc->vma_count;
    uint64_t end = base + len;

    if (!table || len == 0)
        return;

    uint32_t i = 0;
    while (i < count) {
        vma_entry_t *e = &table[i];
        uint64_t e_end = e->base + e->len;

        /* No overlap */
        if (e_end <= base || e->base >= end) {
            i++;
            continue;
        }

        /* Already has the target permissions */
        if (e->prot == new_prot) {
            i++;
            continue;
        }

        /* Fully contained */
        if (e->base >= base && e_end <= end) {
            e->prot = new_prot;
            i++;
            continue;
        }

        /* Partial overlap at the start of this entry */
        if (e->base >= base && e->base < end && e_end > end) {
            if (count >= proc->vma_capacity) {
                i++;
                continue;
            }
            /* Split: [e->base..end) gets new_prot, [end..e_end) keeps old */
            vma_shift_right(table, count, i + 1);
            count++;
            table[i + 1].base = end;
            table[i + 1].len  = e_end - end;
            table[i + 1].prot = e->prot;
            table[i + 1].type = e->type;
            table[i + 1]._pad[0] = 0;
            table[i + 1]._pad[1] = 0;
            table[i + 1]._pad[2] = 0;
            e->len  = end - e->base;
            e->prot = new_prot;
            i += 2;
            continue;
        }

        /* Partial overlap at the end of this entry */
        if (e->base < base && e_end > base && e_end <= end) {
            if (count >= proc->vma_capacity) {
                i++;
                continue;
            }
            /* Split: [e->base..base) keeps old, [base..e_end) gets new_prot */
            vma_shift_right(table, count, i + 1);
            count++;
            table[i + 1].base = base;
            table[i + 1].len  = e_end - base;
            table[i + 1].prot = new_prot;
            table[i + 1].type = e->type;
            table[i + 1]._pad[0] = 0;
            table[i + 1]._pad[1] = 0;
            table[i + 1]._pad[2] = 0;
            e->len = base - e->base;
            i += 2;
            continue;
        }

        /* Remove range is entirely within this entry — three-way split */
        if (e->base < base && e_end > end) {
            if (count + 2 > proc->vma_capacity) {
                i++;
                continue;
            }
            uint32_t old_prot = e->prot;
            uint8_t  old_type = e->type;
            /* Insert two new entries after this one */
            vma_shift_right(table, count, i + 1);
            count++;
            vma_shift_right(table, count, i + 2);
            count++;
            /* Middle fragment: [base..end) with new_prot */
            table[i + 1].base = base;
            table[i + 1].len  = len;
            table[i + 1].prot = new_prot;
            table[i + 1].type = old_type;
            table[i + 1]._pad[0] = 0;
            table[i + 1]._pad[1] = 0;
            table[i + 1]._pad[2] = 0;
            /* Right fragment: [end..e_end) with old prot */
            table[i + 2].base = end;
            table[i + 2].len  = e_end - end;
            table[i + 2].prot = old_prot;
            table[i + 2].type = old_type;
            table[i + 2]._pad[0] = 0;
            table[i + 2]._pad[1] = 0;
            table[i + 2]._pad[2] = 0;
            /* Left fragment: [e->base..base) keeps old */
            e->len = base - e->base;
            i += 3;
            continue;
        }

        i++;
    }

    proc->vma_count = count;
}

void vma_clear(struct aegis_process *proc) {
    proc->vma_count = 0;
}

void vma_clone(struct aegis_process *dst, struct aegis_process *src) {
    vma_entry_t *new_table = (vma_entry_t *)kva_alloc_pages(1);
    uint32_t count = src->vma_count;

    if (count > 0)
        __builtin_memcpy(new_table, src->vma_table,
                         count * sizeof(vma_entry_t));

    dst->vma_table    = new_table;
    dst->vma_count    = count;
    dst->vma_capacity = (uint32_t)VMA_CAPACITY;
    dst->vma_refcount = 1;
}

void vma_share(struct aegis_process *child, struct aegis_process *parent) {
    child->vma_table    = parent->vma_table;
    child->vma_count    = parent->vma_count;
    child->vma_capacity = parent->vma_capacity;
    parent->vma_refcount++;
    child->vma_refcount = parent->vma_refcount;
}

void vma_free(struct aegis_process *proc) {
    if (!proc->vma_table)
        return;

    proc->vma_refcount--;
    if (proc->vma_refcount == 0)
        kva_free_pages(proc->vma_table, 1);

    proc->vma_table = (vma_entry_t *)0;
    proc->vma_count = 0;
}
