; isr.asm — Interrupt Service Routine entry stubs for Aegis
;
; Each stub pushes a uniform stack frame (fake error code for exceptions
; that don't push one), then jumps to isr_common_stub.
;
; isr_common_stub saves all GPRs, calls isr_dispatch(cpu_state_t*), restores.
;
; Vector → macro mapping (Intel SDM Vol 3A Table 6-1):
; ISR_NOERR: 0,1,2,3,4,5,6,7,9,15,16,18,19,20,28,31
; ISR_ERR:   8,10,11,12,13,14,17,21,29,30
; Reserved (install ISR_NOERR as placeholder): 22,23,24,25,26,27
; IRQ stubs (no error code): 0x20-0x2F

bits 64
section .text

extern isr_dispatch

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push qword 0    ; fake error code (uniform frame)
    push qword %1   ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    push qword %1   ; vector number (error code already on stack from CPU)
    jmp isr_common_stub
%endmacro

; CPU exceptions 0-31
ISR_NOERR  0   ; #DE divide error
ISR_NOERR  1   ; #DB debug
ISR_NOERR  2   ; NMI
ISR_NOERR  3   ; #BP breakpoint
ISR_NOERR  4   ; #OF overflow
ISR_NOERR  5   ; #BR bound range
ISR_NOERR  6   ; #UD invalid opcode
ISR_NOERR  7   ; #NM device not available
ISR_ERR    8   ; #DF double fault
ISR_NOERR  9   ; coprocessor segment overrun (reserved)
ISR_ERR   10   ; #TS invalid TSS
ISR_ERR   11   ; #NP segment not present
ISR_ERR   12   ; #SS stack fault
ISR_ERR   13   ; #GP general protection
ISR_ERR   14   ; #PF page fault
ISR_NOERR 15   ; reserved
ISR_NOERR 16   ; #MF x87 FP exception
ISR_ERR   17   ; #AC alignment check
ISR_NOERR 18   ; #MC machine check
ISR_NOERR 19   ; #XM SIMD FP exception
ISR_NOERR 20   ; #VE virtualization exception
ISR_ERR   21   ; #CP control protection (#CP has error code)
ISR_NOERR 22   ; reserved
ISR_NOERR 23   ; reserved
ISR_NOERR 24   ; reserved
ISR_NOERR 25   ; reserved
ISR_NOERR 26   ; reserved
ISR_NOERR 27   ; reserved
ISR_NOERR 28   ; #HV hypervisor injection
ISR_ERR   29   ; #VC VMM communication
ISR_ERR   30   ; #SX security exception
ISR_NOERR 31   ; reserved

; Hardware IRQs 0x20-0x2F (remapped by PIC)
ISR_NOERR 0x20 ; IRQ0 — PIT timer
ISR_NOERR 0x21 ; IRQ1 — PS/2 keyboard
ISR_NOERR 0x22 ; IRQ2 — cascade (internal)
ISR_NOERR 0x23 ; IRQ3 — COM2
ISR_NOERR 0x24 ; IRQ4 — COM1
ISR_NOERR 0x25 ; IRQ5
ISR_NOERR 0x26 ; IRQ6 — floppy
ISR_NOERR 0x27 ; IRQ7 — LPT1 / spurious master
ISR_NOERR 0x28 ; IRQ8 — RTC
ISR_NOERR 0x29 ; IRQ9
ISR_NOERR 0x2A ; IRQ10
ISR_NOERR 0x2B ; IRQ11
ISR_NOERR 0x2C ; IRQ12 — PS/2 mouse
ISR_NOERR 0x2D ; IRQ13 — FPU
ISR_NOERR 0x2E ; IRQ14 — primary ATA
ISR_NOERR 0x2F ; IRQ15 — secondary ATA / spurious slave

; Common stub — saves all GPRs, calls isr_dispatch, restores, iretq
;
; On entry the stack holds (low address to high):
;   [RSP+0]  vector        (pushed by stub)
;   [RSP+8]  error_code    (pushed by CPU or fake 0 from stub)
;   [RSP+16] rip           (CPU-pushed interrupt frame)
;   [RSP+24] cs
;   [RSP+32] rflags
;   [RSP+40] rsp
;   [RSP+48] ss
;
; After GPR pushes RSP points to the cpu_state_t.r15 field.
; Field layout (low to high) matches push order exactly:
;   r15, r14, r13, r12, r11, r10, r9, r8,
;   rbp, rdi, rsi, rdx, rcx, rbx, rax,
;   vector, error_code, rip, cs, rflags, rsp, ss
;
; Clobbers: all registers (saved and restored here).
; Calling convention: SystemV AMD64 ABI — rdi = first argument.
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; RSP now points to cpu_state_t (r15 is the first/lowest field).
    ; Pass pointer to the struct as first argument per SystemV AMD64 ABI.
    mov rdi, rsp
    call isr_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; discard vector + error_code
    iretq

; Jump table — isr_stubs[i] = pointer to isr_i
; idt.c references this as: extern void *isr_stubs[48];
section .data
global isr_stubs
isr_stubs:
    dq isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    dq isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    dq isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    dq isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    dq isr_0x20, isr_0x21, isr_0x22, isr_0x23
    dq isr_0x24, isr_0x25, isr_0x26, isr_0x27
    dq isr_0x28, isr_0x29, isr_0x2A, isr_0x2B
    dq isr_0x2C, isr_0x2D, isr_0x2E, isr_0x2F
