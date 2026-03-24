/* netdev.c — Network device registry
 *
 * Static table of up to NETDEV_MAX registered network devices.
 * netdev_rx_deliver() is a stub in Phase 24 — silently discards frames.
 * Phase 25 replaces the stub with eth_rx() dispatch.
 */
#include "netdev.h"
#include <stddef.h>

static netdev_t *s_devices[NETDEV_MAX];
static int        s_count = 0;

int
netdev_register(netdev_t *dev)
{
    if (s_count >= NETDEV_MAX || dev == NULL)
        return -1;
    s_devices[s_count++] = dev;
    return 0;
}

netdev_t *
netdev_get(const char *name)
{
    int i;
    for (i = 0; i < s_count; i++) {
        const char *a = s_devices[i]->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b)
            return s_devices[i];
    }
    return NULL;
}

void
netdev_poll_all(void)
{
    int i;
    for (i = 0; i < s_count; i++) {
        if (s_devices[i]->poll)
            s_devices[i]->poll(s_devices[i]);
    }
}

/* Phase 24 stub: silently discard. No printk — SLIRP sends ARPs/mDNS
 * constantly and logging would spam the console at 100 Hz. */
void
netdev_rx_deliver(netdev_t *dev, const void *frame, uint16_t len)
{
    (void)dev;
    (void)frame;
    (void)len;
}
