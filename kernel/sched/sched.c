#include "sched.h"
#include "arch.h"
#include "kva.h"
#include "pmm.h"
#include "printk.h"
#include "vmm.h"
#include "proc.h"
#include "fd_table.h"
#include "ext2.h"
#include "spinlock.h"
#include "smp.h"
#include "fb.h"
#include <stddef.h>

/* Compile-time guard: ctx_switch.asm assumes sp is at offset 0 of TCB.
 * If anyone adds a field before sp, this catches it immediately. */
_Static_assert(offsetof(aegis_task_t, sp) == 0,
    "sp must be first field in aegis_task_t — ctx_switch depends on this");

#define STACK_PAGES  4                     /* 16KB per task */
#define STACK_SIZE   (STACK_PAGES * 4096UL)

static uint32_t      s_next_tid = 0;
static uint32_t      s_task_count = 0;
static volatile int  s_sched_ready = 0;  /* set by sched_start; guards sched_tick */

spinlock_t sched_lock = SPINLOCK_INIT;

/* RUNNING-only run queue sentinel (P3 audit fix).
 *
 * Circular doubly-linked list of tasks with state == TASK_RUNNING, threaded
 * through aegis_task_t::next_run/prev_run.  The sentinel is a stable anchor:
 * the list is always non-empty (sentinel is in the list even when there are
 * no runnable tasks), so iteration and insertion never need a null check.
 * The sentinel itself is never scheduled — sched_tick et al. skip it by
 * testing `t == &s_run_sentinel`.
 *
 * All mutations (insert/remove) require sched_lock.  The atomic-release
 * state flip from the M1 audit fix is preserved: it now happens under
 * sched_lock alongside the list insert. */
static aegis_task_t s_run_sentinel = {
    .sp               = 0,
    .stack_base       = 0,
    .kernel_stack_top = 0,
    .tid              = 0xFFFFFFFFu,
    .is_user          = 0,
    .stack_pages      = 0,
    .state            = TASK_BLOCKED,   /* never picked by iteration */
    .waiting_for      = 0,
    .fs_base          = 0,
    .clear_child_tid  = 0,
    .sleep_deadline   = 0,
    .read_nonblock    = 0,
    .next             = 0,
    .next_run         = &s_run_sentinel,
    .prev_run         = &s_run_sentinel,
};

/* Insert task at the tail of the run list (just before the sentinel).
 * Idempotent: if the task is already in the list, this is a no-op.
 * Caller must hold sched_lock. */
static void
run_list_insert_locked(aegis_task_t *task)
{
    if (task->next_run != (aegis_task_t *)0)
        return;   /* already in list */
    aegis_task_t *tail = s_run_sentinel.prev_run;
    task->prev_run          = tail;
    task->next_run          = &s_run_sentinel;
    tail->next_run          = task;
    s_run_sentinel.prev_run = task;
}

/* Remove task from the run list.  Idempotent: if task is not in the list,
 * this is a no-op.  Caller must hold sched_lock. */
static void
run_list_remove_locked(aegis_task_t *task)
{
    if (task->next_run == (aegis_task_t *)0)
        return;   /* not in list */
    task->prev_run->next_run = task->next_run;
    task->next_run->prev_run = task->prev_run;
    task->next_run = (aegis_task_t *)0;
    task->prev_run = (aegis_task_t *)0;
}

/* Return the next RUNNING task after `cur` in the run list, skipping the
 * sentinel.  Returns NULL if the list contains only the sentinel (no runnable
 * tasks — should never happen in practice because task_idle is always
 * RUNNING).  If `cur` is not in the list (e.g. just-blocked task), start
 * from the sentinel head. */
static aegis_task_t *
run_list_next_locked(aegis_task_t *cur)
{
    aegis_task_t *start;
    if (cur && cur->next_run)
        start = cur->next_run;
    else
        start = s_run_sentinel.next_run;
    if (start == &s_run_sentinel)
        start = s_run_sentinel.next_run;   /* skip sentinel once */
    if (start == &s_run_sentinel)
        return (aegis_task_t *)0;          /* list empty */
    return start;
}

void
sched_init(void)
{
    percpu_set_current((aegis_task_t *)0);
    s_next_tid   = 0;
    s_task_count = 0;
    s_run_sentinel.next_run = &s_run_sentinel;
    s_run_sentinel.prev_run = &s_run_sentinel;
}

