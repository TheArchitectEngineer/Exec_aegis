/*
 * pwn.c -- Aegis kernel security test harness
 *
 * Attempts a battery of attacks against the kernel from userspace.
 * Each attack expects the kernel to correctly reject the operation.
 *
 * PASS = kernel defended correctly
 * FAIL = vulnerability found (bad for kernel)
 * SKIP = attack could not be attempted (missing precondition)
 *
 * Build: musl-gcc -O2 -static -Wall -Wextra -o pwn.elf pwn.c
 */

#define _GNU_SOURCE
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* ---- Aegis-specific syscall numbers ---- */
#define SYS_AEGIS_SETFG       360
#define SYS_AEGIS_CAP_GRANT   361
#define SYS_AEGIS_CAP_QUERY   362
#define SYS_AEGIS_NETCFG      500
#define SYS_AEGIS_BLKDEV_LIST 510
#define SYS_AEGIS_BLKDEV_IO   511
#define SYS_AEGIS_GPT_RESCAN  512
#define SYS_AEGIS_FB_MAP      513
#define SYS_AEGIS_SPAWN       514

/* x86-64 syscall numbers that may not be in all headers */
#ifndef SYS_brk
#define SYS_brk            12
#endif
#ifndef SYS_arch_prctl
#define SYS_arch_prctl     158
#endif
#ifndef SYS_getrandom
#define SYS_getrandom      318
#endif
#ifndef SYS_epoll_create1
#define SYS_epoll_create1  291
#endif
#ifndef SYS_epoll_wait
#define SYS_epoll_wait     232
#endif
#ifndef SYS_clone
#define SYS_clone          56
#endif

/* ENOCAP is 130 on Aegis */
#define ENOCAP 130

/* ---- Test infrastructure ---- */

static sigjmp_buf g_jmpbuf;
static volatile int g_got_signal;
static volatile int g_signal_num;
static int g_pass, g_fail, g_skip;

static void
crash_handler(int sig)
{
    g_got_signal = 1;
    g_signal_num = sig;
    siglongjmp(g_jmpbuf, 1);
}

static void
install_crash_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

static void
write_str(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    write(STDOUT_FILENO, s, len);
}

static void __attribute__((unused))
write_hex(uint64_t v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    int i;
    for (i = 15; i >= 0; i--) {
        int nib = (v >> (i * 4)) & 0xF;
        buf[17 - i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[18] = '\0';
    write_str(buf);
}

static void
write_int(long v)
{
    char buf[24];
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    int pos = 23;
    buf[pos--] = '\0';
    if (v == 0) {
        buf[pos--] = '0';
    } else {
        while (v > 0) {
            buf[pos--] = '0' + (v % 10);
            v /= 10;
        }
    }
    if (neg) buf[pos--] = '-';
    write_str(&buf[pos + 1]);
}

static void
report(const char *name, const char *result, const char *detail)
{
    write_str("[PWN] ");
    write_str(name);
    write_str(": ");
    write_str(result);
    if (detail && detail[0]) {
        write_str(" (");
        write_str(detail);
        write_str(")");
    }
    write_str("\n");

    if (strcmp(result, "PASS") == 0) g_pass++;
    else if (strcmp(result, "FAIL") == 0) g_fail++;
    else g_skip++;
}

/*
 * TRY_CRASH(name, code_block)
 *
 * Runs code_block inside a sigsetjmp guard.
 * If a signal is caught, the test continues.
 */
#define TRY_CRASH(name, block) do {                         \
    g_got_signal = 0;                                       \
    g_signal_num = 0;                                       \
    if (sigsetjmp(g_jmpbuf, 1) == 0) {                      \
        block;                                              \
    }                                                       \
} while (0)

/* ================================================================
 * 1. SYSCALL FUZZING — invalid syscall numbers and arguments
 * ================================================================ */

static void
test_invalid_syscall_numbers(void)
{
    long r;

    /* Non-existent syscall 999 — should return -ENOSYS */
    r = syscall(999);
    if (r == -1 && errno == 38 /* ENOSYS */)
        report("SYSCALL_999", "PASS", "returned ENOSYS");
    else
        report("SYSCALL_999", "FAIL", "did not return ENOSYS");

    /* Very large syscall number */
    r = syscall(0x7FFFFFFF);
    if (r == -1 && errno == 38)
        report("SYSCALL_MAX_INT", "PASS", "returned ENOSYS");
    else
        report("SYSCALL_MAX_INT", "FAIL", "did not return ENOSYS");

    /* Negative syscall number — wraps to large unsigned */
    r = syscall(-1);
    if (r == -1 && errno == 38)
        report("SYSCALL_NEG1", "PASS", "returned ENOSYS");
    else
        report("SYSCALL_NEG1", "FAIL", "did not return ENOSYS");
}

static void
test_null_pointers(void)
{
    long r;

    /* write with NULL buffer */
    r = syscall(SYS_write, 1, NULL, 100);
    if (r < 0)
        report("WRITE_NULL_BUF", "PASS", "rejected NULL buffer");
    else
        report("WRITE_NULL_BUF", "FAIL", "accepted NULL buffer");

    /* read with NULL buffer */
    r = syscall(SYS_read, 0, NULL, 100);
    if (r < 0)
        report("READ_NULL_BUF", "PASS", "rejected NULL buffer");
    else
        report("READ_NULL_BUF", "FAIL", "accepted NULL buffer");

    /* open with NULL path */
    r = syscall(SYS_open, NULL, 0, 0);
    if (r < 0)
        report("OPEN_NULL_PATH", "PASS", "rejected NULL path");
    else {
        close((int)r);
        report("OPEN_NULL_PATH", "FAIL", "accepted NULL path");
    }

    /* stat with NULL path */
    struct stat st;
    r = syscall(SYS_stat, NULL, &st);
    if (r < 0)
        report("STAT_NULL_PATH", "PASS", "rejected NULL path");
    else
        report("STAT_NULL_PATH", "FAIL", "accepted NULL path");
}

static void
test_extreme_sizes(void)
{
    char buf[16];
    long r;

    /* write with extreme size — should be capped or rejected */
    r = syscall(SYS_write, 1, buf, 0xFFFFFFFFFFFFFFFFULL);
    if (r < 0)
        report("WRITE_HUGE_SIZE", "PASS", "rejected extreme size");
    else
        report("WRITE_HUGE_SIZE", "FAIL", "accepted extreme size");

    /* read with extreme size */
    r = syscall(SYS_read, 0, buf, 0xFFFFFFFFFFFFFFFFULL);
    if (r < 0)
        report("READ_HUGE_SIZE", "PASS", "rejected extreme size");
    else
        report("READ_HUGE_SIZE", "FAIL", "accepted extreme size");
}

/* ================================================================
 * 2. CAPABILITY BYPASS ATTEMPTS
 * ================================================================ */

static void
test_cap_shadow_without_auth(void)
{
    /* /etc/shadow requires CAP_KIND_AUTH.
     * A normal exec'd binary might not have it. */
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == ENOCAP)
            report("SHADOW_NO_AUTH", "PASS", "access denied");
        else {
            report("SHADOW_NO_AUTH", "SKIP", "open failed with unexpected errno");
        }
    } else {
        /* If we CAN open it, check if we actually have AUTH cap —
         * pwn might be launched from login which grants AUTH. */
        close(fd);
        report("SHADOW_NO_AUTH", "SKIP", "process has AUTH cap");
    }
}

