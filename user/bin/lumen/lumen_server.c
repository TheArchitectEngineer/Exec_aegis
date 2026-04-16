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

#define LUMEN_MAX_CLIENTS            8
#define LUMEN_MAX_WINDOWS_PER_CLIENT 8

typedef struct proxy_window proxy_window_t;

typedef struct {
    int             fd;
    proxy_window_t *windows[LUMEN_MAX_WINDOWS_PER_CLIENT];
    int             nwindows;
    uint32_t        next_id;
} lumen_client_t;

struct proxy_window {
    glyph_window_t *win;
    lumen_client_t *client;
    uint32_t        id;
    int             memfd;
    void           *shared;
};

static lumen_client_t *s_clients[LUMEN_MAX_CLIENTS];
static int              s_ncli;

static int lumen_server_read(compositor_t *comp, lumen_client_t *cli)
{
    (void)comp; (void)cli;
    return 0;
}

static void lumen_server_hangup(compositor_t *comp, lumen_client_t *cli)
{
    (void)comp;
    close(cli->fd);
    for (int i = 0; i < s_ncli; i++) {
        if (s_clients[i] == cli) {
            s_clients[i] = s_clients[--s_ncli];
            break;
        }
    }
    free(cli);
}

static void lumen_server_accept_fd(compositor_t *comp, int listen_fd)
{
    (void)comp;

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;

    fcntl(fd, F_SETFL, O_NONBLOCK);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) <= 0) { close(fd); return; }

    lumen_hello_t hello;
    if (read(fd, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
        close(fd);
        return;
    }

    lumen_hello_reply_t reply;
    reply.magic   = LUMEN_MAGIC;
    reply.version = LUMEN_VERSION;

    if (hello.magic != LUMEN_MAGIC || hello.version != LUMEN_VERSION) {
        reply.status = 1;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    if (s_ncli >= LUMEN_MAX_CLIENTS) {
        reply.status = 2;
        write(fd, &reply, sizeof(reply));
        close(fd);
        return;
    }

    reply.status = 0;
    write(fd, &reply, sizeof(reply));

    lumen_client_t *cli = calloc(1, sizeof(*cli));
    if (!cli) { close(fd); return; }
    cli->fd      = fd;
    cli->next_id = 1;
    s_clients[s_ncli++] = cli;
}

int lumen_server_init(void)
{
    mkdir("/run", 0755);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    unlink("/run/lumen.sock");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);

    return fd;
}

int lumen_server_tick(compositor_t *comp, int listen_fd)
{
    int dirtied = 0;

    {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0)
            lumen_server_accept_fd(comp, listen_fd);
    }

    for (int i = 0; i < s_ncli; ) {
        struct pollfd pfd = { .fd = s_clients[i]->fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0) {
            int r = lumen_server_read(comp, s_clients[i]);
            if (r < 0) {
                lumen_server_hangup(comp, s_clients[i]);
                continue;
            }
            if (r > 0)
                dirtied = 1;
        }
        i++;
    }

    return dirtied;
}