void
sched_spawn(void (*fn)(void))
{
    irqflags_t fl = spin_lock_irqsave(&sched_lock);

    /* Allocate TCB (one kva page — higher-half VA, no identity-map dependency). */
    aegis_task_t *task = kva_alloc_pages(1);

    /* Allocate stack: STACK_PAGES usable pages plus one unmapped guard page
     * at the bottom.  Stack grows downward; the guard page causes a #PF on
     * overflow instead of silently corrupting adjacent KVA allocations.
     * The guard VA is permanently abandoned (bump allocator does not rewind). */
    uint8_t *stack_region = kva_alloc_pages(STACK_PAGES + 1);
    uint64_t guard_phys   = kva_page_phys(stack_region);
    vmm_unmap_page((uint64_t)(uintptr_t)stack_region);
    pmm_free_page(guard_phys);
    /* Usable stack starts one page above the (now-unmapped) guard page. */
    uint8_t *stack = stack_region + 4096UL;

    /* Set up the stack to look like ctx_switch already ran.
     * The frame layout must match the push/pop order in ctx_switch.asm/S. */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
#ifdef __aarch64__
    /* ARM64 ctx_switch pushes 6 pairs via stp (x19/x20 ... x29/x30).
     * x30 (lr) = fn. Build from high to low matching ldp order:
     * [x19][x20] [x21][x22] [x23][x24] [x25][x26] [x27][x28] [x29][x30] */
    *--sp = 0;                          /* x20 */
    *--sp = 0;                          /* x19 */
    *--sp = 0;                          /* x22 */
    *--sp = 0;                          /* x21 */
    *--sp = 0;                          /* x24 */
    *--sp = 0;                          /* x23 */
    *--sp = 0;                          /* x26 */
    *--sp = 0;                          /* x25 */
    *--sp = 0;                          /* x28 */
    *--sp = 0;                          /* x27 */
    *--sp = (uint64_t)(uintptr_t)fn;   /* x30 (lr) — ret jumps here */
    *--sp = 0;                          /* x29 (fp) */
#else
    /* x86-64 ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret → fn. */
    *--sp = (uint64_t)(uintptr_t)fn;   /* return address */
    *--sp = 0;                          /* rbx */
    *--sp = 0;                          /* rbp */
    *--sp = 0;                          /* r12 */
    *--sp = 0;                          /* r13 */
    *--sp = 0;                          /* r14 */
    *--sp = 0;                          /* r15 */
#endif

    task->sp               = (uint64_t)(uintptr_t)sp;
    task->stack_base       = stack;
    task->kernel_stack_top = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    task->is_user          = 0;
    task->tid              = s_next_tid++;
    task->stack_pages      = STACK_PAGES;
    task->state            = TASK_RUNNING;
    task->waiting_for      = 0;
    task->next_run         = (aegis_task_t *)0;
    task->prev_run         = (aegis_task_t *)0;

    /* Add to circular list */
    aegis_task_t *cur = sched_current();
    if (!cur) {
        task->next = task;
        percpu_set_current(task);
    } else {
        /* Insert after current */
        task->next = cur->next;
        cur->next  = task;
    }

    /* Insert into the RUNNING-only run queue. */
    run_list_insert_locked(task);

    s_task_count++;

    spin_unlock_irqrestore(&sched_lock, fl);
}

void
sched_add(aegis_task_t *task)
{
    irqflags_t fl = spin_lock_irqsave(&sched_lock);
    task->next_run = (aegis_task_t *)0;
    task->prev_run = (aegis_task_t *)0;
    aegis_task_t *cur = sched_current();
    if (!cur) {
        task->next = task;
        percpu_set_current(task);
    } else {
        task->next = cur->next;
        cur->next  = task;
    }
    /* Only insert into the run list if the task is actually runnable.
     * proc_spawn may set state to TASK_BLOCKED for clone() threads that
     * will be woken later — insert_if_running makes this explicit. */
    if (task->state == TASK_RUNNING)
        run_list_insert_locked(task);
    s_task_count++;
    spin_unlock_irqrestore(&sched_lock, fl);
}

