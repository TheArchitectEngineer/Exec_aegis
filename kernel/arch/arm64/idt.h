#ifndef AEGIS_IDT_H
#define AEGIS_IDT_H

#include <stdint.h>

/*
 * ARM64 cpu_state_t — matches the SAVE_ALL_EL0 frame in vectors.S.
 * 34 slots: x0-x30, sp_el0, elr_el1, spsr_el1.
 *
 * signal.c accesses fields via #ifdef __aarch64__ blocks.
 */
typedef struct cpu_state {
    uint64_t x[31];     /* x0-x30 */
    uint64_t sp_el0;    /* user SP */
    uint64_t elr;       /* return address */
    uint64_t spsr;      /* saved PSTATE */
} cpu_state_t;

/* ARM64 has no IDT/PIC/PIT — these are stubs */
static inline void idt_init(void) {}
static inline void pic_init(void) {}
static inline void pit_init(void) {}

/* isr_post_dispatch label — referenced by x86 fork code, stub on ARM64 */
extern void isr_post_dispatch(void);

/* proc_enter_user — referenced by x86 proc.c, stub on ARM64 */
extern void proc_enter_user(void);

#endif /* AEGIS_IDT_H */
