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

static inline long
sys_open(const char *path, long flags, long mode)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(2L), "D"((long)path), "S"(flags), "d"(mode)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_read(long fd, char *buf, long len)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(0L), "D"(fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_close(long fd)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(3L), "D"(fd)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long
sys_brk(long addr)
{
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(12L), "D"(addr)
        : "rcx", "r11", "memory");
    return ret;
}

void
_start(void)
{
    /* Read and print /etc/motd (tests VFS + sys_read + sys_write). */
    char buf[64];
    long fd = sys_open("/etc/motd", 0, 0);
    long n  = sys_read(fd, buf, (long)sizeof(buf));
    sys_write(1, buf, n);
    sys_close(fd);

    /* Test sys_brk: allocate one heap page, write a string, print it.
     * Uses byte-by-byte copy — no memcpy/strcpy (freestanding, no libc). */
    long heap = sys_brk(0);        /* query current brk */
    sys_brk(heap + 4096);          /* allocate one page */
    char *p = (char *)heap;
    const char msg[] = "[HEAP] OK: brk works\n";
    int i;
    for (i = 0; msg[i]; i++)
        p[i] = msg[i];
    sys_write(1, p, i);

    sys_exit(0);
}
