/* netdev.c — Network device registry
 *
 * Static table of up to NETDEV_MAX registered network devices.
 * netdev_rx_deliver() dispatches received frames to the Ethernet layer.
 */
#include "netdev.h"
#include "eth.h"
#include "../core/spinlock.h"
#include <stddef.h>

static netdev_t *s_devices[NETDEV_MAX];
static int        s_count = 0;
static spinlock_t netdev_lock = SPINLOCK_INIT;

int
netdev_register(netdev_t *dev)
{
    irqflags_t fl = spin_lock_irqsave(&netdev_lock);
    if (s_count >= NETDEV_MAX || dev == NULL) {
        spin_unlock_irqrestore(&netdev_lock, fl);
        return -1;
    }
    s_devices[s_count++] = dev;
    spin_unlock_irqrestore(&netdev_lock, fl);
    return 0;
}

netdev_t *
netdev_get(const char *name)
{
    irqflags_t fl = spin_lock_irqsave(&netdev_lock);
    int i;
    for (i = 0; i < s_count; i++) {
        const char *a = s_devices[i]->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) {
            netdev_t *dev = s_devices[i];
            spin_unlock_irqrestore(&netdev_lock, fl);
            return dev;
        }
    }
    spin_unlock_irqrestore(&netdev_lock, fl);
    return NULL;
}

void
netdev_poll_all(void)
{
    irqflags_t fl = spin_lock_irqsave(&netdev_lock);
    int i;
    for (i = 0; i < s_count; i++) {
        if (s_devices[i]->poll)
            s_devices[i]->poll(s_devices[i]);
    }
    spin_unlock_irqrestore(&netdev_lock, fl);
}

void
netdev_rx_deliver(netdev_t *dev, const void *frame, uint16_t len)
{
    eth_rx(dev, frame, len);
}