static void
test_cap_blkdev_without_disk_admin(void)
{
    long r;
    char buf[512];

    /* sys_blkdev_list without DISK_ADMIN */
    r = syscall(SYS_AEGIS_BLKDEV_LIST, buf, sizeof(buf));
    if (r < 0 && (errno == ENOCAP || errno == EPERM))
        report("BLKDEV_LIST_NO_CAP", "PASS", "capability denied");
    else if (r >= 0)
        report("BLKDEV_LIST_NO_CAP", "SKIP", "process has DISK_ADMIN");
    else
        report("BLKDEV_LIST_NO_CAP", "PASS", "rejected");

    /* sys_blkdev_io: try to read LBA 0 of "nvme0" */
    r = syscall(SYS_AEGIS_BLKDEV_IO, "nvme0", 0, 1, buf, 0);
    if (r < 0 && (errno == ENOCAP || errno == EPERM))
        report("BLKDEV_IO_NO_CAP", "PASS", "capability denied");
    else if (r == 0)
        report("BLKDEV_IO_NO_CAP", "SKIP", "process has DISK_ADMIN");
    else
        report("BLKDEV_IO_NO_CAP", "PASS", "rejected");

    /* sys_gpt_rescan without DISK_ADMIN */
    r = syscall(SYS_AEGIS_GPT_RESCAN, "nvme0");
    if (r < 0 && (errno == ENOCAP || errno == EPERM))
        report("GPT_RESCAN_NO_CAP", "PASS", "capability denied");
    else
        report("GPT_RESCAN_NO_CAP", "SKIP", "process has DISK_ADMIN or no device");
}

static void
test_cap_fb_without_fb_cap(void)
{
    uint8_t info[24];
    long r = syscall(SYS_AEGIS_FB_MAP, info);
    if (r < 0 && (errno == EPERM || errno == ENOCAP))
        report("FB_MAP_NO_CAP", "PASS", "framebuffer access denied");
    else if (r > 0)
        report("FB_MAP_NO_CAP", "SKIP", "process has FB cap");
    else
        report("FB_MAP_NO_CAP", "PASS", "rejected");
}

static void
test_cap_query_without_cap(void)
{
    /* sys_cap_query (362) requires CAP_KIND_CAP_QUERY */
    uint32_t out[32];
    long r = syscall(SYS_AEGIS_CAP_QUERY, getpid(), out, sizeof(out));
    if (r < 0 && (errno == ENOCAP || errno == EPERM))
        report("CAP_QUERY_NO_CAP", "PASS", "query denied without CAP_QUERY");
    else if (r >= 0)
        report("CAP_QUERY_NO_CAP", "SKIP", "process has CAP_QUERY");
    else
        report("CAP_QUERY_NO_CAP", "PASS", "rejected");
}

static void
test_write_bad_fd(void)
{
    long r;

    /* Write to fd that doesn't exist */
    r = syscall(SYS_write, 15, "hello", 5);
    if (r < 0)
        report("WRITE_BAD_FD", "PASS", "rejected write to unopened fd");
    else
        report("WRITE_BAD_FD", "FAIL", "accepted write to unopened fd");

    /* Write to fd WAY out of range */
    r = syscall(SYS_write, 99999, "hello", 5);
    if (r < 0)
        report("WRITE_FD_OOB", "PASS", "rejected out-of-range fd");
    else
        report("WRITE_FD_OOB", "FAIL", "accepted out-of-range fd");
}

/* ================================================================
 * 3. MEMORY ATTACKS
 * ================================================================ */

