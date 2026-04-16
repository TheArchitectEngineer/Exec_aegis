/* lumen_server.h — Lumen external window server interface */
#ifndef LUMEN_SERVER_H
#define LUMEN_SERVER_H

#include "compositor.h"

int lumen_server_init(void);
int lumen_server_tick(compositor_t *comp, int listen_fd);

/* Notify a proxy window of focus change. win may be NULL or non-proxy (no-op). */
void lumen_proxy_notify_focus(glyph_window_t *win, int focused);

#endif /* LUMEN_SERVER_H */
