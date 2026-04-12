/*
 * gic_v3.c — GICv3 (Generic Interrupt Controller v3) driver.
 *
 * Used by:
 *   - QEMU virt with `-machine virt,gic-version=3` (distributor 0x08000000,
 *     redistributor 0x080A0000)
 *   - Raspberry Pi 5 / BCM2712 (GIC-600)
 *
 * GICv3 replaces the GICv2 CPU interface MMIO with system registers
 * (ICC_*) and introduces a per-CPU redistributor block that must be
 * woken before interrupts work. The distributor is still MMIO, but
 * register layout differs slightly (e.g. affinity routing via GICD_CTLR
 * ARE bit + GICD_IROUTER for SPIs).
 *
 * This driver only supports single-CPU operation for now: the BSP
 * wakes redistributor 0, configures SGI/PPIs there, and routes all
 * SPIs to affinity 0 via GICD_IROUTER. SMP is deferred to phase B3.
 *
 * Diagnostics: every spinloop in this file is bounded. On timeout we
 * print a [GICv3] TIMEOUT line with the offending register and bail
 * cleanly so the user has something actionable on bare metal instead
 * of a silent hang after [KVA] OK. Per-step traces are gated on
 * GICV3_DEBUG; flip the macro to 0 once silicon boot is confirmed.
 *
 * See ARM64.md §17.2 for the design rationale and the plan.
 * Spec reference: ARM IHI 0069 (GICv3/v4 architecture).
 */

#include "printk.h"
#include <stdint.h>

/* ---------------------------------------------------------------------
 * Diagnostic macros.
 *
 * GICV3_DEBUG=1 enables the per-step "[GICv3] step N:" traces and the
 * register sanity prints. Set to 0 to suppress them once Pi 5 boot is
 * confirmed. TIMEOUT prints are unconditional — they only fire on
 * failure and are exactly the lines the user needs to debug silicon.
 * --------------------------------------------------------------------- */

#define GICV3_DEBUG 1

#if GICV3_DEBUG
#define GICV3_TRACE(...) printk(__VA_ARGS__)
#else
#define GICV3_TRACE(...) ((void)0)
#endif

/* Bound for every spinloop in this file. ~1M iterations is on the order
 * of a few milliseconds on modern aarch64 hardware (and well under 100ms
 * even on the slowest Cortex-A class core), so any wait that exceeds it
 * is genuinely stuck rather than slow. */
#define GICV3_SPIN_MAX  1000000UL

/* ---------------------------------------------------------------------
 * MMIO accessors — the caller passes the kernel-VA-mapped base; we
 * index into it with byte offsets.
 * --------------------------------------------------------------------- */

static inline uint32_t mmio_r32(uint64_t base, uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void mmio_w32(uint64_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}
static inline void mmio_w64(uint64_t base, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)(uintptr_t)(base + off) = val;
}

/* ---------------------------------------------------------------------
 * GICD (distributor) register offsets — shared with GICv2 but with
 * ARE (affinity routing enable) + IROUTER for SPI routing.
 * --------------------------------------------------------------------- */

#define GICD_CTLR               0x0000
#define GICD_TYPER              0x0004
#define GICD_IIDR               0x0008
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + 4 * (n))
#define GICD_ICFGR(n)           (0x0C00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))   /* 64-bit, one per SPI */

/* GICD_TYPER bits */
#define GICD_TYPER_ITLines_MASK 0x1F
#define GICD_TYPER_LPIS         (1U << 17)

/* GICD_CTLR (secure/non-secure banked; we run NS EL1 on QEMU virt) */
#define GICD_CTLR_EnableGrp1NS  (1U << 1)
#define GICD_CTLR_ARE_NS        (1U << 4)
#define GICD_CTLR_RWP           (1U << 31)

/* ---------------------------------------------------------------------
 * GICR (redistributor) register offsets.
 *
 * Each redistributor is 128 KiB: an "RD" frame at offset 0 and an
 * "SGI" frame at offset 0x10000. The redistributors are laid out in
 * a contiguous block; on QEMU virt at gic-version=3 the block starts
 * at 0x080A0000 and each redistributor is 128 KiB stride. For a
 * single-core boot we only touch redistributor 0.
 * --------------------------------------------------------------------- */

