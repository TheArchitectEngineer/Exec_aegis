#include "sys_impl.h"
#include "cap.h"
#include "uaccess.h"
#include "printk.h"
#include "blkdev.h"
#include "gpt.h"
#include <stdint.h>

/* ── blkdev_info_t — sent to userspace by sys_blkdev_list ───────────── */
typedef struct {
    char     name[16];
    uint64_t block_count;
    uint32_t block_size;
    uint32_t _pad;
} blkdev_info_t;

/*
 * sys_blkdev_list — syscall 510
 * Enumerate registered block devices.
 * arg1 = user buffer pointer
 * arg2 = buffer size in bytes
 * Returns number of devices, or negative errno.
 */
uint64_t
sys_blkdev_list(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_DISK_ADMIN, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    int count = blkdev_count();
    uint64_t max_entries = arg2 / sizeof(blkdev_info_t);
    int i;
    for (i = 0; i < count && (uint64_t)i < max_entries; i++) {
        blkdev_t *d = blkdev_get_index(i);
        if (!d) break;
        blkdev_info_t info;
        __builtin_memset(&info, 0, sizeof(info));
        /* Copy name */
        int j;
        for (j = 0; j < 15 && d->name[j]; j++)
            info.name[j] = d->name[j];
        info.name[j] = '\0';
        info.block_count = d->block_count;
        info.block_size  = d->block_size;
        if (!user_ptr_valid(arg1 + (uint64_t)i * sizeof(blkdev_info_t), sizeof(blkdev_info_t)))
            return (uint64_t)-14;  /* EFAULT */
        copy_to_user((void *)(uintptr_t)(arg1 + (uint64_t)i * sizeof(blkdev_info_t)),
                     &info, sizeof(blkdev_info_t));
    }
    return (uint64_t)i;
}

/*
 * sys_blkdev_io — syscall 511
 * Raw block device read/write.
 * arg1 = user pointer to device name (NUL-terminated)
 * arg2 = LBA start
 * arg3 = block count
 * arg4 = user buffer
 * arg5 = 0=read, 1=write
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_blkdev_io(uint64_t arg1, uint64_t arg2, uint64_t arg3,
              uint64_t arg4, uint64_t arg5)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t rights = (arg5 != 0) ? CAP_RIGHTS_WRITE : CAP_RIGHTS_READ;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_DISK_ADMIN, rights) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* Copy device name from user */
    char name[16];
    {
        uint64_t i;
        for (i = 0; i < 15; i++) {
            if (!user_ptr_valid(arg1 + i, 1))
                return (uint64_t)-14;
            char c;
            copy_from_user(&c, (const void *)(uintptr_t)(arg1 + i), 1);
            name[i] = c;
            if (c == '\0') break;
        }
        name[15] = '\0';
    }

    blkdev_t *dev = blkdev_get(name);
    if (!dev) return (uint64_t)-19;  /* ENODEV */

    uint64_t lba   = arg2;
    uint64_t count = arg3;
    if (count == 0) return 0;
    if (lba + count > dev->block_count)
        return (uint64_t)-22;  /* EINVAL */

    /* Transfer in 8-sector (4KB) chunks using a kernel bounce buffer.
     * NVMe driver limits each transfer to count*512 <= 4096. */
    static uint8_t s_bounce[4096];
    uint64_t done = 0;
    while (done < count) {
        uint32_t chunk = (uint32_t)(count - done);
        if (chunk > 8) chunk = 8;
        uint64_t user_off = done * 512;

        if (arg5 == 0) {
            /* Read: dev → bounce → user */
            if (dev->read(dev, lba + done, chunk, s_bounce) < 0)
                return (uint64_t)-5;  /* EIO */
            if (!user_ptr_valid(arg4 + user_off, (uint64_t)chunk * 512))
                return (uint64_t)-14;
            copy_to_user((void *)(uintptr_t)(arg4 + user_off),
                         s_bounce, (uint64_t)chunk * 512);
        } else {
            /* Write: user → bounce → dev */
            if (!user_ptr_valid(arg4 + user_off, (uint64_t)chunk * 512))
                return (uint64_t)-14;
            copy_from_user(s_bounce,
                           (const void *)(uintptr_t)(arg4 + user_off),
                           (uint64_t)chunk * 512);
            if (dev->write(dev, lba + done, chunk, s_bounce) < 0)
                return (uint64_t)-5;  /* EIO */
        }
        done += chunk;
    }
    return 0;
}

/*
 * sys_gpt_rescan — syscall 512
 * Re-scan GPT on a named block device.
 * arg1 = user pointer to device name
 * Returns number of partitions, or negative errno.
 */
uint64_t
sys_gpt_rescan(uint64_t arg1)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_DISK_ADMIN, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char name[16];
    {
        uint64_t i;
        for (i = 0; i < 15; i++) {
            if (!user_ptr_valid(arg1 + i, 1))
                return (uint64_t)-14;
            char c;
            copy_from_user(&c, (const void *)(uintptr_t)(arg1 + i), 1);
            name[i] = c;
            if (c == '\0') break;
        }
        name[15] = '\0';
    }

    int n = gpt_rescan(name);
    return (uint64_t)(int64_t)n;
}
