#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

int main(void)
{
    /* Test 1: posix_openpt + grantpt + unlockpt + ptsname */
    int master = posix_openpt(O_RDWR);
    if (master < 0) {
        printf("PTY FAIL: posix_openpt\n");
        return 1;
    }
    if (grantpt(master) != 0) {
        printf("PTY FAIL: grantpt\n");
        return 1;
    }
    if (unlockpt(master) != 0) {
        printf("PTY FAIL: unlockpt\n");
        return 1;
    }
    char *name = ptsname(master);
    if (!name) {
        printf("PTY FAIL: ptsname returned NULL\n");
        return 1;
    }
    if (strncmp(name, "/dev/pts/", 9) != 0) {
        printf("PTY FAIL: ptsname='%s'\n", name);
        return 1;
    }

    /* Test 2: fork, child opens slave and writes, parent reads from master */
    pid_t pid = fork();
    if (pid < 0) {
        printf("PTY FAIL: fork\n");
        return 1;
    }
    if (pid == 0) {
        /* Child */
        close(master);
        int slave = open(name, O_RDWR);
        if (slave < 0) _exit(1);
        write(slave, "hello\n", 6);
        close(slave);
        _exit(0);
    }

    /* Parent: read from master */
    usleep(200000); /* give child time to write */
    char buf[128];
    int n = read(master, buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
    else buf[0] = '\0';

    int status;
    waitpid(pid, &status, 0);

    if (n <= 0 || !strstr(buf, "hello")) {
        printf("PTY FAIL: master read got %d bytes: '%s'\n", n, buf);
        close(master);
        return 1;
    }

    close(master);
    printf("PTY OK\n");
    return 0;
}
