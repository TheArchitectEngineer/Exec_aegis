/* spinlock.h — Ticket spinlock for SMP synchronization
 *
 * Ticket lock guarantees FIFO ordering (no starvation).
 * Two 16-bit counters: 'next' (take-a-number) and 'owner' (now-serving).
 *
 * IRQ-safe variants save/restore interrupt flags via arch.h helpers
 * to prevent deadlock (ISR acquires lock already held by interrupted code).
 */
#ifndef AEGIS_SPINLOCK_H
#define AEGIS_SPINLOCK_H

#include "arch.h"

typedef struct {
    volatile uint16_t owner;
    volatile uint16_t next;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void
spin_lock(spinlock_t *lock)
{
    uint16_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != ticket)
        arch_pause();
}

static inline void
spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->owner, lock->owner + 1, __ATOMIC_RELEASE);
}

static inline int
spin_trylock(spinlock_t *lock)
{
    uint16_t owner = __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);
    uint16_t next  = __atomic_load_n(&lock->next, __ATOMIC_RELAXED);
    if (owner != next) return 0;
    return __atomic_compare_exchange_n(&lock->next, &next, (uint16_t)(next + 1),
                                       0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

typedef unsigned long irqflags_t;

static inline irqflags_t
spin_lock_irqsave(spinlock_t *lock)
{
    irqflags_t flags = arch_irq_save();
    spin_lock(lock);
    return flags;
}

static inline void
spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags)
{
    spin_unlock(lock);
    arch_irq_restore(flags);
}

#endif /* AEGIS_SPINLOCK_H */