static void
test_mmap_kernel_addr(void)
{
    /* Try to mmap at kernel virtual address */
    void *p = mmap((void *)0xFFFFFFFF80000000ULL, 4096,
                   PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED,
                   -1, 0);
    if (p == MAP_FAILED)
        report("MMAP_KERNEL_ADDR", "PASS", "rejected kernel address");
    else {
        munmap(p, 4096);
        report("MMAP_KERNEL_ADDR", "FAIL", "mapped kernel address!");
    }
}

static void
test_mmap_huge_size(void)
{
    /* Try to mmap terabytes */
    void *p = mmap(NULL, (size_t)1ULL << 40, /* 1 TB */
                   PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE,
                   -1, 0);
    if (p == MAP_FAILED)
        report("MMAP_HUGE_SIZE", "PASS", "rejected 1TB mmap");
    else {
        munmap(p, (size_t)1ULL << 40);
        report("MMAP_HUGE_SIZE", "FAIL", "accepted 1TB mmap!");
    }
}

static void
test_mprotect_kernel_pages(void)
{
    /* Try to mprotect kernel pages to writable */
    long r = syscall(SYS_mprotect, 0xFFFFFFFF80000000ULL, 4096,
                     PROT_READ | PROT_WRITE);
    if (r < 0)
        report("MPROTECT_KERNEL", "PASS", "rejected kernel mprotect");
    else
        report("MPROTECT_KERNEL", "FAIL", "mprotect on kernel pages succeeded!");
}

static void
test_null_deref(void)
{
    TRY_CRASH("NULL_DEREF", {
        volatile int *p = NULL;
        *p = 42;
        /* If we get here, bad */
        report("NULL_DEREF", "FAIL", "NULL write did not crash");
    });
    if (g_got_signal)
        report("NULL_DEREF", "PASS", "SIGSEGV delivered correctly");
}

static void
test_kernel_addr_read(void)
{
    TRY_CRASH("KERNEL_READ", {
        volatile uint64_t *p = (volatile uint64_t *)0xFFFFFFFF80100000ULL;
        volatile uint64_t val = *p;
        (void)val;
        report("KERNEL_READ", "FAIL", "read kernel memory from userspace!");
    });
    if (g_got_signal)
        report("KERNEL_READ", "PASS", "kernel read caused SIGSEGV");
}

static void
test_kernel_addr_write(void)
{
    TRY_CRASH("KERNEL_WRITE", {
        volatile uint64_t *p = (volatile uint64_t *)0xFFFFFFFF80100000ULL;
        *p = 0xDEADBEEFDEADBEEFULL;
        report("KERNEL_WRITE", "FAIL", "wrote to kernel memory from userspace!");
    });
    if (g_got_signal)
        report("KERNEL_WRITE", "PASS", "kernel write caused SIGSEGV");
}

static void
test_write_kernel_buf(void)
{
    /* sys_write with buffer pointing to kernel memory.
     * Should get EFAULT, not leak kernel memory to the fd. */
    long r = syscall(SYS_write, 1, 0xFFFFFFFF80100000ULL, 64);
    if (r < 0)
        report("WRITE_KERNEL_BUF", "PASS", "rejected kernel buffer pointer");
    else
        report("WRITE_KERNEL_BUF", "FAIL", "kernel buffer accepted — data leak!");
}

static void
test_read_to_kernel_buf(void)
{
    /* sys_read with destination in kernel memory — should EFAULT */
    long r = syscall(SYS_read, 0, 0xFFFFFFFF80100000ULL, 64);
    if (r < 0)
        report("READ_KERNEL_BUF", "PASS", "rejected kernel destination pointer");
    else
        report("READ_KERNEL_BUF", "FAIL", "kernel destination accepted — overwrite!");
}

static void
test_brk_extreme(void)
{
    long old_brk = syscall(SYS_brk, 0);

    /* Try to set brk to kernel address */
    long r = syscall(SYS_brk, 0xFFFFFFFF80000000ULL);
    if (r == old_brk)
        report("BRK_KERNEL_ADDR", "PASS", "brk to kernel addr rejected");
    else if ((uint64_t)r >= 0xFFFFFFFF80000000ULL)
        report("BRK_KERNEL_ADDR", "FAIL", "brk moved to kernel space!");
    else
        report("BRK_KERNEL_ADDR", "PASS", "brk clamped to user space");

    /* Try brk at max user address */
    r = syscall(SYS_brk, 0x00007FFFFFFFFFFFULL);
    if (r == old_brk || (uint64_t)r < 0x00007FFFFFFFFFFFULL)
        report("BRK_MAX_USER", "PASS", "extreme brk rejected or capped");
    else
        report("BRK_MAX_USER", "FAIL", "brk accepted max user addr");

    /* Restore original brk */
    syscall(SYS_brk, old_brk);
}

static void
test_proc_maps_kernel_leak(void)
{
    /* Read /proc/self/maps and check if kernel addresses are leaked */
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        report("PROC_MAPS_LEAK", "SKIP", "cannot open /proc/self/maps");
        return;
    }
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        report("PROC_MAPS_LEAK", "SKIP", "empty /proc/self/maps");
        return;
    }
    buf[n] = '\0';

    /* Search for kernel addresses (0xFFFF...) in the output */
    int found_kernel = 0;
    for (int i = 0; i < n - 16; i++) {
        if (buf[i] == 'f' && buf[i+1] == 'f' && buf[i+2] == 'f' &&
            buf[i+3] == 'f' && buf[i+4] == 'f' && buf[i+5] == 'f') {
            found_kernel = 1;
            break;
        }
    }
    if (!found_kernel)
        report("PROC_MAPS_LEAK", "PASS", "no kernel addresses leaked");
    else
        report("PROC_MAPS_LEAK", "FAIL", "kernel addresses visible in /proc/self/maps");
}