/* Deferred cleanup: dying task's resources cannot be freed before ctx_switch
 * (ctx_switch writes dying->sp; the dying stack is live until the stack pointer switches).
 * Recorded in percpu_t (prev_dying_tcb/stack/stack_pages) and freed at the
 * entry of the next sched_exit call. Per-CPU storage prevents two CPUs
 * exiting tasks simultaneously from overwriting each other's dying state. */

void
sched_exit(void)
{
    irqflags_t fl = spin_lock_irqsave(&sched_lock);

    /* ── Deferred cleanup from the PREVIOUS exiting kernel/orphaned task ──
     * Free TCB + kernel stack of the task that exited last time on this CPU.
     * Safe: ctx_switch has completed; that TCB and stack are no longer live
     * on any CPU. Must be at the TOP of sched_exit, before any new exit logic. */
    percpu_t *pc = percpu_self();
    if (pc->prev_dying_tcb) {
        kva_free_pages(pc->prev_dying_stack, pc->prev_dying_stack_pages);
        kva_free_pages(pc->prev_dying_tcb,
                       ((aegis_task_t *)pc->prev_dying_tcb)->is_user ? 2 : 1);
        pc->prev_dying_tcb = NULL;
    }

    /* Switch to master PML4 so kernel structures are safely accessible.
     * TCBs are kva-mapped higher-half VAs visible from any CR3 (pd_hi is
     * shared), so this is a defensive measure rather than a hard requirement. */
    vmm_switch_to(vmm_get_master_pml4());

    aegis_task_t *s_cur = sched_current();
    if (s_cur->is_user) {
        aegis_process_t *dying = (aegis_process_t *)s_cur;
        /* dying->exit_status was set by sys_exit before calling sched_exit. */

        /* Release the shared fd table (closes all fds if refcount drops to 0).
         *
         * Required for pipe correctness: write-end close fires sched_wake()
         * on any blocked reader, which must happen while the task is still
         * TASK_RUNNING (not TASK_ZOMBIE) so the woken task can be properly
         * scheduled.
         *
         * Ordering invariant: this runs before vmm_free_user_pml4
         * (wherever it is called). pipe_t lives in kva (kernel VA, always
         * accessible). Any future fd type whose close op touches user memory
         * must also rely on this ordering — do not move this call later. */
        fd_table_unref(dying->fd_table);
        dying->fd_table = (fd_table_t *)0;

        /* Mark self zombie — stays in the full task list until waitpid
         * reaps, but must be removed from the RUNNING-only run queue
         * immediately so sched_tick does not schedule a dying task. */
        s_cur->state = TASK_ZOMBIE;
        run_list_remove_locked(s_cur);

        /* Notify parent of child exit via SIGCHLD.
         * signal_send_pid_locked sets SIGCHLD pending on the parent and
         * calls sched_wake_locked() if the parent is TASK_BLOCKED
         * (sigsuspend path or blocking waitpid path), transitioning it to
         * TASK_RUNNING and inserting it into the run list.  We hold
         * sched_lock here, so we must use the _locked variant to avoid
         * recursive acquisition.
         * Must run before the woken_parent scan so the scan finds the
         * parent in TASK_RUNNING state. */
        if (dying->ppid != 0)
            signal_send_pid_locked(dying->ppid, SIGCHLD);

        /* Find parent for direct ctx_switch (avoids PIT dependency).
         * signal_send_pid may have transitioned the parent BLOCKED→RUNNING;
         * check TASK_RUNNING here. */
        aegis_task_t *woken_parent = (void *)0;
        aegis_task_t *t = s_cur->next;
        while (t != s_cur) {
            if (t->is_user && t->state == TASK_RUNNING) {
                aegis_process_t *p = (aegis_process_t *)t;
                if (p->pid == dying->ppid &&
                    (t->waiting_for == 0 || t->waiting_for == dying->pid)) {
                    woken_parent = t;
                    break;
                }
            }
            t = t->next;
        }

        /* Shutdown detection: scan for remaining RUNNING user tasks.
         * Zombies have is_user=1 but state==TASK_ZOMBIE — they are not live.
         * Without the state check, this scan would never trigger shutdown
         * because the just-zombified task is still in the queue with is_user=1. */
        int live_users = 0;
        t = s_cur->next;
        while (t != s_cur) {
            if (t->is_user && t->state != TASK_ZOMBIE)
                live_users = 1;
            t = t->next;
        }
        if (!live_users) {
            ext2_sync();    /* flush dirty blocks to NVMe before exit */
            printk("[AEGIS] System halted.\n");
            arch_request_shutdown();
        }

        /* Yield — zombie stays in queue until waitpid reaps.
         * Do NOT use the deferred-cleanup (g_prev_dying_tcb) path; that is
         * for non-zombie kernel task exits only.
         *
         * Direct-to-parent switch: if we woke a parent, switch to it
         * immediately instead of calling sched_yield_to_next().
         * sched_yield_to_next starts scanning from zombie->next in the
         * circular queue; when the queue is task_idle→parent→zombie,
         * zombie->next==task_idle and task_idle is picked first.  On AMD
         * bare metal the 8259A PIC IRQ0 may never reach the CPU (LAPIC not
         * in ExtINT/virtual-wire mode), so task_idle's sti+hlt never gets
         * preempted and the parent is never scheduled.  Switching directly
         * to the parent eliminates the PIT dependency for this path. */
        if (woken_parent) {
            aegis_task_t *zombie_task = s_cur;
            percpu_set_current(woken_parent);
            arch_set_kernel_stack(woken_parent->kernel_stack_top);
            if (woken_parent->is_user)
                arch_set_fs_base(woken_parent->fs_base);
            spin_unlock_irqrestore(&sched_lock, fl);
            ctx_switch(zombie_task, woken_parent);
            /* unreachable — zombie never resumes after direct switch */
        } else {
            spin_unlock_irqrestore(&sched_lock, fl);
            sched_yield_to_next();
        }
        /* unreachable — zombie never resumes */
        panic_halt("[SCHED] FAIL: zombie task resumed after exit");
    }

    /* ── Kernel task (non-user) exit path ── */

    /* IF=0 throughout (IA32_SFMASK cleared IF on SYSCALL entry) —
     * no preemption can occur during list manipulation. */
    aegis_task_t *prev = s_cur;
    while (prev->next != s_cur)
        prev = prev->next;

    aegis_task_t *dying_k = s_cur;
    aegis_task_t *next_k  = dying_k->next;
    prev->next            = next_k;

    /* Remove from the RUNNING-only run queue as well.  Capture the next
     * runnable task via the run list so we preserve round-robin order
     * instead of following the full-list successor. */
    aegis_task_t *next_run = dying_k->next_run;
    run_list_remove_locked(dying_k);
    if (next_run == (aegis_task_t *)0 || next_run == &s_run_sentinel)
        next_run = s_run_sentinel.next_run;
    if (next_run != (aegis_task_t *)0 && next_run != &s_run_sentinel)
        next_k = next_run;

    s_task_count--;

    if (next_k == dying_k) {  /* last task — everything has exited */
        arch_request_shutdown();
        for (;;) arch_halt();
    }

    percpu_set_current(next_k);
    arch_set_kernel_stack(next_k->kernel_stack_top);

    /* If the next task is a user task, switch to its PML4. */
    if (next_k->is_user)
        vmm_switch_to(((aegis_process_t *)next_k)->pml4_phys);

    /* Record dying kernel task for deferred cleanup at the next sched_exit entry.
     * Must be set AFTER all list manipulation and BEFORE ctx_switch:
     * ctx_switch writes dying_k->sp, so the TCB must remain valid until
     * after the stack pointer switch completes. Per-CPU storage is safe here
     * because sched_lock is held and the dying task runs on this CPU only. */
    pc->prev_dying_stack       = (void *)dying_k->stack_base;
    pc->prev_dying_stack_pages = dying_k->stack_pages;
    pc->prev_dying_tcb         = dying_k;
    spin_unlock_irqrestore(&sched_lock, fl);
    ctx_switch(dying_k, next_k);
    __builtin_unreachable();
}

