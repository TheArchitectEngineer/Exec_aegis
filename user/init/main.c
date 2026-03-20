/* Freestanding user-space init binary — no libc, no headers */

static inline long
sys_write(int fd, const char *buf, long len)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline void
sys_exit(int code)
{
    __asm__ volatile ("syscall"
        : : "a"(60L), "D"((long)code) : "rcx", "r11");
    __builtin_unreachable();
}

void
_start(void)
{
    int i;
    for (i = 0; i < 3; i++)
        sys_write(1, "[USER] hello from ring 3\n", 25);
    sys_write(1, "[USER] done\n", 12);
    sys_exit(0);
}