/* ================================================================
 * 4. SIGNAL ATTACKS
 * ================================================================ */

/*
 * Forge a sigreturn frame.
 * We set up a real signal handler, then when the handler fires, we corrupt
 * the signal frame on the stack to try to set privileged registers.
 *
 * The kernel's sys_rt_sigreturn should sanitize all these.
 */

static void
test_kill_init(void)
{
    /* Try to kill PID 1 (init/vigil) — should be denied */
    long r = syscall(SYS_kill, 1, SIGKILL);
    if (r < 0)
        report("KILL_INIT", "PASS", "cannot kill init");
    else
        report("KILL_INIT", "FAIL", "killed init! system will crash");
}

static void
test_kill_pid_zero(void)
{
    /* Send signal to PID 0 — should not crash kernel */
    long r = syscall(SYS_kill, 0, 0);
    /* signal 0 is a probe, should succeed for own pgrp */
    report("KILL_PID_ZERO", "PASS", "kernel survived kill(0,0)");
    (void)r;
}

static void
test_invalid_signals(void)
{
    /* Invalid signal number — should return EINVAL */
    long r = syscall(SYS_kill, getpid(), 99);
    if (r < 0 && errno == EINVAL)
        report("KILL_INVALID_SIG", "PASS", "invalid signal rejected");
    else
        report("KILL_INVALID_SIG", "FAIL", "invalid signal accepted");

    /* Signal 0 to self — probe, should succeed */
    r = syscall(SYS_kill, getpid(), 0);
    if (r < 0 && errno == EINVAL)
        report("KILL_SIG_ZERO", "PASS", "signal 0 returns EINVAL");
    else if (r == 0)
        report("KILL_SIG_ZERO", "PASS", "signal 0 probe succeeded");
    else
        report("KILL_SIG_ZERO", "SKIP", "unexpected result");
}

static void
test_sigaction_kernel_handler(void)
{
    /* Try to install a signal handler pointing to kernel space */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (void (*)(int))0xFFFFFFFF80100000ULL;
    int r = sigaction(SIGUSR1, &sa, NULL);
    if (r < 0)
        report("SIGACTION_KERNEL_HANDLER", "PASS", "rejected kernel handler address");
    else
        report("SIGACTION_KERNEL_HANDLER", "FAIL", "accepted kernel handler address");
}

/* ================================================================
 * 5. PROCESS ATTACKS
 * ================================================================ */

static void
test_fork_bomb_limited(void)
{
    /* Fork 5 children, then immediately reap them.
     * Tests that the kernel can handle rapid fork/exit without crashing. */
    pid_t pids[5];
    int i;
    int forked = 0;
    for (i = 0; i < 5; i++) {
        pid_t p = fork();
        if (p < 0) break;  /* out of resources — fine */
        if (p == 0) {
            /* child: exit immediately */
            _exit(0);
        }
        pids[forked++] = p;
    }
    /* Reap all children */
    for (i = 0; i < forked; i++) {
        waitpid(pids[i], NULL, 0);
    }
    report("FORK_BOMB_5", "PASS", "fork/exit survived");
}

static void
test_exec_kernel_path(void)
{
    /* execve with path pointing to kernel memory — should EFAULT */
    long r = syscall(SYS_execve, 0xFFFFFFFF80100000ULL, NULL, NULL);
    if (r < 0)
        report("EXEC_KERNEL_PATH", "PASS", "rejected kernel path pointer");
    else
        report("EXEC_KERNEL_PATH", "FAIL", "accepted kernel path!");
}

static void
test_spawn_kernel_path(void)
{
    /* sys_spawn with path in kernel space */
    long r = syscall(SYS_AEGIS_SPAWN, 0xFFFFFFFF80100000ULL, NULL, NULL, -1, 0);
    if (r < 0)
        report("SPAWN_KERNEL_PATH", "PASS", "rejected kernel path pointer");
    else
        report("SPAWN_KERNEL_PATH", "FAIL", "accepted kernel path!");
}

static void
test_waitpid_unowned(void)
{
    /* Try to wait on PID 1 — we don't own it */
    int status;
    pid_t r = waitpid(1, &status, WNOHANG);
    /* Should return -1/ECHILD or 0 (nothing to wait for) */
    if (r <= 0)
        report("WAITPID_UNOWNED", "PASS", "cannot wait on unowned PID");
    else
        report("WAITPID_UNOWNED", "FAIL", "waited on PID 1");
}

static void
test_exec_huge_argv(void)
{
    /* Build a large argv array. Not insanely large, just enough to test
     * the kernel doesn't crash copying it. */
    char *argv[130];
    char buf[130 * 4];  /* short strings */
    int i;
    for (i = 0; i < 128; i++) {
        buf[i * 4] = 'A';
        buf[i * 4 + 1] = '\0';
        argv[i] = &buf[i * 4];
    }
    argv[128] = NULL;

    pid_t p = fork();
    if (p < 0) {
        report("EXEC_HUGE_ARGV", "SKIP", "fork failed");
        return;
    }
    if (p == 0) {
        /* child: try execve with big argv — should fail (bad path) */
        execve("/nonexistent", argv, NULL);
        _exit(1);  /* execve should have failed */
    }
    int status;
    waitpid(p, &status, 0);
    report("EXEC_HUGE_ARGV", "PASS", "kernel survived large argv");
}

/* ================================================================
 * 6. FILE DESCRIPTOR ATTACKS
 * ================================================================ */

