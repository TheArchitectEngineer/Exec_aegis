/*
 * init.S — Minimal ARM64 init binary for Aegis.
 *
 * Runs in EL0 (user mode). Uses Linux AArch64 syscall convention:
 *   x8 = syscall number
 *   x0-x5 = arguments
 *   SVC #0 triggers the syscall
 *
 * Syscall numbers (Linux AArch64 / Aegis):
 *   64 = write (fd, buf, len)  — Aegis maps to sys_write
 *   93 = exit (status)         — Aegis maps to sys_exit
 *
 * Wait — Aegis uses x86-64 syscall numbers (write=1, exit=60).
 * The syscall_dispatch table in syscall.c uses x86 numbers.
 * So we use x86 numbers here too.
 */

    .text
    .globl _start
_start:
    /* sys_write(1, msg, msg_len) — fd 1 = stdout */
    mov     x0, #1              /* fd = stdout */
    adr     x1, msg             /* buf */
    mov     x2, #30             /* len */
    mov     x8, #1              /* syscall: write */
    svc     #0

    /* sys_exit(0) */
    mov     x0, #0              /* status = 0 */
    mov     x8, #60             /* syscall: exit */
    svc     #0

    /* Should not reach here */
    b       .

msg:
    .ascii "[INIT] Hello from ARM64 EL0!\n\0"
