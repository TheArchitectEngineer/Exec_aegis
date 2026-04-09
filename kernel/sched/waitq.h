#ifndef AEGIS_WAITQ_H
#define AEGIS_WAITQ_H

/*
 * waitq_t — wait queue abstraction (audit item WQ, 2026-03-29).
 *
 * Kernel blocking I/O historically used ad-hoc per-subsystem patterns:
 *
 *     static aegis_task_t *s_waiter;   // single-waiter pointer
 *     if (s_waiter) { sched_wake(s_waiter); s_waiter = NULL; }
 *     s_waiter = sched_current(); sched_block();
 *
 * Problems with that pattern:
 *   1. Only one waiter per wait point — breaks for multi-reader scenarios.
 *   2. Easy to get the condition-check / block sequencing wrong, leading
 *      to lost wakeups (wake fires between check and sched_block).
 *   3. No FIFO ordering for multiple waiters.
 *   4. Every subsystem reinvents the memory-ordering story.
 *
 * waitq_t solves (1), (3), and (4): it is a FIFO list of tasks waiting
 * on an event, with internal locking. Multiple tasks can wait; wake_one
 * dequeues the oldest waiter; wake_all drains the list.
 *
 * The caller is still responsible for the condition-check loop, and for
 * structuring the wait so the wakeup cannot be lost. The canonical pattern:
 *
 *     for (;;) {
 *         if (condition_is_met(state))
 *             break;
 *         waitq_wait(&state->wq);
 *     }
 *
 * The producer side calls waitq_wake_one(&state->wq) or waitq_wake_all
 * AFTER arranging the condition. On single-core preemptive Aegis this
 * works because waitq_wait disables interrupts across the enqueue + block
 * window (sched_block acquires sched_lock with irqsave). On true SMP a
 * more elaborate protocol with external locks will be required — that
 * is future work and should be done when we first see multi-core wakeup
 * bugs bite.
 *
 * Migration policy: new blocking I/O paths SHOULD use waitq_t. Existing
 * subsystems (pipe, tty, socket, unix_socket, epoll, futex, usb_mouse)
 * keep their single-waiter patterns until independently audited and
 * migrated. Do NOT mass-rewrite all call sites in one commit — the risk
 * of introducing lost-wakeup regressions outweighs the cleanup benefit.
 */

#include <stddef.h>
#include "sched.h"
#include "spinlock.h"

typedef struct waitq {
    struct aegis_task_t *head;   /* FIFO head of waiting tasks (linked via wq_next) */
    spinlock_t           lock;
} waitq_t;

#define WAITQ_INIT { .head = NULL, .lock = SPINLOCK_INIT }

/* waitq_init — runtime initializer for a waitq_t embedded in a dynamically
 * allocated struct. Static waitqs should use WAITQ_INIT. */
static inline void
waitq_init(waitq_t *wq)
{
    wq->head = NULL;
    wq->lock = (spinlock_t)SPINLOCK_INIT;
}

/* waitq_wait — add the current task to wq's FIFO, then sched_block.
 *
 * Returns when some other party calls waitq_wake_one / waitq_wake_all.
 * On return, the task is no longer on wq's list and its state is
 * TASK_RUNNING.
 *
 * MUST be called from a context where sched_block is legal (not from
 * ISR, not holding sched_lock, and at least one other RUNNING task
 * exists — task_idle guarantees this).
 *
 * IMPORTANT: the caller must have already checked the wait condition
 * and determined it is NOT met. See the canonical pattern in the
 * header doc comment above. Failure to check the condition atomically
 * with the enqueue can lose wakeups. */
void waitq_wait(waitq_t *wq);

/* waitq_wake_one — remove the oldest waiter from wq and transition it
 * to TASK_RUNNING. No-op if the queue is empty.
 *
 * Safe to call from IRQ context: does not call sched_block, only
 * sched_wake (which acquires sched_lock briefly). */
void waitq_wake_one(waitq_t *wq);

/* waitq_wake_all — wake every task currently waiting on wq.
 *
 * Safe to call from IRQ context. Waiters are woken in FIFO order;
 * tasks that add themselves to wq after waitq_wake_all starts are not
 * woken by this call. */
void waitq_wake_all(waitq_t *wq);

#endif /* AEGIS_WAITQ_H */