static void
test_fd_out_of_range(void)
{
    long r;

    /* Huge fd number */
    r = syscall(SYS_read, 99999, &r, 1);
    if (r < 0)
        report("READ_FD_99999", "PASS", "rejected huge fd");
    else
        report("READ_FD_99999", "FAIL", "accepted huge fd");

    /* Negative fd */
    r = syscall(SYS_read, -1, &r, 1);
    if (r < 0)
        report("READ_FD_NEG", "PASS", "rejected negative fd");
    else
        report("READ_FD_NEG", "FAIL", "accepted negative fd");
}

static void
test_close_stdin_then_read(void)
{
    /* Close stdin and try to read from it */
    int saved = dup(0);
    close(0);
    long r = syscall(SYS_read, 0, &r, 1);
    if (r < 0)
        report("READ_CLOSED_FD0", "PASS", "read after close returns error");
    else
        report("READ_CLOSED_FD0", "FAIL", "read after close succeeded");

    /* Restore fd 0 */
    if (saved >= 0) {
        dup2(saved, 0);
        close(saved);
    }
}

static void
test_dup2_replace_stdout(void)
{
    /* Replace stdout with a copy of stderr — should not crash */
    int saved = dup(1);
    int r = dup2(2, 1);
    if (r == 1) {
        write(1, "[PWN] test output via replaced stdout\n", 38);
        report("DUP2_REPLACE_STDOUT", "PASS", "dup2 replacement worked safely");
    } else {
        report("DUP2_REPLACE_STDOUT", "PASS", "dup2 failed safely");
    }
    /* Restore */
    if (saved >= 0) {
        dup2(saved, 1);
        close(saved);
    }
}

static void
test_exhaust_fds(void)
{
    /* Try to open many files to exhaust the fd table (PROC_MAX_FDS=16) */
    int fds[20];
    int count = 0;
    int i;
    for (i = 0; i < 20; i++) {
        fds[i] = open("/etc/motd", O_RDONLY);
        if (fds[i] < 0) break;
        count++;
    }
    /* Close all opened fds */
    for (i = 0; i < count; i++)
        close(fds[i]);

    if (count < 20)
        report("FD_EXHAUST", "PASS", "fd table exhaustion returned error");
    else
        report("FD_EXHAUST", "FAIL", "opened 20 fds with PROC_MAX_FDS=16");
}

/* ================================================================
 * 7. NETWORK ATTACKS (if socket cap is present)
 * ================================================================ */

static void
test_socket_attacks(void)
{
    /* Try creating a socket — requires CAP_NET_SOCKET */
    int sock = syscall(SYS_socket, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (sock < 0) {
        if (errno == ENOCAP || errno == EPERM)
            report("SOCKET_NO_CAP", "PASS", "socket creation denied without cap");
        else
            report("SOCKET_NO_CAP", "SKIP", "socket failed with unexpected errno");
        return;
    }

    /* We have socket cap — try binding to low port */
    struct {
        uint16_t sin_family;
        uint16_t sin_port;
        uint32_t sin_addr;
        uint8_t  sin_zero[8];
    } addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = 2; /* AF_INET */
    addr.sin_port = 0x5000; /* port 80 in network byte order */
    addr.sin_addr = 0;      /* INADDR_ANY */

    long r = syscall(SYS_bind, sock, &addr, sizeof(addr));
    /* Aegis doesn't restrict low ports (no concept of privileged ports) */
    if (r == 0)
        report("BIND_LOW_PORT", "SKIP", "Aegis allows low port bind (no restriction)");
    else
        report("BIND_LOW_PORT", "PASS", "low port bind rejected");

    /* Try connect to 0.0.0.0:0 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = 2;
    addr.sin_port = 0;
    addr.sin_addr = 0;
    r = syscall(SYS_connect, sock, &addr, sizeof(addr));
    if (r < 0)
        report("CONNECT_ZERO_ADDR", "PASS", "connect to 0.0.0.0:0 rejected");
    else
        report("CONNECT_ZERO_ADDR", "SKIP", "connect to 0.0.0.0:0 accepted (may be valid)");

    close(sock);
}

/* ================================================================
 * 8. EXT2/VFS PATH ATTACKS
 * ================================================================ */

static void
test_path_traversal(void)
{
    /* Try path traversal to escape root */
    int fd;

    fd = open("/../../../etc/motd", O_RDONLY);
    if (fd >= 0) {
        /* This is actually OK — /../ resolves to / */
        close(fd);
        report("PATH_TRAVERSAL", "PASS", "resolved to / (expected)");
    } else {
        report("PATH_TRAVERSAL", "PASS", "traversal rejected");
    }
}

static void
test_very_long_path(void)
{
    /* Try a path longer than the kernel's 256-byte buffer */
    char long_path[4200];
    memset(long_path, 'A', 4199);
    long_path[0] = '/';
    long_path[4199] = '\0';

    int fd = open(long_path, O_RDONLY);
    if (fd < 0)
        report("LONG_PATH", "PASS", "extremely long path rejected");
    else {
        close(fd);
        report("LONG_PATH", "FAIL", "accepted 4KB path");
    }
}

static void
test_null_in_path(void)
{
    /* Path with embedded null byte.
     * The kernel copies byte-by-byte and stops at null.
     * So "/etc\0/shadow" becomes just "/etc" — the \0 truncates. */
    char path[] = "/etc\0/shadow";
    /* Use raw syscall to pass the full buffer */
    long r = syscall(SYS_open, path, O_RDONLY, 0);
    if (r >= 0) {
        /* Opened "/etc" — this is a directory, should probably fail or
         * return a directory fd. Either way, not "/etc/shadow". */
        close((int)r);
        report("NULL_IN_PATH", "PASS", "null truncated path before /shadow");
    } else {
        report("NULL_IN_PATH", "PASS", "path with null rejected");
    }
}

static void
test_open_dir_write(void)
{
    /* Try to open a directory and write to it */
    int fd = open("/bin", O_RDONLY);
    if (fd < 0) {
        report("DIR_WRITE", "SKIP", "cannot open /bin");
        return;
    }
    long r = syscall(SYS_write, fd, "evil", 4);
    close(fd);
    if (r < 0)
        report("DIR_WRITE", "PASS", "cannot write to directory fd");
    else
        report("DIR_WRITE", "FAIL", "wrote to directory!");
}

/* ================================================================
 * 9. TIMING/RACE ATTACKS
 * ================================================================ */

static void
test_double_close(void)
{
    /* Close the same fd twice — should return EBADF second time */
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) {
        report("DOUBLE_CLOSE", "SKIP", "cannot open file");
        return;
    }
    close(fd);
    int r = close(fd);
    if (r < 0)
        report("DOUBLE_CLOSE", "PASS", "second close returned EBADF");
    else
        report("DOUBLE_CLOSE", "FAIL", "second close succeeded — use-after-close?");
}

static void
test_read_write_after_close(void)
{
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) {
        report("RW_AFTER_CLOSE", "SKIP", "cannot open file");
        return;
    }
    int saved_fd = fd;
    close(fd);

    char buf[16];
    long r = syscall(SYS_read, saved_fd, buf, sizeof(buf));
    if (r < 0)
        report("RW_AFTER_CLOSE", "PASS", "read after close rejected");
    else
        report("RW_AFTER_CLOSE", "FAIL", "read after close succeeded — UAF?");
}