void
sched_block(void)
{
    irqflags_t fl = spin_lock_irqsave(&sched_lock);

    aegis_task_t *old = sched_current();

    /* Capture the next runnable task BEFORE removing old from the run list,
     * so round-robin order is preserved. */
    aegis_task_t *next = old->next_run;

    old->state = TASK_BLOCKED;
    run_list_remove_locked(old);

    /* Note: the task remains in the FULL circular list (next/prev via `next`)
     * so sched_exit's signal_send_pid scan and waitpid iteration can still
     * find it by pid.  It is only removed from the RUNNING-only run queue. */

    if (next == (aegis_task_t *)0 || next == &s_run_sentinel)
        next = s_run_sentinel.next_run;
    if (next == &s_run_sentinel) {
        /* Run list contains only the sentinel — no runnable tasks.
         * task_idle always stays RUNNING, so this should never happen in
         * practice. Panic if it does (corruption). */
        panic_halt("[SCHED] FAIL: sched_block with empty run list");
    }
    percpu_set_current(next);

    /* Update TSS RSP0 and percpu.kernel_stack for the incoming task
     * before ctx_switch so the next syscall from this task uses its own
     * kernel stack, not the stack of whatever task ran last. */
    arch_set_kernel_stack(next->kernel_stack_top);

    /* Set FS.base for the incoming task before ctx_switch. */
    if (next->is_user)
        arch_set_fs_base(next->fs_base);

    spin_unlock_irqrestore(&sched_lock, fl);
    ctx_switch(old, next);

    /* Restore CR3 for the incoming user task after ctx_switch returns.
     * sched_exit switches to master PML4 before context-switching to this
     * task (via sched_yield_to_next).  Without this restore, a user task
     * that was unblocked by sched_exit would resume with master PML4 loaded,
     * causing any copy_to_user call (e.g. sys_waitpid wstatus write) to #PF
     * because user stack pages are only mapped in the process's user PML4. */
    aegis_task_t *resumed = sched_current();
    if (resumed->is_user)
        vmm_switch_to(((aegis_process_t *)resumed)->pml4_phys);

    /* Restore FS.base for the incoming user task after ctx_switch returns.
     * sched_tick does this in its path; sched_block must mirror it.
     * Without this, a user task that was blocked while another user task
     * ran and set a different FS_BASE would resume with the wrong FS_BASE,
     * corrupting TLS access (__errno_location, stack canary, etc.). */
    if (resumed->is_user)
        arch_set_fs_base(resumed->fs_base);
}

