/* waitq.c — wait queue primitive, audit item WQ.
 *
 * See waitq.h for the big picture. This file just implements the three
 * operations: wait, wake_one, wake_all.
 *
 * Implementation is a singly-linked LIFO internally (push to head,
 * pop from head), but wake_all drains the list in arrival order by
 * first reversing it into a local chain. Wake_one always pops the
 * head, which is the newest waiter — strictly this is LIFO, not FIFO.
 *
 * For our current workloads (single reader per wait point in most
 * subsystems, plus epoll which wakes everyone anyway) LIFO-vs-FIFO is
 * irrelevant. If a future caller needs FIFO, insert at tail with a
 * cached tail pointer — the struct layout already supports it.
 */
#include "waitq.h"
#include "sched.h"
#include "spinlock.h"
#include <stddef.h>

void
waitq_wait(waitq_t *wq)
{
    aegis_task_t *self = sched_current();

    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    /* Push self onto the head of the list. */
    self->wq_next = wq->head;
    wq->head      = self;
    spin_unlock_irqrestore(&wq->lock, fl);

    /* sched_block acquires sched_lock internally; safe because we are
     * NOT holding it (wq->lock is released above). On wake, the waker
     * already unlinked us from wq via waitq_wake_one / waitq_wake_all,
     * so wq_next is back to NULL by the time we resume. */
    sched_block();
}

void
waitq_wake_one(waitq_t *wq)
{
    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    aegis_task_t *t = wq->head;
    if (t) {
        wq->head   = t->wq_next;
        t->wq_next = NULL;
    }
    spin_unlock_irqrestore(&wq->lock, fl);

    /* sched_wake outside wq->lock: sched_wake acquires sched_lock and
     * we must respect the documented ordering (sched_lock is the top
     * of the hierarchy, above wq locks). */
    if (t)
        sched_wake(t);
}

void
waitq_wake_all(waitq_t *wq)
{
    irqflags_t fl = spin_lock_irqsave(&wq->lock);
    aegis_task_t *t = wq->head;
    wq->head = NULL;
    spin_unlock_irqrestore(&wq->lock, fl);

    while (t) {
        aegis_task_t *next = t->wq_next;
        t->wq_next = NULL;
        sched_wake(t);
        t = next;
    }
}