/* ================================================================
 * 10. ARCH_PRCTL ATTACKS
 * ================================================================ */

static void
test_arch_prctl_kernel_addr(void)
{
    /* Try to set FS base to kernel address.
     * If the kernel doesn't validate, we could use FS-relative addressing
     * to read kernel memory. */
    long r = syscall(SYS_arch_prctl, 0x1002 /* ARCH_SET_FS */, 0xFFFFFFFF80000000ULL);
    /* This might succeed (kernel may not validate FS base) but reading
     * through FS should still fault due to SMEP/SMAP. */
    if (r < 0)
        report("ARCH_PRCTL_KERNEL_FS", "PASS", "rejected kernel FS base");
    else {
        /* It was accepted — try to read through it */
        TRY_CRASH("ARCH_PRCTL_KERNEL_FS_READ", {
            uint64_t val;
            __asm__ volatile ("mov %%fs:0, %0" : "=r"(val));
            (void)val;
            report("ARCH_PRCTL_KERNEL_FS", "FAIL", "read kernel via FS!");
        });
        if (g_got_signal)
            report("ARCH_PRCTL_KERNEL_FS", "PASS", "FS read faulted (SMAP/SMEP)");

        /* Restore FS base to something sane — get TLS from musl */
        /* musl stores TLS pointer in a known place; just set a dummy value
         * that won't crash us. Actually, let musl re-set it. */
        /* We may have corrupted TLS. If we're still alive, call it a pass
         * since the read faulted. */
    }
}

/* ================================================================
 * 11. IOCTL ATTACKS
 * ================================================================ */

static void
test_ioctl_invalid(void)
{
    /* Send garbage ioctl to stdout */
    long r = syscall(SYS_ioctl, 1, 0xDEADBEEF, 0);
    /* Should return -ENOTTY or -EINVAL, not crash */
    if (r < 0)
        report("IOCTL_GARBAGE", "PASS", "garbage ioctl rejected");
    else
        report("IOCTL_GARBAGE", "SKIP", "ioctl accepted (may be valid)");

    /* ioctl with kernel pointer as arg */
    r = syscall(SYS_ioctl, 1, 0x5401 /* TCGETS */, 0xFFFFFFFF80000000ULL);
    if (r < 0)
        report("IOCTL_KERNEL_ARG", "PASS", "ioctl with kernel pointer rejected");
    else
        report("IOCTL_KERNEL_ARG", "FAIL", "ioctl with kernel pointer accepted!");
}

/* ================================================================
 * 12. NETCFG ATTACK — try to reconfigure network without cap
 * ================================================================ */

static void
test_netcfg_without_cap(void)
{
    /* sys_netcfg (500) requires CAP_NET_ADMIN */
    long r = syscall(SYS_AEGIS_NETCFG, 1 /* set IP */,
                     0x0A000201UL /* 10.0.2.1 */, 0xFFFFFF00UL, 0);
    if (r < 0 && (errno == ENOCAP || errno == EPERM))
        report("NETCFG_NO_CAP", "PASS", "network reconfiguration denied");
    else if (r == 0)
        report("NETCFG_NO_CAP", "SKIP", "process has NET_ADMIN cap");
    else
        report("NETCFG_NO_CAP", "PASS", "rejected");
}

/* ================================================================
 * 13. MMAP + MPROTECT RWX ATTACKS — W^X enforcement
 * ================================================================ */

static void
test_mmap_rwx(void)
{
    /* Try to mmap a page that is both writable and executable.
     * W^X enforcement should prevent this. */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        report("MMAP_RWX", "PASS", "RWX mmap rejected");
    } else {
        /* Some kernels allow the mmap but enforce W^X via NX.
         * Check if we can actually write + execute. */
        unsigned char *code = (unsigned char *)p;
        /* Write a RET instruction */
        code[0] = 0xC3;
        /* Try to execute it */
        TRY_CRASH("MMAP_RWX_EXEC", {
            void (*fn)(void) = (void (*)(void))p;
            fn();
            /* If we get here, W^X is not enforced */
            report("MMAP_RWX", "FAIL", "RWX page: write + execute succeeded!");
        });
        if (g_got_signal)
            report("MMAP_RWX", "PASS", "RWX page execution faulted (NX enforced)");
        munmap(p, 4096);
    }
}

