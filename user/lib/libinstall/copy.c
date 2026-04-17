/* copy.c — block device enumeration + rootfs/ESP copy (libinstall) */
#include "libinstall.h"
#include "syscalls.h"
#include <string.h>

/*
 * All logical block devices (ramdisk0, ramdisk1, partition devices via the
 * kernel's 512e GPT wrapper) present 512-byte logical sectors.  Only the
 * raw NVMe device (e.g. "nvme0") has native 4K sectors on Advanced Format
 * drives.
 *
 * Therefore:
 *   - rootfs copy  (ramdisk0 → nvme0p1): both 512B, simple loop
 *   - ESP copy     (ramdisk1 → nvme0):   ramdisk1 is 512B, nvme0 may be
 *                                         4K native — use block_size for
 *                                         target offset + chunk calculation
 */

#define RAM_BLOCK_SIZE   512ULL
#define XFER_BYTES       4096ULL
#define ESP_ALIGN_BYTES (1ULL * 1024 * 1024)   /* 1 MiB start alignment  */
#define ESP_SIZE_BYTES  (32ULL * 1024 * 1024)  /* 32 MiB ESP size        */

static void report_err(install_progress_t *p, const char *msg)
{
    if (p && p->on_error)
        p->on_error(msg, p->ctx);
}

/*
 * Simple 512B-unit copy: both src and dst present 512-byte logical sectors.
 * count is in 512B units.
 */
static int copy_512b(const char *src_dev, uint64_t src_lba,
                     const char *dst_dev, uint64_t dst_lba,
                     uint64_t count, install_progress_t *p)
{
    static unsigned char buf[4096];
    uint64_t max_chunk = XFER_BYTES / RAM_BLOCK_SIZE; /* = 8 */
    int last_pct = -1;
    uint64_t done = 0;

    while (done < count) {
        uint64_t chunk = count - done;
        if (chunk > max_chunk) chunk = max_chunk;

        if (li_blkdev_io(src_dev, src_lba + done, chunk, buf, 0) < 0) {
            report_err(p, "block read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, dst_lba + done, chunk, buf, 1) < 0) {
            report_err(p, "block write failed");
            return -1;
        }
        done += chunk;

        int pct = (int)(done * 100 / count);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress) p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/*
 * Copy src_block_count 512-byte ramdisk sectors to a raw device (dst_dev)
 * that may have native block_size sectors.  dst_start_lba is in native
 * block_size units.  Reads 8 × 512B (= 4096B) at a time; writes
 * 4096/block_size native sectors at a time.
 */
static int copy_to_native(const char *src_dev, uint64_t src_block_count,
                           const char *dst_dev, uint64_t dst_start_lba,
                           uint32_t dst_block_size, install_progress_t *p)
{
    static unsigned char buf[4096];
    uint64_t src_per_xfer = XFER_BYTES / RAM_BLOCK_SIZE;          /* = 8 */
    uint64_t dst_per_xfer = XFER_BYTES / (uint64_t)dst_block_size; /* = 1 for 4K */
    uint64_t total_xfers  = (src_block_count + src_per_xfer - 1) / src_per_xfer;
    int last_pct = -1;
    uint64_t i;

    for (i = 0; i < total_xfers; i++) {
        uint64_t s_lba   = i * src_per_xfer;
        uint64_t d_lba   = dst_start_lba + i * dst_per_xfer;
        uint64_t s_count = src_per_xfer;
        if (s_lba + s_count > src_block_count)
            s_count = src_block_count - s_lba;

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
            if (p && p->on_progress) p->on_progress(pct, p->ctx);
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

    /* ESP start in native block_size LBAs on the raw disk */
    uint64_t esp_start_lba = ESP_ALIGN_BYTES / (uint64_t)block_size;

    /* devname is the raw disk (e.g. "nvme0") which may have 4K native blocks */
    return copy_to_native("ramdisk1", esp_blocks,
                          devname, esp_start_lba, block_size, p);
}

/* ── Public: install_copy_rootfs ────────────────────────────────────── */

int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        uint32_t block_size, install_progress_t *p)
{
    (void)block_size; /* partition device presents 512B after 512e */
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
    if (src_blocks > dst_blocks) {
        report_err(p, "rootfs larger than target partition");
        return -1;
    }

    /* dst_dev (nvme0p1) presents 512B logical sectors via 512e emulation */
    return copy_512b("ramdisk0", 0, dst_dev, 0, src_blocks, p);
}
