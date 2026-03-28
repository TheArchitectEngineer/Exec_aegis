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

int blkdev_count(void)
{
    return s_count;
}

blkdev_t *blkdev_get_index(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return s_devices[i];
}

void blkdev_unregister_children(const char *parent_prefix)
{
    int plen = 0;
    while (parent_prefix[plen]) plen++;
    int dst = 0;
    int i;
    for (i = 0; i < s_count; i++) {
        /* Check if name starts with prefix + 'p' */
        int match = 1;
        int j;
        for (j = 0; j < plen; j++) {
            if (s_devices[i]->name[j] != parent_prefix[j]) { match = 0; break; }
        }
        if (match && s_devices[i]->name[plen] == 'p') {
            /* Skip this device (unregister) */
            continue;
        }
        s_devices[dst++] = s_devices[i];
    }
    s_count = dst;
}