void
sched_wake_locked(aegis_task_t *task)
{
    /* Caller holds sched_lock.  Flip state to TASK_RUNNING and insert the
     * task into the RUNNING-only run queue if it is not already there.
     *
     * The atomic-release state write (M1 audit fix) is preserved: sched_tick
     * reads task->state without the lock under some paths, and the release
     * ordering makes the state flip visible to other CPUs along with any
     * wake-up condition the caller arranged before invoking us. */
    __atomic_store_n(&task->state, TASK_RUNNING, __ATOMIC_RELEASE);
    run_list_insert_locked(task);
}

void
sched_wake(aegis_task_t *task)
{
    /* Public entry — acquires sched_lock.  Most callers (pipe close,
     * socket rx, futex wake, signal_send_pid outside sched_exit) do not
     * hold sched_lock, so they use this variant.
     *
     * sched_exit → signal_send_pid_locked → sched_wake_locked takes the
     * other path because sched_lock is already held there. */
    irqflags_t fl = spin_lock_irqsave(&sched_lock);
    sched_wake_locked(task);
    spin_unlock_irqrestore(&sched_lock, fl);
}

void
sched_stop(aegis_task_t *task)
{
    irqflags_t fl = spin_lock_irqsave(&sched_lock);

    aegis_task_t *cur = sched_current();
    if (task != cur) {
        /* Stopping a different task: flip state and remove from run list. */
        task->state = TASK_STOPPED;
        run_list_remove_locked(task);
        spin_unlock_irqrestore(&sched_lock, fl);
        return;
    }

    /* Self-stop: mirrors sched_block exactly, but sets TASK_STOPPED. */
    aegis_task_t *old = cur;

    /* Capture the next runnable task BEFORE removing old from the run list. */
    aegis_task_t *next = old->next_run;

    old->state = TASK_STOPPED;
    run_list_remove_locked(old);

    if (next == (aegis_task_t *)0 || next == &s_run_sentinel)
        next = s_run_sentinel.next_run;
    if (next == &s_run_sentinel)
        panic_halt("[SCHED] FAIL: sched_stop with empty run list");
    percpu_set_current(next);

    arch_set_kernel_stack(next->kernel_stack_top);

    if (next->is_user)
        arch_set_fs_base(next->fs_base);

    spin_unlock_irqrestore(&sched_lock, fl);
    ctx_switch(old, next);

    /* After ctx_switch returns (SIGCONT has resumed us), restore CR3 + FS.base.
     * Mirrors sched_block tail exactly. */
    aegis_task_t *resumed = sched_current();
    if (resumed->is_user)
        vmm_switch_to(((aegis_process_t *)resumed)->pml4_phys);

    if (resumed->is_user)
        arch_set_fs_base(resumed->fs_base);
}

