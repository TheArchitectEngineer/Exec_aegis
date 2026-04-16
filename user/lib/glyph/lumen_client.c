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

int lumen_connect(void) { return -ENOSYS; }

lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    (void)fd; (void)title; (void)w; (void)h;
    return NULL;
}

void lumen_window_present(lumen_window_t *win) { (void)win; }
int  lumen_poll_event(int fd, lumen_event_t *ev) { (void)fd; (void)ev; return 0; }
int  lumen_wait_event(int fd, lumen_event_t *ev, int t) { (void)fd; (void)ev; (void)t; return 0; }
void lumen_window_destroy(lumen_window_t *win) { (void)win; }
