/* copy.c — block device enumeration + rootfs/ESP copy (libinstall) */
#include "libinstall.h"
#include "syscalls.h"
#include <string.h>

/*
 * RAM block devices (ramdisk0, ramdisk1) always use 512-byte sectors.
 * The target NVMe may use 512 B or 4096 B native sectors (block_size).
 *
 * All copies work in 4096-byte quanta (the kernel bounce-buffer size):
 *   source side: 4096/512 = 8 ramdisk sectors per transfer
 *   dest   side: 4096/block_size native sectors per transfer (1 for 4K, 8 for 512B)
 */
#define RAM_BLOCK_SIZE   512ULL
#define XFER_BYTES       4096ULL
#define ESP_ALIGN_BYTES (1ULL * 1024 * 1024)   /* 1 MiB start alignment */
#define ESP_SIZE_BYTES  (32ULL * 1024 * 1024)  /* 32 MiB ESP size        */

static void report_err(install_progress_t *p, const char *msg)
{
    if (p && p->on_error)
        p->on_error(msg, p->ctx);
}

/*
 * Copy src_block_count 512-byte ramdisk sectors from src_dev/src_start
 * to dst_dev/dst_start (in native block_size units), 4096 bytes at a time.
 */
static int copy_aligned(const char *src_dev, uint64_t src_start,
                        const char *dst_dev, uint64_t dst_start,
                        uint64_t src_block_count, uint32_t dst_block_size,
                        install_progress_t *p)
{
    static unsigned char buf[4096];
    uint64_t src_per_xfer = XFER_BYTES / RAM_BLOCK_SIZE;        /* = 8 */
    uint64_t dst_per_xfer = XFER_BYTES / (uint64_t)dst_block_size; /* = 1 for 4K */
    uint64_t total_xfers  = (src_block_count + src_per_xfer - 1) / src_per_xfer;
    int last_pct = -1;
    uint64_t i;

    for (i = 0; i < total_xfers; i++) {
        uint64_t s_lba   = src_start + i * src_per_xfer;
        uint64_t d_lba   = dst_start + i * dst_per_xfer;
        uint64_t s_count = src_per_xfer;
        if (s_lba + s_count > src_start + src_block_count)
            s_count = (src_start + src_block_count) - s_lba;

        memset(buf, 0, sizeof(buf));
        if (li_blkdev_io(src_dev, s_lba, s_count, buf, 0) < 0) {
            report_err(p, "block read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, d_lba, dst_per_xfer, buf, 1) < 0) {
            report_err(p, "block write failed");
            return -1;
        }

        int pct = (int)((i + 1) * 100 / total_xfers);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress)
                p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/* ── Public: install_list_blkdevs ───────────────────────────────────── */

int install_list_blkdevs(install_blkdev_t *out, int max)
{
    long n = li_blkdev_list(out,
                            (unsigned long)(sizeof(install_blkdev_t) * (unsigned)max));
    if (n < 0)
        return 0;
    return (int)n;
}

/* ── Public: install_copy_esp ───────────────────────────────────────── */

int install_copy_esp(const char *devname, uint32_t block_size,
                     install_progress_t *p)
{
    if (p && p->on_step)
        p->on_step("Installing EFI bootloader", p->ctx);

    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    uint64_t esp_blocks = 0; /* ramdisk1 block count (512B each) */
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk1") == 0) {
            esp_blocks = devs[i].block_count;
            break;
        }
    }
    if (esp_blocks == 0) {
        report_err(p, "ramdisk1 (ESP image) not found");
        return -1;
    }

    /* Clamp to ESP partition size in 512B terms */
    uint64_t esp_max_src = ESP_SIZE_BYTES / RAM_BLOCK_SIZE;
    if (esp_blocks > esp_max_src) esp_blocks = esp_max_src;

    /* ESP starts at 1 MiB in native block_size LBAs */
    uint64_t esp_start_lba = ESP_ALIGN_BYTES / (uint64_t)block_size;

    return copy_aligned("ramdisk1", 0, devname, esp_start_lba,
                        esp_blocks, block_size, p);
}

/* ── Public: install_copy_rootfs ────────────────────────────────────── */

int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        uint32_t block_size, install_progress_t *p)
{
    if (p && p->on_step)
        p->on_step("Copying root filesystem", p->ctx);

    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    uint64_t src_blocks = 0; /* ramdisk0 block count (512B each) */
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk0") == 0) {
            src_blocks = devs[i].block_count;
            break;
        }
    }
    if (src_blocks == 0) {
        report_err(p, "ramdisk0 not found");
        return -1;
    }

    /* Convert src_blocks (512B) to dst blocks (native) for size check */
    uint64_t src_bytes = src_blocks * RAM_BLOCK_SIZE;
    uint64_t dst_bytes = dst_blocks * (uint64_t)block_size;
    if (src_bytes > dst_bytes) {
        report_err(p, "rootfs larger than target partition");
        return -1;
    }

    return copy_aligned("ramdisk0", 0, dst_dev, 0,
                        src_blocks, block_size, p);
}