#define GICR_CTLR               0x0000
#define GICR_TYPER              0x0008
#define GICR_WAKER              0x0014

#define GICR_WAKER_ProcessorSleep   (1U << 1)
#define GICR_WAKER_ChildrenAsleep   (1U << 2)

/* SGI/PPI frame = redistributor base + 0x10000 */
#define GICR_SGI_OFFSET         0x10000
#define GICR_IGROUPR0           0x0080   /* within SGI frame */
#define GICR_ISENABLER0         0x0100
#define GICR_ICENABLER0         0x0180
#define GICR_ICPENDR0           0x0280
#define GICR_IPRIORITYR(n)      (0x0400 + 4 * (n))
#define GICR_ICFGR0             0x0C00   /* SGI config (read-only) */
#define GICR_ICFGR1             0x0C04   /* PPI config (edge/level) */

/* ---------------------------------------------------------------------
 * ICC_* system register accessors.
 *
 * GAS in the bundled aarch64-elf toolchain accepts the short names for
 * all ICC_* registers we need at the armv8.0 baseline (SRE/PMR/IGRPEN1
 * are always present; IAR1/EOIR1/BPR1 come with GICv3 system-register
 * support, which QEMU virt + Cortex-A76 advertise). Use the names for
 * readability — no Sn_m_Cx_Cy_z encodings needed.
 * --------------------------------------------------------------------- */

static inline uint64_t read_icc_sre_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(v));
    return v;
}
static inline void write_icc_sre_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_SRE_EL1, %0" : : "r"(v));
    __asm__ volatile("isb");
}
static inline void write_icc_pmr_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(v));
}
static inline void write_icc_bpr1_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(v));
}
static inline void write_icc_igrpen1_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(v));
    __asm__ volatile("isb");
}
static inline uint64_t read_icc_iar1_el1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(v));
    return v;
}
static inline void write_icc_eoir1_el1(uint64_t v)
{
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" : : "r"(v));
}

/* ---------------------------------------------------------------------
 * Driver state.
 * --------------------------------------------------------------------- */

static uint64_t s_dist_base;       /* GICD VA */
static uint64_t s_redist_base;     /* RD_base[0] VA */
static uint64_t s_sgi_base;        /* RD_base[0] + 0x10000 VA */
static uint32_t s_max_spi;         /* highest supported SPI (from GICD_TYPER) */

/* ---------------------------------------------------------------------
 * Bounded spinloops.
 *
 * Returns 0 on success, -1 on timeout. The caller must propagate the
 * failure so init bails before doing more damage. Timeout messages are
 * unconditional (raw printk, not GICV3_TRACE).
 * --------------------------------------------------------------------- */

static int
gicd_wait_rwp(void)
{
    for (unsigned long i = 0; i < GICV3_SPIN_MAX; i++) {
        if (!(mmio_r32(s_dist_base, GICD_CTLR) & GICD_CTLR_RWP))
            return 0;
        __asm__ volatile("yield");
    }
    printk("[GICv3] TIMEOUT: gicd_wait_rwp GICD_CTLR=0x%x after %lu iters "
           "(dist_base=0x%lx, expected RWP=0)\n",
           mmio_r32(s_dist_base, GICD_CTLR),
           GICV3_SPIN_MAX,
           (unsigned long)s_dist_base);
    return -1;
}

/* Wake redistributor 0 for the BSP. Returns 0 on success, -1 on timeout. */
static int
gicr_wake_bsp(void)
{
    uint32_t waker = mmio_r32(s_redist_base, GICR_WAKER);
    waker &= ~GICR_WAKER_ProcessorSleep;
    mmio_w32(s_redist_base, GICR_WAKER, waker);

    for (unsigned long i = 0; i < GICV3_SPIN_MAX; i++) {
        if (!(mmio_r32(s_redist_base, GICR_WAKER) & GICR_WAKER_ChildrenAsleep))
            return 0;
        __asm__ volatile("yield");
    }
    printk("[GICv3] TIMEOUT: gicr_wake_bsp GICR_WAKER=0x%x after %lu iters "
           "(redist_base=0x%lx, expected ChildrenAsleep=0)\n",
           mmio_r32(s_redist_base, GICR_WAKER),
           GICV3_SPIN_MAX,
           (unsigned long)s_redist_base);
    return -1;
}

