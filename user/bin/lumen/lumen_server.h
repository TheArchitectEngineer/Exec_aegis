/* lumen_server.h — Lumen external window server interface */
#ifndef LUMEN_SERVER_H
#define LUMEN_SERVER_H

#include "compositor.h"

int lumen_server_init(void);
int lumen_server_tick(compositor_t *comp, int listen_fd);

#endif /* LUMEN_SERVER_H */
