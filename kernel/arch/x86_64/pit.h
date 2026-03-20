#ifndef AEGIS_PIT_H
#define AEGIS_PIT_H

#include <stdint.h>

/* Program PIT channel 0 at 100 Hz. Unmasks IRQ0.
 * Prints [PIT] OK: timer at 100 Hz. */
void pit_init(void);

/* Called by isr_dispatch on vector 0x20 (after EOI is sent).
 * Increments tick counter, then calls sched_tick(). */
void pit_handler(void);

#endif /* AEGIS_PIT_H */
