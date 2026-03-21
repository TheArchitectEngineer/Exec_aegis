#include "syscall.h"
#include "sched.h"
#include "printk.h"
#include "arch.h"
#include <stdint.h>

/* USER_ADDR_MAX — highest canonical user-space virtual address (x86-64).
 * Phase 9 forward-looking: move to arch.h when a second syscall file needs it. */
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFUL

/* user_ptr_valid — return 1 if [addr, addr+len) lies entirely within the
 * canonical user address space, 0 otherwise.
 * For len=0, validates that addr itself is a canonical user address (does
 * NOT unconditionally pass — a kernel addr with len=0 still returns 0).
 * Overflow-safe: addr <= USER_ADDR_MAX - len avoids addr+len wraparound. */
static inline int
user_ptr_valid(uint64_t addr, uint64_t len)
{
    return len <= USER_ADDR_MAX && addr <= USER_ADDR_MAX - len;
}

/*
 * sys_write — syscall 1
 *
 * arg1 = fd (ignored: all output goes to printk)
 * arg2 = user virtual address of buffer
 * arg3 = byte count
 *
 * Returns byte count on success, -14 (EFAULT) if [arg2, arg2+arg3) is not
 * a canonical user-space range.
 *
 * stac/clac bracket each individual byte load (narrow window).
 * IF=0 (SFMASK) throughout syscall dispatch, so no interrupt can fire
 * with RFLAGS.AC set. The per-character pattern is intentionally
 * conservative — Phase 9 replaces this with a copy_from_user approach
 * once a kernel scratch buffer exists.
 */
static uint64_t
sys_write(uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    if (!user_ptr_valid(arg2, arg3))
        return (uint64_t)-14;   /* EFAULT */
    const char *s = (const char *)(uintptr_t)arg2;
    uint64_t i;
    for (i = 0; i < arg3; i++) {
        char c;
        /* Narrow stac/clac window: bracket only the single user-memory load.
         * Never call functions between arch_stac() and arch_clac(). */
        arch_stac();
        c = s[i];
        arch_clac();
        printk("%c", c);
    }
    return arg3;
}

/*
 * sys_exit — syscall 60
 *
 * arg1 = exit code (ignored for Phase 5)
 * Calls sched_exit() which never returns.
 */
static uint64_t
sys_exit(uint64_t arg1)
{
    (void)arg1;
    sched_exit();
    __builtin_unreachable();
}

uint64_t
syscall_dispatch(uint64_t num, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3)
{
    switch (num) {
    case 1:  return sys_write(arg1, arg2, arg3);
    case 60: return sys_exit(arg1);
    default: return (uint64_t)-1;   /* ENOSYS */
    }
}