/* ---------------------------------------------------------------------
 * Public API — matches gic.c signatures so the dispatcher can route
 * identically for v2/v3. gic_v3_init now returns int so the dispatcher
 * can suppress the success line on timeout.
 * --------------------------------------------------------------------- */

int
gic_v3_init(uint64_t dist_base, uint64_t redist_base)
{
    s_dist_base   = dist_base;
    s_redist_base = redist_base;
    s_sgi_base    = redist_base + GICR_SGI_OFFSET;

    /* Read GICD_TYPER.ITLinesNumber (bits [4:0]) to size the SPI loop.
     * ITLinesNumber N => supported INTIDs = 32 * (N + 1). */
    uint32_t typer = mmio_r32(s_dist_base, GICD_TYPER);
    uint32_t it_lines = (typer & GICD_TYPER_ITLines_MASK) + 1;
    s_max_spi = (it_lines * 32) - 1;
    if (s_max_spi > 1019) s_max_spi = 1019;

    GICV3_TRACE("[GICv3]   GICD_TYPER=0x%x SPIs=%u LPIs=%s\n",
                typer,
                it_lines * 32,
                (typer & GICD_TYPER_LPIS) ? "yes" : "no");

    uint32_t rtyper_lo = mmio_r32(s_redist_base, GICR_TYPER);
    GICV3_TRACE("[GICv3]   GICR_TYPER lo=0x%x\n", rtyper_lo);

    GICV3_TRACE("[GICv3] step 1: disable distributor\n");
    /* 1. Disable the distributor before touching its registers. */
    mmio_w32(s_dist_base, GICD_CTLR, 0);

    GICV3_TRACE("[GICv3] step 2: wait GICD_CTLR RWP\n");
    if (gicd_wait_rwp() != 0)
        return -1;

    GICV3_TRACE("[GICv3] step 3: configure SPIs (max_spi=%u)\n", s_max_spi);
    /* 3. SPI configuration (INTIDs >= 32):
     *    - all group 1 non-secure
     *    - priority 0xA0 (middle)
     *    - level-sensitive (ICFGR = 0 for each 2-bit field)
     *    - routed to affinity 0 (aff3.2.1.0 = 0) via IROUTER
     *
     * Skip the SGI/PPI range [0..31] — those live in the redistributor's
     * SGI frame and are configured separately.
     */
    for (uint32_t i = 32; i <= s_max_spi; i += 32) {
        mmio_w32(s_dist_base, GICD_IGROUPR(i / 32), 0xFFFFFFFFU);
        mmio_w32(s_dist_base, GICD_ICENABLER(i / 32), 0xFFFFFFFFU);
        mmio_w32(s_dist_base, GICD_ICPENDR(i / 32), 0xFFFFFFFFU);
    }
    for (uint32_t i = 32; i <= s_max_spi; i += 4) {
        mmio_w32(s_dist_base, GICD_IPRIORITYR(i / 4), 0xA0A0A0A0U);
    }
    for (uint32_t i = 32; i <= s_max_spi; i += 16) {
        mmio_w32(s_dist_base, GICD_ICFGR(i / 16), 0);
    }
    for (uint32_t i = 32; i <= s_max_spi; i++) {
        /* Route to affinity 0 (CPU 0). Bit 31 (IRM) = 0 → use explicit
         * aff3.aff2.aff1.aff0. */
        mmio_w64(s_dist_base, GICD_IROUTER(i), 0);
    }

    GICV3_TRACE("[GICv3] step 4: enable distributor (ARE_NS|Grp1NS)\n");
    /* 4. Enable distributor with ARE_NS + Group 1 NS. ARE is required
     *    in GICv3; GICD_ITARGETSR is RES0 once ARE is set. */
    mmio_w32(s_dist_base, GICD_CTLR,
             GICD_CTLR_ARE_NS | GICD_CTLR_EnableGrp1NS);
    if (gicd_wait_rwp() != 0)
        return -1;

    GICV3_TRACE("[GICv3] step 5: wake redistributor at 0x%lx\n",
                (unsigned long)redist_base);
    /* 5. Wake this CPU's redistributor. Must happen before any ICC_*
     *    system register traps will be allowed to function. */
    if (gicr_wake_bsp() != 0)
        return -1;

    GICV3_TRACE("[GICv3] step 6: configure SGI/PPI frame\n");
    /* 6. SGI/PPI configuration in the redistributor SGI frame:
     *    - all group 1 NS
     *    - priority 0xA0
     *    - disable everything initially; individual enables come from
     *      gic_v3_enable_irq() / gic_v3_timer_init().
     */
    mmio_w32(s_sgi_base, GICR_IGROUPR0, 0xFFFFFFFFU);
    mmio_w32(s_sgi_base, GICR_ICENABLER0, 0xFFFFFFFFU);
    mmio_w32(s_sgi_base, GICR_ICPENDR0, 0xFFFFFFFFU);
    for (uint32_t i = 0; i < 32; i += 4) {
        mmio_w32(s_sgi_base, GICR_IPRIORITYR(i / 4), 0xA0A0A0A0U);
    }
    /* Leave ICFGR1 (PPI config) as default — timer PPI 30 is set up
     * in gic_v3_timer_init(). */

    GICV3_TRACE("[GICv3] step 7: enable system registers (ICC_SRE_EL1)\n");
    /* 7. Enable system-register access. SRE=1, DIB=1 (disable IRQ
     *    bypass), DFB=1 (disable FIQ bypass). */
    uint64_t sre_before = read_icc_sre_el1();
    GICV3_TRACE("[GICv3]   ICC_SRE_EL1 before = 0x%lx\n",
                (unsigned long)sre_before);
    write_icc_sre_el1(0x7);
    uint64_t sre_after = read_icc_sre_el1();
    GICV3_TRACE("[GICv3]   ICC_SRE_EL1 after  = 0x%lx\n",
                (unsigned long)sre_after);

    GICV3_TRACE("[GICv3] step 8: set PMR/BPR/IGRPEN1\n");
    /* 8. Priority mask: allow everything. */
    write_icc_pmr_el1(0xFF);

    /* 9. Binary point: no group preemption. */
    write_icc_bpr1_el1(0);

    /* 10. Enable Group 1 IRQs. */
    write_icc_igrpen1_el1(1);

    return 0;
}

