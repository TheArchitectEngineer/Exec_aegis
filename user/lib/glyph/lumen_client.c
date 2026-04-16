/* lumen_client.c — Lumen external window protocol client implementation */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "lumen_client.h"

int lumen_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }

    lumen_hello_t hello = { LUMEN_MAGIC, LUMEN_VERSION };
    if (write(fd, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        close(fd); return -EIO;
    }

    lumen_hello_reply_t reply;
    if (read(fd, &reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
        close(fd); return -EIO;
    }
    if (reply.magic != LUMEN_MAGIC || reply.status != 0) {
        close(fd); return -EPROTO;
    }

    return fd;
}

lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    (void)fd; (void)title; (void)w; (void)h;
    return NULL;
}

void lumen_window_present(lumen_window_t *win) { (void)win; }
int  lumen_poll_event(int fd, lumen_event_t *ev) { (void)fd; (void)ev; return 0; }
int  lumen_wait_event(int fd, lumen_event_t *ev, int t) { (void)fd; (void)ev; (void)t; return 0; }
void lumen_window_destroy(lumen_window_t *win) { (void)win; }