void
sched_resume(aegis_task_t *task)
{
    /* Mirrors sched_wake: flip state back to RUNNING and insert the task
     * into the run queue.  Works for both TASK_STOPPED and TASK_BLOCKED
     * (SIGCONT while blocked on a read must also let the read return EINTR).
     * Acquires sched_lock; no caller currently holds it when calling this. */
    irqflags_t fl = spin_lock_irqsave(&sched_lock);
    __atomic_store_n(&task->state, TASK_RUNNING, __ATOMIC_RELEASE);
    run_list_insert_locked(task);
    spin_unlock_irqrestore(&sched_lock, fl);
}

void
sched_yield_to_next(void)
{
    irqflags_t fl = spin_lock_irqsave(&sched_lock);

    aegis_task_t *old = sched_current();
    /* old may or may not still be in the run list (zombie callers remove
     * it before invoking us).  run_list_next_locked handles both. */
    aegis_task_t *next = run_list_next_locked(old);
    if (next == (aegis_task_t *)0)
        panic_halt("[SCHED] FAIL: sched_yield_to_next with empty run list");
    if (next == old) {
        /* Only one runnable task (old itself) — nothing to switch to.
         * Caller is expected to have removed old from the run list before
         * calling; if we end up with old still as the only option, just
         * return and let the original context continue. */
        spin_unlock_irqrestore(&sched_lock, fl);
        return;
    }
    percpu_set_current(next);
    arch_set_kernel_stack(next->kernel_stack_top);

    /* Set FS.base for the incoming task before ctx_switch. */
    if (next->is_user)
        arch_set_fs_base(next->fs_base);

    spin_unlock_irqrestore(&sched_lock, fl);
    ctx_switch(old, next);

    /* Restore CR3 for the incoming user task after ctx_switch returns.
     * sched_exit calls vmm_switch_to(master_pml4) at its top, then calls
     * sched_yield_to_next to switch away from the dying task.  The task
     * that resumes here would have master PML4 loaded; any subsequent
     * copy_to_user (e.g. sys_waitpid wstatus write) would #PF because the
     * user stack pages are only mapped in the process's user PML4. */
    aegis_task_t *resumed = sched_current();
    if (resumed->is_user)
        vmm_switch_to(((aegis_process_t *)resumed)->pml4_phys);

    /* Restore FS.base for the incoming user task after ctx_switch returns.
     * sched_tick does this in its path; sched_yield_to_next must mirror it.
     * Without this, a user task that was blocked while another user task
     * ran and set a different FS_BASE would resume with the wrong FS_BASE,
     * corrupting TLS access (__errno_location, stack canary, etc.). */
    if (resumed->is_user)
        arch_set_fs_base(resumed->fs_base);
}

void
sched_start(void)
{
    aegis_task_t *first = sched_current();
    if (!first) {
        printk("[SCHED] FAIL: sched_start called with no tasks\n");
        panic_halt("[SCHED] FAIL: sched_start called with no tasks");
    }

    s_sched_ready = 1;  /* guard: sched_tick now safe to context-switch */
    printk("[SCHED] OK: scheduler started, %u tasks\n", s_task_count);

    /* One-way switch into the first task.
     *
     * IMPORTANT: Do NOT enable interrupts and return here. If we returned to
     * the idle loop and the first timer tick fired from there, sched_tick would call
     * ctx_switch(task_kbd, task_heartbeat) while RSP is deep in the ISR frame.
     * ctx_switch would save the ISR stack pointer into task_kbd->sp, corrupting
     * the TCB. Resuming task_kbd later would load a garbage stack pointer and crash.
     *
     * Fix: switch directly into the first task using a stack-local dummy TCB.
     * ctx_switch saves our current stack pointer into dummy.sp (which we immediately
     * abandon). The first task starts on its own correctly-constructed initial
     * stack. Each task enables interrupts at startup (arch-specific).
     *
     * sched_start() never returns.
     */
    arch_set_kernel_stack(first->kernel_stack_top);
    /* sched_start always enters the first task (kbd, a kernel task).
     * No CR3 switch here: if the first task is a user task, proc_enter_user
     * handles the PML4 switch before iretq (same as the timer-preemption
     * first-entry path). */

    aegis_task_t dummy;
    ctx_switch(&dummy, first);
    __builtin_unreachable();
}

