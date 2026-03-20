#ifndef AEGIS_SCHED_H
#define AEGIS_SCHED_H

#include <stdint.h>

typedef struct aegis_task_t {
    uint64_t             rsp;              /* MUST be first — ctx_switch reads [rdi+0] */
    uint8_t             *stack_base;       /* bottom of PMM-allocated stack (for future cleanup) */
    uint64_t             kernel_stack_top; /* RSP0 value: kernel stack top for this task */
    uint32_t             tid;              /* task ID */
    uint8_t              is_user;          /* 1 = user process (aegis_process_t), 0 = kernel task */
    struct aegis_task_t *next;             /* circular linked list */
} aegis_task_t;

/* Initialize the run queue. No tasks yet. */
void sched_init(void);

/* Allocate a TCB and 16KB stack from PMM; wire fn as entry point; add to queue. */
void sched_spawn(void (*fn)(void));

/* Print [SCHED] OK line, then switch directly into the first task via a
 * dummy TCB (one-way ctx_switch). Does not return. Each task enables
 * interrupts on entry via the arch interrupt-enable primitive. */
void sched_start(void);

/* Called by pit_handler on each timer tick.
 * Advances current task and calls ctx_switch. */
void sched_tick(void);

/* Add a pre-initialized TCB to the run queue.
 * Used by proc_spawn to insert a user process without duplicating
 * the list-insertion logic from sched_spawn. */
void sched_add(aegis_task_t *task);

/* Remove the current task from the run queue and switch to the next task.
 * Called from syscall exit (60). Does not return. Memory is leaked
 * (intentional for Phase 5 — Phase 6 adds cleanup). */
void sched_exit(void);

#endif /* AEGIS_SCHED_H */