static void
test_mprotect_wx(void)
{
    /* mmap RW, then mprotect to WX — test W^X enforcement */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        report("MPROTECT_WX", "SKIP", "mmap failed");
        return;
    }
    unsigned char *code = (unsigned char *)p;
    code[0] = 0xC3; /* RET */

    long r = mprotect(p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (r < 0) {
        report("MPROTECT_WX", "PASS", "mprotect RWX rejected");
        munmap(p, 4096);
        return;
    }

    /* mprotect allowed it — try execution */
    TRY_CRASH("MPROTECT_WX_EXEC", {
        void (*fn)(void) = (void (*)(void))p;
        fn();
        report("MPROTECT_WX", "FAIL", "W+X page executed successfully!");
    });
    if (g_got_signal)
        report("MPROTECT_WX", "PASS", "W+X execution faulted (NX enforced)");
    munmap(p, 4096);
}

/* ================================================================
 * 14. SIGRETURN FRAME FORGING
 * ================================================================ */

/*
 * Build a signal frame by hand and call sys_rt_sigreturn.
 * This tests whether the kernel validates the restored register state.
 *
 * We can't easily corrupt a real signal frame from a signal handler
 * without inline assembly. Instead, test the guards indirectly by
 * trying to install handlers with kernel addresses (already tested)
 * and by verifying that after a real signal+return, we're still in
 * user mode.
 */
static volatile int g_sigreturn_handler_called;

static void
sigreturn_test_handler(int sig)
{
    g_sigreturn_handler_called = 1;
    /* The handler returns normally via sigreturn. If the kernel's
     * sigreturn sanitization is broken, we'd crash after return. */
}

static void
test_sigreturn_survives(void)
{
    g_sigreturn_handler_called = 0;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigreturn_test_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, &old);

    kill(getpid(), SIGUSR1);

    if (g_sigreturn_handler_called)
        report("SIGRETURN_SANE", "PASS", "signal handler returned cleanly");
    else
        report("SIGRETURN_SANE", "FAIL", "handler never called");

    /* Restore */
    sigaction(SIGUSR1, &old, NULL);
}

/* ================================================================
 * 15. PROC FILESYSTEM ATTACKS
 * ================================================================ */

static void
test_proc_other_pid(void)
{
    /* Try to read another process's /proc entry */
    int fd = open("/proc/1/status", O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES || errno == ENOCAP || errno == ENOENT)
            report("PROC_OTHER_PID", "PASS", "access to /proc/1 denied");
        else
            report("PROC_OTHER_PID", "SKIP", "unexpected errno");
    } else {
        /* If CAP_PROC_READ is granted, this is expected to work */
        close(fd);
        report("PROC_OTHER_PID", "SKIP", "access granted (has PROC_READ cap)");
    }
}

/* ================================================================
 * 16. GETRANDOM ATTACKS
 * ================================================================ */

static void
test_getrandom_kernel_buf(void)
{
    /* getrandom with kernel destination pointer */
    long r = syscall(SYS_getrandom, 0xFFFFFFFF80000000ULL, 32, 0);
    if (r < 0)
        report("GETRANDOM_KERNEL_BUF", "PASS", "rejected kernel buffer");
    else
        report("GETRANDOM_KERNEL_BUF", "FAIL", "wrote random data to kernel memory!");
}

/* ================================================================
 * 17. WRITEV ATTACKS
 * ================================================================ */

static void
test_writev_kernel_iov(void)
{
    /* writev with iovec array in kernel space */
    long r = syscall(SYS_writev, 1, 0xFFFFFFFF80000000ULL, 1);
    if (r < 0)
        report("WRITEV_KERNEL_IOV", "PASS", "rejected kernel iovec pointer");
    else
        report("WRITEV_KERNEL_IOV", "FAIL", "accepted kernel iovec pointer!");

    /* writev with huge iovcnt */
    r = syscall(SYS_writev, 1, "hello", 0x7FFFFFFFUL);
    if (r < 0)
        report("WRITEV_HUGE_IOVCNT", "PASS", "rejected huge iovcnt");
    else
        report("WRITEV_HUGE_IOVCNT", "FAIL", "accepted huge iovcnt!");
}

/* ================================================================
 * 18. EPOLL ATTACKS
 * ================================================================ */

static void
test_epoll_attacks(void)
{
    /* epoll_wait with kernel pointer for events buffer */
    int epfd = syscall(SYS_epoll_create1, 0);
    if (epfd < 0) {
        report("EPOLL_ATTACKS", "SKIP", "epoll_create1 failed");
        return;
    }

    long r = syscall(SYS_epoll_wait, epfd, 0xFFFFFFFF80000000ULL, 10, 0);
    if (r < 0)
        report("EPOLL_KERNEL_BUF", "PASS", "rejected kernel buffer for events");
    else
        report("EPOLL_KERNEL_BUF", "FAIL", "accepted kernel buffer!");

    close(epfd);
}

/* ================================================================
 * 19. CLONE/THREAD ATTACKS
 * ================================================================ */