void
sched_tick(void)
{
    if (!s_sched_ready)                    /* PIT fires before sched_start */
        return;
    aegis_task_t *cur = sched_current();
    if (!cur)                              /* no tasks spawned yet */
        return;

    spin_lock(&sched_lock);

    /* Wake any tasks whose nanosleep deadline has passed.
     *
     * We still need to iterate the FULL task list (`next`) here because
     * sleeping tasks are TASK_BLOCKED and therefore NOT in the RUNNING-only
     * run queue.  The run queue does not contain them, so we cannot use it
     * to find them.  Once a sleeping task's deadline expires we flip its
     * state to TASK_RUNNING and insert it into the run list. */
    {
        uint64_t now = arch_get_ticks();
        aegis_task_t *t = cur->next;
        aegis_task_t *stop = cur;
        do {
            if (t->state == TASK_BLOCKED && t->sleep_deadline != 0 &&
                now >= t->sleep_deadline) {
                t->sleep_deadline = 0;
                t->state = TASK_RUNNING;
                run_list_insert_locked(t);
            }
            t = t->next;
        } while (t != stop);
    }

    aegis_task_t *old = cur;
    /* Walk the RUNNING-only run queue — O(R) where R = runnable count.
     * This is the whole point of the P3 audit fix: we no longer scan
     * blocked/zombie/stopped tasks here.
     *
     * `old` should normally be in the run list (it is the currently
     * running task, which is TASK_RUNNING).  If for some reason it is
     * not (e.g. sched_tick fired while old was transitioning states),
     * run_list_next_locked starts from the sentinel head. */
    aegis_task_t *next = run_list_next_locked(old);
    if (next == (aegis_task_t *)0 || next == old) {
        /* Nothing else runnable — stay on `cur`. */
        spin_unlock(&sched_lock);
        return;
    }
    percpu_set_current(next);

    arch_set_kernel_stack(next->kernel_stack_top);

    /* Set FS.base for the incoming task before ctx_switch so that the task
     * enters user space (or resumes) with the correct TLS pointer.
     * Must be paired with the arch_set_fs_base after ctx_switch (for the
     * outgoing task's subsequent resume). */
    if (next->is_user)
        arch_set_fs_base(next->fs_base);

    /*
     * CR3 switch policy in sched_tick:
     *
     * sched_tick always runs inside isr_common_stub which switches to the
     * master PML4 at interrupt entry.  sched_tick therefore always executes
     * with the master PML4 loaded, regardless of whether the interrupted task
     * was a kernel or user task.
     *
     * (a) Switching TO a user task: do NOT switch CR3 here.  The switch to
     *     the user PML4 is performed by proc_enter_user (first entry) or by
     *     isr_common_stub's saved-CR3 restore (subsequent preemptions).
     *
     *     CRITICAL: sched_tick runs on the OUTGOING kernel task's kva-mapped
     *     stack.  Calling vmm_switch_to(user_pml4) from mid-sched_tick would
     *     switch away from the task being context-switched out while its stack
     *     is still live on the CPU — the next stack access would use the wrong
     *     CR3 context.  CR3 switches happen only in proc_enter_user (ring-3
     *     entry) and sched_exit (task teardown).
     *
     * (b) Switching FROM a user task to a kernel task: isr_common_stub
     *     already switched to master PML4 at interrupt entry.  No further
     *     CR3 switch is needed here.
     */

    /* ctx_switch is declared in arch.h with a forward struct declaration.
     * It saves old->sp, loads next->sp, and returns into new task. */
    spin_unlock(&sched_lock);
    ctx_switch(old, next);

    /* Restore the incoming user process's FS base.
     * This must run AFTER ctx_switch returns (sched_current() is now the new task).
     * proc_enter_user handles only the first entry; preempted tasks resume
     * via isr_common_stub which does not reload FS.base. IF=0 here (PIT ISR). */
    aegis_task_t *resumed = sched_current();
    if (resumed->is_user)
        arch_set_fs_base(resumed->fs_base);
}
