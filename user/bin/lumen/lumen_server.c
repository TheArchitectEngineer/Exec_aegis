/* lumen_server.c — Lumen external window protocol server */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <glyph.h>
#include "compositor.h"
#include "lumen_server.h"
#include "lumen_proto.h"

int lumen_server_init(void) { return -1; }

int lumen_server_tick(compositor_t *comp, int listen_fd)
{
    (void)comp; (void)listen_fd;
    return 0;
}
