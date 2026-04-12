/*
 * arch_mm.c — ARM64 memory map + GIC discovery from DTB.
 *
 * Parses the flattened device tree blob (FDT) to discover:
 *   - /memory@*           → usable RAM regions
 *   - /reserved-memory/   → firmware reserved ranges (e.g. BL31 on Pi 5)
 *   - GIC interrupt-controller node → routed to gic_set_version()
 *
 * The walker is a single recursive pass that maintains a small per-depth
 * stack so each node remembers:
 *
 *   - its parent's #address-cells / #size-cells (used to decode our reg)
 *   - its own #address-cells / #size-cells (used to decode CHILDREN regs)
 *   - the `ranges` translation up into the parent bus (a single entry is
 *     sufficient for QEMU virt and Pi 5 — both SoCs use one contiguous
 *     child→parent mapping for their device bus)
 *
 * Translating a local address to an absolute physical address walks the
 * stack from the current depth toward root, adding each level's ranges
 * offset in turn. This is the minimum subset of DT parsing necessary to
 * correctly locate the Pi 5 GIC-400 at 0x10_7fff_9000 even though its
 * DT reg is the SoC-local 0x7fff_9000.
 */

#include "arch.h"
#include <stdint.h>

#define MAX_MEM_REGIONS 8
#define MAX_RESERVED    8

/* DTB staging buffer — the live DTB sits in device memory (slow to
 * read byte-by-byte) and, on Pi 5, is ~80 KiB. We stage it into .bss
 * so the walker can rip through it at normal-memory speed without
 * eating the 16 KiB boot stack. 128 KiB gives comfortable headroom
 * for any production ARM64 DTB. */
#define DTB_COPY_CAP 131072

static aegis_mem_region_t s_regions[MAX_MEM_REGIONS];
static uint32_t           s_region_count;
static aegis_mem_region_t s_reserved[MAX_RESERVED];
static uint32_t           s_reserved_count;
static uint8_t            s_dtb_copy[DTB_COPY_CAP];

/* Forward decl — from gic.c */
void gic_set_version(int version, uint64_t dist_pa, uint64_t redist_or_cpu_pa);

/* ── endian helpers ────────────────────────────────────────────────── */

static inline uint32_t be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}
static inline uint64_t be64(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint64_t)be32(b) << 32) | (uint64_t)be32((const uint8_t *)p + 4);
}

/* Read a variable-width big-endian cell count (1..4 cells = 4..16 bytes)
 * from *p, advance *p by the consumed bytes, and return it as a 64-bit
 * unsigned. Only 1 or 2 cells are used in practice; we clamp defensively. */
static uint64_t
read_cells(const uint8_t **p, uint32_t cells)
{
    uint64_t v = 0;
    if (cells > 4) cells = 4;
    for (uint32_t i = 0; i < cells; i++) {
        v = (v << 32) | (uint64_t)be32(*p);
        *p += 4;
    }
    return v;
}

/* ── FDT tokens ────────────────────────────────────────────────────── */

#define FDT_MAGIC      0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

/* ── string helpers ────────────────────────────────────────────────── */

static int
streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int
starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

/* Returns 1 if the compatible string list (sequence of NUL-terminated
 * strings packed end-to-end) contains an exact match for `needle`. */
static int
compat_contains(const char *list, uint32_t list_len, const char *needle)
{
    uint32_t i = 0;
    while (i < list_len) {
        const char *s = list + i;
        uint32_t slen = 0;
        while (i + slen < list_len && s[slen]) slen++;
        if (streq(s, needle)) return 1;
        i += slen + 1;
    }
    return 0;
}

/* ── per-depth stack ───────────────────────────────────────────────── */

/* A DTB-walker frame. We keep one per depth level, initialised when we
 * descend via FDT_BEGIN_NODE and rolled back on FDT_END_NODE.
 *
 * - parent_addr_cells / parent_size_cells: inherited from the enclosing
 *   node; used to decode THIS node's `reg` property.
 * - addr_cells / size_cells: the #*-cells declared ON this node (default
 *   2/1 per the DT spec); used to decode children's `reg` properties.
 * - has_ranges + range_child_addr + range_parent_addr + range_size: the
 *   first entry from this node's `ranges` property, normalised to u64s.
 *   One entry is enough for QEMU virt (root identity ranges) and Pi 5
 *   (soc has a single contiguous child→parent window).
 *
 * Name is used to recognise /memory, /reserved-memory and the per-node
 * cells lookup. */