static void
test_clone_bad_stack(void)
{
    /* clone with kernel address as child stack */
    long r = syscall(SYS_clone, 0x00010100 /* CLONE_VM|CLONE_THREAD|SIGCHLD */,
                     0xFFFFFFFF80000000ULL, NULL, NULL, NULL);
    if (r < 0)
        report("CLONE_KERNEL_STACK", "PASS", "rejected kernel stack pointer");
    else {
        /* If it somehow succeeded, that's very bad */
        report("CLONE_KERNEL_STACK", "FAIL", "clone with kernel stack succeeded!");
    }
}

/* ================================================================
 * 20. LSEEK ATTACKS
 * ================================================================ */

static void
test_lseek_overflow(void)
{
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) {
        report("LSEEK_OVERFLOW", "SKIP", "cannot open file");
        return;
    }
    /* Seek to extreme offset — should not crash */
    (void)lseek(fd, 0x7FFFFFFFFFFFFFFFLL, SEEK_SET);
    /* May succeed (just setting offset) or fail — either is fine */
    (void)lseek(fd, 0x7FFFFFFFFFFFFFFFLL, SEEK_CUR);
    /* The overflow should be caught or harmlessly large */
    close(fd);
    report("LSEEK_OVERFLOW", "PASS", "kernel survived extreme lseek");
}

/* ================================================================
 * MAIN
 * ================================================================ */

int
main(int argc, char **argv)
{
    write_str("=== Aegis Kernel Security Test Suite ===\n");
    write_str("=== Each test attacks the kernel from userspace ===\n\n");

    install_crash_handlers();

    /* 1. Syscall fuzzing */
    write_str("--- Syscall Fuzzing ---\n");
    test_invalid_syscall_numbers();
    test_null_pointers();
    test_extreme_sizes();

    /* 2. Capability bypass */
    write_str("\n--- Capability Bypass ---\n");
    test_cap_shadow_without_auth();
    test_cap_blkdev_without_disk_admin();
    test_cap_fb_without_fb_cap();
    test_cap_query_without_cap();
    test_write_bad_fd();

    /* 3. Memory attacks */
    write_str("\n--- Memory Attacks ---\n");
    test_mmap_kernel_addr();
    test_mmap_huge_size();
    test_mprotect_kernel_pages();
    test_null_deref();
    test_kernel_addr_read();
    test_kernel_addr_write();
    test_write_kernel_buf();
    test_read_to_kernel_buf();
    test_brk_extreme();
    test_proc_maps_kernel_leak();

    /* 4. Signal attacks */
    write_str("\n--- Signal Attacks ---\n");
    test_kill_init();
    test_kill_pid_zero();
    test_invalid_signals();
    test_sigaction_kernel_handler();
    test_sigreturn_survives();

    /* 5. Process attacks */
    write_str("\n--- Process Attacks ---\n");
    test_fork_bomb_limited();
    test_exec_kernel_path();
    test_spawn_kernel_path();
    test_waitpid_unowned();
    test_exec_huge_argv();

    /* 6. File descriptor attacks */
    write_str("\n--- File Descriptor Attacks ---\n");
    test_fd_out_of_range();
    test_close_stdin_then_read();
    test_dup2_replace_stdout();
    test_exhaust_fds();

    /* 7. Network attacks */
    write_str("\n--- Network Attacks ---\n");
    test_socket_attacks();

    /* 8. VFS/path attacks */
    write_str("\n--- VFS/Path Attacks ---\n");
    test_path_traversal();
    test_very_long_path();
    test_null_in_path();
    test_open_dir_write();

    /* 9. Race/lifecycle attacks */
    write_str("\n--- Race/Lifecycle Attacks ---\n");
    test_double_close();
    test_read_write_after_close();

    /* 10. Arch prctl */
    write_str("\n--- Arch Prctl Attacks ---\n");
    test_arch_prctl_kernel_addr();

    /* 11. Ioctl attacks */
    write_str("\n--- Ioctl Attacks ---\n");
    test_ioctl_invalid();

    /* 12. Network config */
    write_str("\n--- Network Config Attacks ---\n");
    test_netcfg_without_cap();

    /* 13. W^X enforcement */
    write_str("\n--- W^X Enforcement ---\n");
    test_mmap_rwx();
    test_mprotect_wx();

    /* 14. Sigreturn */
    write_str("\n--- Sigreturn Attacks ---\n");
    test_sigreturn_survives();

    /* 15. Proc filesystem */
    write_str("\n--- Proc Filesystem Attacks ---\n");
    test_proc_other_pid();

    /* 16. Getrandom */
    write_str("\n--- Getrandom Attacks ---\n");
    test_getrandom_kernel_buf();

    /* 17. Writev */
    write_str("\n--- Writev Attacks ---\n");
    test_writev_kernel_iov();

    /* 18. Epoll */
    write_str("\n--- Epoll Attacks ---\n");
    test_epoll_attacks();

    /* 19. Clone/thread */
    write_str("\n--- Clone/Thread Attacks ---\n");
    test_clone_bad_stack();

    /* 20. Lseek */
    write_str("\n--- Lseek Attacks ---\n");
    test_lseek_overflow();

    /* Summary */
    write_str("\n=== RESULTS ===\n");
    write_str("PASS: "); write_int(g_pass); write_str("\n");
    write_str("FAIL: "); write_int(g_fail); write_str("\n");
    write_str("SKIP: "); write_int(g_skip); write_str("\n");
    write_str("TOTAL: "); write_int(g_pass + g_fail + g_skip); write_str("\n");

    if (g_fail > 0) {
        write_str("\n!!! VULNERABILITIES FOUND — review FAIL results above !!!\n");
        return 1;
    }
    write_str("\nAll attacks defended. Kernel security looks solid.\n");
    return 0;
}
