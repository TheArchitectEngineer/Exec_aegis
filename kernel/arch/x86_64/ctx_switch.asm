; ctx_switch.asm — preemptive context switch for Aegis scheduler
;
; Saves callee-saved registers (rbx, rbp, r12-r15) for the outgoing task
; and restores them for the incoming task. Swaps RSP via the TCB's rsp field.
;
; The compiler already saves caller-saved registers (rax, rcx, rdx, rsi,
; rdi, r8-r11) at any call site, so we only need callee-saved here.
;
; Calling convention: System V AMD64 ABI
;   rdi = pointer to current task's aegis_task_t  (outgoing)
;   rsi = pointer to next task's aegis_task_t     (incoming)
;
; Clobbers: RSP (switches to new task's stack). All callee-saved registers
;   are preserved across the call from each task's perspective.
;
; New task entry: sched_spawn sets up the stack so the first ctx_switch
;   into a new task "returns" into the task's entry function. Stack layout
;   (low to high, RSP points at r15 slot):
;     [r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0][fn]
;   After pops: r15-rbx restored to 0, ret pops fn into RIP.

bits 64
section .text

global ctx_switch
ctx_switch:
    ; Save outgoing task's callee-saved registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; current->rsp = rsp  (rsp field is at offset 0 — verified by _Static_assert)
    mov [rdi], rsp

    ; rsp = next->rsp
    mov rsp, [rsi]

    ; Restore incoming task's callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Return into the incoming task (pops its saved RIP from the stack)
    ret