#define MAX_DEPTH 16

typedef struct {
    const char *name;           /* node name (pointer into dtb_copy) */
    uint32_t    addr_cells;     /* #address-cells for CHILDREN of this node */
    uint32_t    size_cells;     /* #size-cells for CHILDREN of this node */
    uint32_t    parent_addr_cells;
    uint32_t    parent_size_cells;

    int         has_ranges;     /* 1 if ranges property was parsed */
    uint64_t    range_child_addr;
    uint64_t    range_parent_addr;
    uint64_t    range_size;
} dt_frame_t;

/* Walk `stack[0..depth]` and translate a local PA declared at depth
 * `depth` into an absolute PA by applying each ancestor's ranges.
 *
 * The ranges property on node N maps child addresses (seen by N's
 * children, i.e. nodes at depth N+1) into N's own parent coordinate
 * space. So to translate addr from depth D up to absolute, we start at
 * depth D-1 (the parent node whose ranges apply to D's children) and
 * walk upward. */
static uint64_t
translate_address(const dt_frame_t *stack, int depth, uint64_t addr)
{
    /* Stack index of the *parent* node of the reg-declaring node. */
    for (int d = depth - 1; d >= 1; d--) {
        const dt_frame_t *f = &stack[d];
        if (!f->has_ranges) continue;

        /* Empty `ranges;` (no entries) means identity — skip. */
        if (f->range_size == 0 && f->range_child_addr == 0 &&
            f->range_parent_addr == 0) {
            continue;
        }

        if (addr >= f->range_child_addr &&
            addr <  f->range_child_addr + f->range_size) {
            addr = (addr - f->range_child_addr) + f->range_parent_addr;
        }
    }
    return addr;
}

/* ── main walker ───────────────────────────────────────────────────── */