void
gic_v3_enable_irq(uint32_t irq)
{
    static int first = 1;
    if (first) {
        GICV3_TRACE("[GICv3] first enable_irq: irq=%u (PPI=%u)\n",
                    irq, irq < 32 ? 1U : 0U);
        first = 0;
    }

    if (irq < 32) {
        /* SGI or PPI: configure in the redistributor SGI frame. */
        mmio_w32(s_sgi_base, GICR_ISENABLER0, 1U << irq);
    } else {
        /* SPI: configure in the distributor. Best-effort RWP wait —
         * a timeout here is non-fatal (the bit may simply already be
         * set), but we still print the diagnostic. */
        mmio_w32(s_dist_base, GICD_ISENABLER(irq / 32),
                 1U << (irq % 32));
        (void)gicd_wait_rwp();
    }
}

uint32_t
gic_v3_ack_irq(void)
{
    /* ICC_IAR1_EL1 returns INTID in bits [23:0]. */
    return (uint32_t)(read_icc_iar1_el1() & 0xFFFFFFU);
}

void
gic_v3_eoi(uint32_t irq)
{
    write_icc_eoir1_el1(irq);
}

void
gic_v3_timer_init(void)
{
    /* The non-secure EL1 physical timer (CNTP) is PPI 30 on GICv3.
     * PPIs live in the redistributor SGI frame and default to
     * level-sensitive in GICR_ICFGR1 — which is what we want for the
     * generic timer. All we need here is to enable the interrupt. */
    gic_v3_enable_irq(30);
}
