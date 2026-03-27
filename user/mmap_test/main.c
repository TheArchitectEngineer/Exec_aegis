#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static volatile int got_signal = 0;
static sigjmp_buf jmpbuf;

static void segv_handler(int sig)
{
    (void)sig;
    got_signal = 1;
    siglongjmp(jmpbuf, 1);
}

int main(void)
{
    /* Test 1: VA reuse — munmap then mmap should recycle the address. */
    void *a1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a1 == MAP_FAILED) {
        printf("MMAP FAIL: mmap1\n");
        return 1;
    }
    *(volatile int *)a1 = 99;
    munmap(a1, 4096);

    void *a2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a2 == MAP_FAILED) {
        printf("MMAP FAIL: mmap2\n");
        return 1;
    }
    if (a2 != a1) {
        printf("MMAP FAIL: VA not reused a1=%p a2=%p\n", a1, a2);
        return 1;
    }

    /* Test 2: mprotect PROT_NONE — write should SIGSEGV. */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        printf("MMAP FAIL: mmap3\n");
        return 1;
    }
    *(volatile int *)p = 42;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = segv_handler;
    sa.sa_flags   = 0;
    sigaction(SIGSEGV, &sa, NULL);

    mprotect(p, 4096, PROT_NONE);
    got_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        *(volatile int *)p = 0;  /* should fault */
        printf("MMAP FAIL: PROT_NONE did not fault\n");
        return 1;
    }
    if (!got_signal) {
        printf("MMAP FAIL: no SIGSEGV for PROT_NONE\n");
        return 1;
    }

    /* Test 3: mprotect read-only — read should work, write should SIGSEGV. */
    void *r = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (r == MAP_FAILED) {
        printf("MMAP FAIL: mmap4\n");
        return 1;
    }
    *(volatile int *)r = 123;
    mprotect(r, 4096, PROT_READ);

    /* Read should succeed. */
    volatile int val = *(volatile int *)r;
    if (val != 123) {
        printf("MMAP FAIL: read-only read got %d\n", val);
        return 1;
    }

    /* Write should fault. */
    got_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        *(volatile int *)r = 0;  /* should fault */
        printf("MMAP FAIL: PROT_READ did not fault on write\n");
        return 1;
    }
    if (!got_signal) {
        printf("MMAP FAIL: no SIGSEGV for PROT_READ write\n");
        return 1;
    }

    printf("MMAP OK\n");
    return 0;
}
