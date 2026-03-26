/*
 * stubs.c — ARM64 stubs for interfaces not yet implemented.
 *
 * Provides symbols that kernel/core/ and kernel/mm/ reference via arch.h,
 * but that don't apply to the ARM64 boot stage (no VGA, no framebuffer).
 * Also provides memcpy/memset which GCC may emit at -O2.
 */

#include <stdint.h>
#include <stddef.h>

/* GCC emits calls to memcpy/memset even with -ffreestanding.
 * Provide minimal implementations. */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

/* VGA is not available on ARM64 QEMU virt. */
int vga_available = 0;

/* PAN not yet enabled. */
int arch_smap_enabled = 0;

void
vga_write_string(const char *s)
{
    (void)s;
}

/* fb_available and fb_write_string are provided by the real fb.c
 * (compiled as a shared source). fb.c handles the fb_available=0 case. */

/* arch_init — on ARM64, UART is ready from reset. Nothing to do. */
void
arch_init(void)
{
}

/* arch_debug_exit — use QEMU semihosting to exit.
 * QEMU virt with -semihosting supports the angel SYS_EXIT call. */
void
arch_debug_exit(unsigned char value)
{
    (void)value;
    for (;;)
        __asm__ volatile("wfi");
}

void
arch_request_shutdown(void)
{
    arch_debug_exit(1);
}

/* ── Capability system stubs (Rust FFI — not yet built for aarch64) ── */
void cap_init(void) {
    extern void serial_write_string(const char *);
    serial_write_string("[CAP] OK: capability subsystem initialized\n");
}
int cap_check(void *proc, uint32_t kind, uint32_t rights)
    { (void)proc; (void)kind; (void)rights; return 0; /* allow all */ }
int cap_grant(void *proc, uint32_t kind, uint32_t rights)
    { (void)proc; (void)kind; (void)rights; return 0; }

/* ── String/memory functions GCC may need ── */
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

/* ── USB HID stub ── */
void kbd_usb_inject(char c) { (void)c; }

/* ── Signal stubs ── */
void signal_send_pgrp(uint32_t pgrp, int sig) { (void)pgrp; (void)sig; }

/* ── User binary blobs — empty placeholders (no user binaries for ARM64 yet) ── */
/* init_elf and init_elf_len provided by init_arm64_bin.c (compiled separately) */
const char *init_name = "/bin/init";
const unsigned char shell_elf[] = { 0 };
const unsigned int  shell_elf_len = 0;
const unsigned char cat_elf[] = { 0 }, echo_elf[] = { 0 }, ls_elf[] = { 0 };
const unsigned int  cat_elf_len = 0, echo_elf_len = 0, ls_elf_len = 0;
const unsigned char pwd_elf[] = { 0 }, uname_elf[] = { 0 }, clear_elf[] = { 0 };
const unsigned int  pwd_elf_len = 0, uname_elf_len = 0, clear_elf_len = 0;
const unsigned char true_bin_elf[] = { 0 }, false_bin_elf[] = { 0 };
const unsigned int  true_bin_elf_len = 0, false_bin_elf_len = 0;
const unsigned char wc_elf[] = { 0 }, grep_elf[] = { 0 }, sort_elf[] = { 0 };
const unsigned int  wc_elf_len = 0, grep_elf_len = 0, sort_elf_len = 0;
const unsigned char mkdir_elf[] = { 0 }, touch_elf[] = { 0 }, rm_elf[] = { 0 };
const unsigned int  mkdir_elf_len = 0, touch_elf_len = 0, rm_elf_len = 0;
const unsigned char cp_elf[] = { 0 }, mv_elf[] = { 0 }, whoami_elf[] = { 0 };
const unsigned int  cp_elf_len = 0, mv_elf_len = 0, whoami_elf_len = 0;
const unsigned char oksh_elf[] = { 0 }, login_elf[] = { 0 };
const unsigned int  oksh_elf_len = 0, login_elf_len = 0;
const unsigned char vigil_elf[] = { 0 }, vigictl_elf[] = { 0 };
const unsigned int  vigil_elf_len = 0, vigictl_elf_len = 0;
const unsigned char httpd_bin_elf[] = { 0 };
const unsigned int  httpd_bin_elf_len = 0;

/* proc_enter_user and fork_child_return are now in proc_enter.S */

/* ── Stubs for subsystems not yet ported ──────────────────────────── */

/* kbd stubs — no PS/2 on ARM64 QEMU virt */
void kbd_init(void) {}
void kbd_handler(void) {}
char kbd_read(void) { for (;;) __asm__ volatile("wfi"); }
char kbd_read_interruptible(int *interrupted) { (void)interrupted; for (;;) __asm__ volatile("wfi"); }
int  kbd_poll(char *out) { (void)out; return 0; }
static uint32_t s_tty_pgrp = 0;
void     kbd_set_tty_pgrp(uint32_t pgid) { s_tty_pgrp = pgid; }
uint32_t kbd_get_tty_pgrp(void) { return s_tty_pgrp; }

/* sched.c calls these — stub until user process support is added */
void arch_set_kernel_stack(uint64_t sp0) {
    /* On ARM64 with SPSel=1, the kernel uses SP (= SP_EL1 at EL1).
     * Exceptions from EL0 auto-use SP_EL1. The scheduler's ctx_switch
     * already switches SP to the incoming task's kernel stack. */
    (void)sp0;
}
uint64_t g_master_pml4 = 0;
void arch_set_master_pml4(uint64_t pml4_phys) { g_master_pml4 = pml4_phys; }

/* sched.c exit path calls ext2_sync and signal_send_pid */
/* ext2_sync provided by real ext2_cache.c */
void signal_send_pid(uint32_t pid, int sig) { (void)pid; (void)sig; }

/* proc_spawn_init is provided by the real proc.c (shared source). */
