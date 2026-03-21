#include "sched.h"
#include "arch.h"
#include "kva.h"
#include "printk.h"
#include "vmm.h"
#include "proc.h"
#include <stddef.h>

/* Compile-time guard: ctx_switch.asm assumes rsp is at offset 0 of TCB.
 * If anyone adds a field before rsp, this catches it immediately. */
_Static_assert(offsetof(aegis_task_t, rsp) == 0,
    "rsp must be first field in aegis_task_t — ctx_switch depends on this");

#define STACK_PAGES  4                     /* 16KB per task */
#define STACK_SIZE   (STACK_PAGES * 4096UL)

static aegis_task_t *s_current = (void *)0;
static uint32_t      s_next_tid = 0;
static uint32_t      s_task_count = 0;

void
sched_init(void)
{
    s_current    = (void *)0;
    s_next_tid   = 0;
    s_task_count = 0;
}

void
sched_spawn(void (*fn)(void))
{
    /* Allocate TCB (one kva page — higher-half VA, no identity-map dependency). */
    aegis_task_t *task = kva_alloc_pages(1);

    /* Allocate stack (STACK_PAGES kva pages — consecutive VAs, no contiguity
     * assumption on physical addresses). */
    uint8_t *stack = kva_alloc_pages(STACK_PAGES);

    /* Set up the stack to look like ctx_switch already ran.
     *
     * ctx_switch pops: r15, r14, r13, r12, rbp, rbx, then ret → fn.
     * So the stack from low (RSP) to high must be:
     *   [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0][fn]
     *
     * We build this by decrementing a pointer from stack_top:
     *   fn pushed first (deepest = highest address before RSP setup)
     *   then six zeros for the callee-saved regs
     *   RSP ends up pointing at the r15 slot.
     */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
    *--sp = (uint64_t)(uintptr_t)fn;  /* return address: ret jumps here */
    *--sp = 0;                         /* rbx */
    *--sp = 0;                         /* rbp */
    *--sp = 0;                         /* r12 */
    *--sp = 0;                         /* r13 */
    *--sp = 0;                         /* r14 */
    *--sp = 0;                         /* r15  ← new task's RSP */

    task->rsp              = (uint64_t)(uintptr_t)sp;
    task->stack_base       = stack;
    task->kernel_stack_top = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    task->is_user          = 0;
    task->tid              = s_next_tid++;

    /* Add to circular list */
    if (!s_current) {
        task->next = task;
        s_current  = task;
    } else {
        /* Insert after current */
        task->next      = s_current->next;
        s_current->next = task;
    }

    s_task_count++;
}

void
sched_add(aegis_task_t *task)
{
    if (!s_current) {
        task->next = task;
        s_current  = task;
    } else {
        task->next      = s_current->next;
        s_current->next = task;
    }
    s_task_count++;
}

void
sched_exit(void)
{
    /* Switch to master PML4 at entry (defensive measure).
     *
     * Before Phase 7: required because TCBs were in identity-mapped [0..4MB),
     * which is absent from user PML4s.
     * After Phase 7: TCBs are kva-mapped higher-half VAs, visible from any CR3
     * (pd_hi is shared). The switch is retained as a defensive measure. */
    vmm_switch_to(vmm_get_master_pml4());

    /* IF=0 throughout (IA32_SFMASK cleared IF on SYSCALL entry) —
     * no preemption can occur during list manipulation. */
    aegis_task_t *prev = s_current;
    while (prev->next != s_current)
        prev = prev->next;

    aegis_task_t *dying = s_current;
    s_current           = dying->next;
    prev->next          = s_current;
    s_task_count--;

    if (s_current == dying) {  /* last task — everything has exited */
        arch_request_shutdown();
        for (;;) __asm__ volatile ("hlt");
    }

    arch_set_kernel_stack(s_current->kernel_stack_top);

    /* If dying is a user task and the next task is a kernel task, switch to
     * master PML4. Kernel task stacks are kva-mapped (higher-half), visible
     * in any PML4, but the switch ensures a clean CR3 state for the resume. */
    if (dying->is_user && !s_current->is_user)
        vmm_switch_to(vmm_get_master_pml4());
    else if (s_current->is_user)
        vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);

    /*
     * PHASE 6 CLEANUP NOTE: ctx_switch saves dying->rsp here — that RSP
     * is somewhere in the middle of the kernel stack (the call depth at
     * sched_exit time). Phase 6 must free dying->stack_base (the PMM
     * allocation) and the process TCB using the physical addresses, not
     * by dereferencing dying->rsp. dying->stack_base + STACK_SIZE gives
     * the allocation top; dying->rsp is the current stack pointer.
     */
    ctx_switch(dying, s_current);
    __builtin_unreachable();
}

void
sched_start(void)
{
    if (!s_current) {
        printk("[SCHED] FAIL: sched_start called with no tasks\n");
        for (;;) {}
    }

    printk("[SCHED] OK: scheduler started, %u tasks\n", s_task_count);

    /* One-way switch into the first task.
     *
     * IMPORTANT: Do NOT enable interrupts and return here. If we returned to
     * the idle loop and the first timer tick fired from there, sched_tick would call
     * ctx_switch(task_kbd, task_heartbeat) while RSP is deep in the ISR frame.
     * ctx_switch would save the ISR stack pointer into task_kbd->rsp, corrupting
     * the TCB. Resuming task_kbd later would load a garbage RSP and crash.
     *
     * Fix: switch directly into the first task using a stack-local dummy TCB.
     * ctx_switch saves our current RSP into dummy.rsp (which we immediately
     * abandon). The first task starts on its own correctly-constructed initial
     * stack. Each task enables interrupts at startup (arch-specific).
     *
     * sched_start() never returns.
     */
    arch_set_kernel_stack(s_current->kernel_stack_top);
    /* sched_start always enters the first task (kbd, a kernel task).
     * No CR3 switch here: if the first task is a user task, proc_enter_user
     * handles the PML4 switch before iretq (same as the timer-preemption
     * first-entry path). */

    aegis_task_t dummy;
    ctx_switch(&dummy, s_current);
    __builtin_unreachable();
}

void
sched_tick(void)
{
    if (!s_current)                        /* no tasks spawned yet */
        return;
    if (s_current->next == s_current)      /* single task: nowhere to switch */
        return;

    aegis_task_t *old = s_current;
    s_current = s_current->next;

    arch_set_kernel_stack(s_current->kernel_stack_top);

    /*
     * CR3 switch policy in sched_tick (Phase 5):
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
     *     CRITICAL: sched_tick runs on the OUTGOING kernel task's physical
     *     stack (identity-mapped only in master PML4).  Calling
     *     vmm_switch_to(user_pml4) from that stack would remove the identity
     *     map, making the stack inaccessible, and causing a triple fault on
     *     the very next stack access inside arch_vmm_load_pml4.
     *
     * (b) Switching FROM a user task to a kernel task: isr_common_stub
     *     already switched to master PML4 at interrupt entry.  No further
     *     CR3 switch is needed here.
     */

    /* ctx_switch is declared in arch.h with a forward struct declaration.
     * It saves old->rsp, loads s_current->rsp, and returns into new task. */
    ctx_switch(old, s_current);
}
