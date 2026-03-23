/* blkdev.c — Block device registration table */
#include "blkdev.h"
#include <stddef.h>

static blkdev_t *s_devices[BLKDEV_MAX];
static int        s_count = 0;

int blkdev_register(blkdev_t *dev)
{
    if (s_count >= BLKDEV_MAX || dev == NULL)
        return -1;
    s_devices[s_count++] = dev;
    return 0;
}

blkdev_t *blkdev_get(const char *name)
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