void
arch_mm_init(void *dtb)
{
    s_region_count = 0;
    s_reserved_count = 0;

    const uint8_t *base = (const uint8_t *)dtb;
    if (!base || be32(base) != FDT_MAGIC) {
        /* No valid DTB — fall back to 128 MiB at 0x40000000. */
        s_regions[0].base = 0x40000000UL;
        s_regions[0].len  = 128UL * 1024 * 1024;
        s_region_count = 1;
        s_reserved[0].base = 0;
        s_reserved[0].len  = 0x40000000UL;
        s_reserved_count = 1;
        return;
    }

    /* Copy the struct + strings blocks out of firmware/device memory
     * into s_dtb_copy (.bss — see DTB_COPY_CAP above). Clamp at the
     * static buffer size; on Pi 5 the full DTB is ~80 KiB, QEMU virt's
     * is ~1 MiB but the live part is <8 KiB. */
    uint32_t totalsize = be32(base + 4);
    if (totalsize > DTB_COPY_CAP) totalsize = DTB_COPY_CAP;

    {
        const volatile uint8_t *src = (const volatile uint8_t *)base;
        for (uint32_t ci = 0; ci < totalsize; ci++)
            s_dtb_copy[ci] = src[ci];
    }
    const uint8_t *d = s_dtb_copy;

    uint32_t struct_off  = be32(d + 8);
    uint32_t strings_off = be32(d + 12);
    const uint8_t *structs = d + struct_off;
    const char    *strings = (const char *)(d + strings_off);
    const uint8_t *end = d + totalsize;

    dt_frame_t stack[MAX_DEPTH];

    /* Depth 0 is the synthetic "above root" frame. The DT spec says
     * the root inherits #address-cells=2 / #size-cells=1 by default,
     * but every ARM64 DTB we care about (QEMU virt, Pi 5) sets its own
     * values on the root node, so this just provides a sane starting
     * point for the stack. */
    stack[0].name              = "";
    stack[0].addr_cells        = 2;
    stack[0].size_cells        = 1;
    stack[0].parent_addr_cells = 2;
    stack[0].parent_size_cells = 1;
    stack[0].has_ranges        = 0;
    stack[0].range_child_addr  = 0;
    stack[0].range_parent_addr = 0;
    stack[0].range_size        = 0;

    int depth = 0;

    /* Per-node scratch — collected from FDT_PROP tokens as we scan the
     * current node's properties, committed/acted on at FDT_END_NODE. */
    const char     *compat_data = 0;
    uint32_t        compat_len  = 0;
    const uint8_t  *reg_data    = 0;
    uint32_t        reg_len     = 0;

    const uint8_t *p = structs;
    while (p < end) {
        if (p + 4 > end) break;
        uint32_t tok = be32(p); p += 4;

        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            uint32_t len = 0;
            while ((const uint8_t *)&name[len] < end && name[len]) len++;
            p += (len + 4) & ~3U;

            if (depth + 1 >= MAX_DEPTH) {
                /* Too deep — still track depth so FDT_END_NODE
                 * bookkeeping stays balanced, but don't clobber the
                 * last valid frame. */
                depth++;
                break;
            }
            depth++;

            dt_frame_t *parent = &stack[depth - 1];
            dt_frame_t *f      = &stack[depth];
            f->name              = name;
            /* Default per DT spec: a node that doesn't declare its own
             * #*-cells values uses 2 / 1. QEMU virt + Pi 5 both set
             * these explicitly on bus nodes, so this mostly matters
             * for the root frame initialisation. */
            f->addr_cells        = 2;
            f->size_cells        = 1;
            f->parent_addr_cells = parent->addr_cells;
            f->parent_size_cells = parent->size_cells;
            f->has_ranges        = 0;
            f->range_child_addr  = 0;
            f->range_parent_addr = 0;
            f->range_size        = 0;

            /* Reset per-node property scratch. */
            compat_data = 0; compat_len = 0;
            reg_data    = 0; reg_len    = 0;
            break;
        }

        case FDT_END_NODE: {
            if (depth >= MAX_DEPTH) {
                depth--;
                break;
            }

            dt_frame_t *f      = &stack[depth];
            dt_frame_t *parent = (depth > 0) ? &stack[depth - 1] : 0;

            /* ── /memory@* → usable RAM ── */
            if (reg_data && starts_with(f->name, "memory")) {
                uint32_t ac = f->parent_addr_cells;
                uint32_t sc = f->parent_size_cells;
                uint32_t stride = 4 * (ac + sc);
                if (stride > 0) {
                    const uint8_t *cur = reg_data;
                    uint32_t consumed = 0;
                    while (consumed + stride <= reg_len &&
                           s_region_count < MAX_MEM_REGIONS) {
                        uint64_t rbase = read_cells(&cur, ac);
                        uint64_t rsize = read_cells(&cur, sc);
                        rbase = translate_address(stack, depth, rbase);
                        if (rsize > 0) {
                            s_regions[s_region_count].base = rbase;
                            s_regions[s_region_count].len  = rsize;
                            s_region_count++;
                        }
                        consumed += stride;
                    }
                }
            }

            /* ── /reserved-memory children → PMM exclusions ──
             *
             * The reserved-memory node itself has no reg. Each child
             * declares one using the reserved-memory node's
             * #address-cells / #size-cells. On Pi 5 those are 2 and 2
             * respectively, giving 4 cells (16 bytes) per entry. */
            if (reg_data && parent &&
                streq(parent->name, "reserved-memory")) {
                uint32_t ac = f->parent_addr_cells;
                uint32_t sc = f->parent_size_cells;
                uint32_t stride = 4 * (ac + sc);
                if (stride > 0) {
                    const uint8_t *cur = reg_data;
                    uint32_t consumed = 0;
                    while (consumed + stride <= reg_len &&
                           s_reserved_count < MAX_RESERVED) {
                        uint64_t rbase = read_cells(&cur, ac);
                        uint64_t rsize = read_cells(&cur, sc);
                        /* Reserved-memory entries live in the parent's
                         * coordinate space; apply ranges translation
                         * starting from reserved-memory's parent. */
                        rbase = translate_address(stack, depth, rbase);
                        if (rsize > 0) {
                            s_reserved[s_reserved_count].base = rbase;
                            s_reserved[s_reserved_count].len  = rsize;
                            s_reserved_count++;
                        }
                        consumed += stride;
                    }
                }
            }

            /* ── GIC interrupt-controller ──
             *
             * Recognise any of the GIC compatible strings we support.
             * The node can live anywhere in the tree — QEMU virt puts
             * it directly under root; Pi 5 nests it under
             * /soc@107c000000/. */
            if (compat_data && reg_data) {
                int version = 0;
                if (compat_contains(compat_data, compat_len, "arm,gic-v3") ||
                    compat_contains(compat_data, compat_len, "arm,gic-600")) {
                    version = 3;
                } else if (
                    compat_contains(compat_data, compat_len, "arm,gic-400") ||
                    compat_contains(compat_data, compat_len, "arm,cortex-a15-gic") ||
                    compat_contains(compat_data, compat_len, "arm,cortex-a7-gic")) {
                    version = 2;
                }
                if (version != 0) {
                    uint32_t ac = f->parent_addr_cells;
                    uint32_t sc = f->parent_size_cells;
                    uint32_t stride = 4 * (ac + sc);
                    if (stride > 0 && reg_len >= 2 * stride) {
                        const uint8_t *cur = reg_data;
                        uint64_t dist_pa = read_cells(&cur, ac);
                        (void)read_cells(&cur, sc); /* dist size */
                        uint64_t second_pa = read_cells(&cur, ac);
                        (void)read_cells(&cur, sc); /* second size */

                        dist_pa   = translate_address(stack, depth, dist_pa);
                        second_pa = translate_address(stack, depth, second_pa);

                        gic_set_version(version, dist_pa, second_pa);
                    }
                }
            }

            /* Clear scratch before exiting — sibling nodes must not see
             * this node's compat/reg. */
            compat_data = 0; compat_len = 0;
            reg_data    = 0; reg_len    = 0;

            depth--;
            break;
        }

        case FDT_PROP: {
            if (p + 8 > end) goto walk_done;
            uint32_t prop_len = be32(p); p += 4;
            uint32_t name_off = be32(p); p += 4;
            const uint8_t *data = p;
            p += (prop_len + 3) & ~3U;

            if (depth >= MAX_DEPTH || depth < 0) break;
            dt_frame_t *f = &stack[depth];
            const char *pname = strings + name_off;

            if (streq(pname, "#address-cells") && prop_len >= 4) {
                f->addr_cells = be32(data);
            } else if (streq(pname, "#size-cells") && prop_len >= 4) {
                f->size_cells = be32(data);
            } else if (streq(pname, "ranges")) {
                /* ranges entry layout:
                 *   child-bus-address (f->addr_cells cells)
                 *   parent-bus-address (f->parent_addr_cells cells)
                 *   size (f->size_cells cells)
                 *
                 * Empty `ranges;` (prop_len == 0) means identity. */
                if (prop_len == 0) {
                    f->has_ranges        = 1;
                    f->range_child_addr  = 0;
                    f->range_parent_addr = 0;
                    f->range_size        = 0;
                } else {
                    uint32_t need =
                        4 * (f->addr_cells + f->parent_addr_cells + f->size_cells);
                    if (need > 0 && prop_len >= need) {
                        const uint8_t *cur = data;
                        uint64_t c = read_cells(&cur, f->addr_cells);
                        uint64_t pa = read_cells(&cur, f->parent_addr_cells);
                        uint64_t sz = read_cells(&cur, f->size_cells);
                        f->has_ranges        = 1;
                        f->range_child_addr  = c;
                        f->range_parent_addr = pa;
                        f->range_size        = sz;
                    }
                }
            } else if (streq(pname, "compatible")) {
                compat_data = (const char *)data;
                compat_len  = prop_len;
            } else if (streq(pname, "reg")) {
                reg_data = data;
                reg_len  = prop_len;
            }
            break;
        }

        case FDT_NOP:
            break;
        case FDT_END:
            goto walk_done;
        default:
            goto walk_done;
        }
    }

