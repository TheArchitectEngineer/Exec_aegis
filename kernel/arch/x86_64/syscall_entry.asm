; syscall_entry.asm — SYSCALL landing pad and ring-3 entry helper
;
; syscall_entry: called by CPU on SYSCALL instruction.
;   CPU state on entry:
;     RCX = return RIP (user), R11 = saved RFLAGS, RSP = user RSP
;     IF=0, DF=0 (set by IA32_SFMASK=0x700)
;
; proc_enter_user: bare iretq label used by proc_spawn to enter ring 3
;   for the first time. ctx_switch's ret lands here; RSP points at an
;   iretq frame built by proc_spawn. Must NOT have a C prologue.

bits 64
section .text

extern syscall_dispatch
extern g_kernel_rsp
extern g_user_rsp

global syscall_entry
global proc_enter_user

syscall_entry:
    ; RSP is still user RSP — save it, switch to kernel stack
    mov  [rel g_user_rsp], rsp
    mov  rsp, [rel g_kernel_rsp]

    ; Build restore frame on kernel stack (popped before sysretq)
    push qword [rel g_user_rsp]   ; [rsp+16] user RSP  (deepest)
    push rcx                       ; [rsp+8]  return RIP
    push r11                       ; [rsp+0]  RFLAGS    (top, popped first)

    ; Translate Linux syscall ABI → SysV C calling convention:
    ;   Linux: rax=num, rdi=arg1, rsi=arg2, rdx=arg3
    ;   SysV:  rdi=num, rsi=arg1, rdx=arg2, rcx=arg3
    ; rcx already pushed above (return RIP), safe to overwrite now.
    mov  rcx, rdx    ; arg3 ← user rdx
    mov  rdx, rsi    ; arg2 ← user rsi
    mov  rsi, rdi    ; arg1 ← user rdi
    mov  rdi, rax    ; num  ← syscall number

    call syscall_dispatch
    ; rax = return value (already in position for sysretq)

    pop  r11          ; restore RFLAGS
    pop  rcx          ; restore return RIP
    pop  rsp          ; restore user RSP (pop rsp sets RSP=[RSP], not RSP+8)

    sysretq

; proc_enter_user — bare iretq, NO C prologue.
; On entry RSP points at ring-3 iretq frame:
;   [rsp+0]  RIP  (user entry point)
;   [rsp+8]  CS   (0x23 = user code | RPL=3)
;   [rsp+16] RFLAGS (0x202 = IF=1 | reserved bit 1)
;   [rsp+24] RSP  (user stack top, 16-byte aligned)
;   [rsp+32] SS   (0x1B = user data | RPL=3)
proc_enter_user:
    iretq