walk_done:
    if (s_region_count == 0) {
        s_regions[0].base = 0x40000000UL;
        s_regions[0].len  = 128UL * 1024 * 1024;
        s_region_count = 1;
    }

    /* Always reserve everything below the kernel load address. On QEMU
     * virt that's 0x0..0x40000000 (the GIC/UART live there and QEMU
     * reports 0 B of RAM below 0x40000000 anyway, so this is cheap);
     * on Pi 5 that same range contains the BL31 runtime and PSCI
     * mailbox and MUST stay reserved.
     *
     * The reserved-memory parse above will also have added atf@0
     * (0x0..0x80000) on Pi 5 — that's a subset of this range, and
     * pmm_reserve_region's bitmap set is idempotent, so the double
     * cover is harmless. */
    if (s_reserved_count < MAX_RESERVED) {
        s_reserved[s_reserved_count].base = 0;
        s_reserved[s_reserved_count].len  = 0x40000000UL;
        s_reserved_count++;
    }
}

uint32_t arch_mm_region_count(void)            { return s_region_count; }
const aegis_mem_region_t *arch_mm_get_regions(void) { return s_regions; }
uint32_t arch_mm_reserved_region_count(void)   { return s_reserved_count; }
const aegis_mem_region_t *arch_mm_get_reserved_regions(void) { return s_reserved; }
