/* ext2.c — ext2 filesystem driver (read path + block cache)
 *
 * Mount, 16-slot LRU block cache, inode read, path walk, file read.
 * Write path stubs are in place for Task 3.
 *
 * No libc, no malloc, no VLAs.
 */

#include "ext2.h"
#include "blkdev.h"
#include "../core/printk.h"

/* ------------------------------------------------------------------ */
/* Static globals                                                       */
/* ------------------------------------------------------------------ */

static blkdev_t *s_dev;
static ext2_superblock_t s_sb;
static uint32_t s_block_size;
static uint32_t s_num_groups;
static ext2_bgd_t s_bgd[32];   /* support up to 32 block groups */
static int s_mounted = 0;

/* ------------------------------------------------------------------ */
/* 16-slot LRU block cache                                             */
/* ------------------------------------------------------------------ */

#define CACHE_SLOTS 16

typedef struct {
    uint32_t block_num;
    uint8_t  dirty;
    uint32_t age;
    uint8_t  data[4096];    /* max block size */
} cache_slot_t;

static cache_slot_t s_cache[CACHE_SLOTS];
static uint32_t s_cache_age = 0;

/* cache_find — return slot index of block_num, or -1 if not cached */
static int cache_find(uint32_t block_num)
{
    int i;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].block_num == block_num && s_cache[i].age != 0)
            return i;
    }
    return -1;
}

/* cache_evict — find the LRU slot (lowest age, prefer clean over dirty).
 * Writes back dirty data before evicting. Returns slot index. */
static int cache_evict(void)
{
    int i;
    int best = 0;

    /* Prefer a clean slot with the lowest age */
    for (i = 1; i < CACHE_SLOTS; i++) {
        /* Prefer clean over dirty */
        if (s_cache[i].dirty == 0 && s_cache[best].dirty != 0) {
            best = i;
            continue;
        }
        if (s_cache[i].dirty != 0 && s_cache[best].dirty == 0) {
            continue;
        }
        /* Same cleanliness: pick lower age */
        if (s_cache[i].age < s_cache[best].age) {
            best = i;
        }
    }

    /* Write back if dirty */
    if (s_cache[best].dirty && s_cache[best].block_num != 0) {
        uint64_t lba = (uint64_t)s_cache[best].block_num *
                       (s_block_size / 512);
        s_dev->write(s_dev, lba, s_block_size / 512, s_cache[best].data);
        s_cache[best].dirty = 0;
    }

    return best;
}

/* cache_read_block — read block_num into data_out (caller-supplied buf) */
static int cache_read_block(uint32_t block_num, uint8_t *data_out)
{
    int idx = cache_find(block_num);
    uint32_t i;

    if (idx < 0) {
        idx = cache_evict();
        s_cache[idx].block_num = block_num;
        s_cache[idx].dirty = 0;
        s_cache_age++;
        s_cache[idx].age = s_cache_age;

        uint64_t lba = (uint64_t)block_num * (s_block_size / 512);
        int ret = s_dev->read(s_dev, lba, s_block_size / 512,
                              s_cache[idx].data);
        if (ret < 0)
            return -1;
    } else {
        s_cache_age++;
        s_cache[idx].age = s_cache_age;
    }

    for (i = 0; i < s_block_size; i++)
        data_out[i] = s_cache[idx].data[i];

    return 0;
}

/* cache_mark_dirty — mark the cached slot for block_num dirty */
static void cache_mark_dirty(uint32_t block_num)
{
    int idx = cache_find(block_num);
    if (idx >= 0)
        s_cache[idx].dirty = 1;
}

/* cache_get_slot — return pointer to cached data for block_num,
 * loading from disk if necessary. Returns NULL on I/O error. */
static uint8_t *cache_get_slot(uint32_t block_num)
{
    int idx = cache_find(block_num);

    if (idx < 0) {
        idx = cache_evict();
        s_cache[idx].block_num = block_num;
        s_cache[idx].dirty = 0;
        s_cache_age++;
        s_cache[idx].age = s_cache_age;

        uint64_t lba = (uint64_t)block_num * (s_block_size / 512);
        int ret = s_dev->read(s_dev, lba, s_block_size / 512,
                              s_cache[idx].data);
        if (ret < 0)
            return (uint8_t *)0;
    } else {
        s_cache_age++;
        s_cache[idx].age = s_cache_age;
    }

    return s_cache[idx].data;
}

/* ------------------------------------------------------------------ */
/* ext2_mount                                                          */
/* ------------------------------------------------------------------ */

int ext2_mount(const char *devname)
{
    uint32_t i;

    /* Initialise cache slots to age 0 (unused) */
    for (i = 0; i < CACHE_SLOTS; i++) {
        s_cache[i].block_num = 0;
        s_cache[i].dirty = 0;
        s_cache[i].age = 0;
    }

    s_dev = blkdev_get(devname);
    if (!s_dev)
        return -1;  /* silent — no NVMe on -machine pc */

    /* Superblock is at byte offset 1024 from partition start.
     * blkdev uses 512-byte sectors: LBA 2 = byte 1024. */
    uint8_t sb_buf[1024];
    if (s_dev->read(s_dev, 2, 2, sb_buf) < 0)
        return -1;

    /* Copy superblock from start of buffer */
    uint8_t *src = sb_buf;
    uint8_t *dst = (uint8_t *)&s_sb;
    for (i = 0; i < sizeof(ext2_superblock_t); i++)
        dst[i] = src[i];

    if (s_sb.s_magic != EXT2_MAGIC)
        return -1;

    s_block_size = 1024u << s_sb.s_log_block_size;
    if (s_sb.s_rev_level >= 1 && (s_sb.s_inode_size < 128 || s_sb.s_inode_size > 4096)) {
        return -1;
    }
    s_num_groups = (s_sb.s_blocks_count + s_sb.s_blocks_per_group - 1)
                   / s_sb.s_blocks_per_group;
    if (s_num_groups > 32)
        s_num_groups = 32;

    /* BGD table is at the block immediately after the superblock.
     * For 1024-byte blocks: superblock is in block 1, BGD at block 2.
     * For larger blocks:    superblock is in block 0, BGD at block 1. */
    uint32_t bgd_block = (s_sb.s_first_data_block == 1) ? 2 : 1;

    uint8_t bgd_buf[4096];
    if (cache_read_block(bgd_block, bgd_buf) < 0)
        return -1;

    uint32_t bgd_bytes = s_num_groups * sizeof(ext2_bgd_t);
    src = bgd_buf;
    dst = (uint8_t *)s_bgd;
    for (i = 0; i < bgd_bytes; i++)
        dst[i] = src[i];

    s_mounted = 1;
    printk("[EXT2] OK: mounted %s, %u blocks, %u inodes\n",
           devname, s_sb.s_blocks_count, s_sb.s_inodes_count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_read_inode (internal)                                          */
/* ------------------------------------------------------------------ */

static int ext2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    uint32_t group       = (ino - 1) / s_sb.s_inodes_per_group;
    uint32_t index       = (ino - 1) % s_sb.s_inodes_per_group;
    if (group >= s_num_groups || group >= 32) {
        return -1;
    }
    uint32_t inode_size  = (s_sb.s_rev_level >= 1)
                           ? (uint32_t)s_sb.s_inode_size : 128u;
    uint32_t inode_table_block = s_bgd[group].bg_inode_table;
    uint32_t byte_offset = index * inode_size;
    uint32_t block_offset = byte_offset / s_block_size;
    uint32_t in_block    = byte_offset % s_block_size;

    uint8_t *data = cache_get_slot(inode_table_block + block_offset);
    if (!data)
        return -1;

    uint8_t *src = data + in_block;
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;
    for (i = 0; i < sizeof(ext2_inode_t); i++)
        dst[i] = src[i];

    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_write_inode (internal)                                         */
/* ------------------------------------------------------------------ */

/* __attribute__((unused)): write_inode is called only by the Task 3 write
 * path (ext2_write, ext2_create, ext2_unlink).  It is compiled here so the
 * implementation stays next to read_inode, but is not yet reachable from
 * any external caller.  Remove the attribute when Task 3 is wired in. */
static int __attribute__((unused))
ext2_write_inode(uint32_t ino, const ext2_inode_t *inode)
{
    uint32_t group       = (ino - 1) / s_sb.s_inodes_per_group;
    uint32_t index       = (ino - 1) % s_sb.s_inodes_per_group;
    if (group >= s_num_groups || group >= 32) {
        return -1;
    }
    uint32_t inode_size  = (s_sb.s_rev_level >= 1)
                           ? (uint32_t)s_sb.s_inode_size : 128u;
    uint32_t inode_table_block = s_bgd[group].bg_inode_table;
    uint32_t byte_offset = index * inode_size;
    uint32_t block_offset = byte_offset / s_block_size;
    uint32_t in_block    = byte_offset % s_block_size;

    uint8_t *data = cache_get_slot(inode_table_block + block_offset);
    if (!data)
        return -1;

    uint8_t *dst = data + in_block;
    const uint8_t *src = (const uint8_t *)inode;
    uint32_t i;
    for (i = 0; i < sizeof(ext2_inode_t); i++)
        dst[i] = src[i];

    cache_mark_dirty(inode_table_block + block_offset);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_block_num (internal)                                           */
/* ------------------------------------------------------------------ */

static uint32_t ext2_block_num(const ext2_inode_t *inode,
                               uint32_t file_block)
{
    uint32_t ptrs_per_block = s_block_size / 4;

    if (file_block < 12)
        return inode->i_block[file_block];

    if (file_block < 12 + ptrs_per_block) {
        uint32_t indirect = inode->i_block[12];
        if (indirect == 0)
            return 0;
        uint8_t *data = cache_get_slot(indirect);
        if (!data)
            return 0;
        uint32_t off = file_block - 12;
        uint32_t entry;
        uint8_t *p = data + off * 4;
        entry  = (uint32_t)p[0];
        entry |= (uint32_t)p[1] << 8;
        entry |= (uint32_t)p[2] << 16;
        entry |= (uint32_t)p[3] << 24;
        return entry;
    }

    return 0;   /* double/triple indirect not supported */
}

/* ------------------------------------------------------------------ */
/* ext2_open — walk path from root inode 2                             */
/* ------------------------------------------------------------------ */

int ext2_open(const char *path, uint32_t *inode_out)
{
    if (!s_mounted)
        return -1;

    uint32_t current_ino = EXT2_ROOT_INODE;

    /* skip leading slashes */
    while (*path == '/')
        path++;

    /* If path is empty (root dir itself) */
    if (*path == '\0') {
        *inode_out = current_ino;
        return 0;
    }

    while (*path != '\0') {
        /* Extract next component into a local buffer */
        char component[256];
        uint32_t clen = 0;
        while (*path != '\0' && *path != '/') {
            if (clen < 255)
                component[clen++] = *path;
            path++;
        }
        component[clen] = '\0';

        /* Skip trailing slashes between components */
        while (*path == '/')
            path++;

        /* Read current directory inode */
        ext2_inode_t inode;
        if (ext2_read_inode(current_ino, &inode) < 0)
            return -1;

        /* Must be a directory */
        if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
            return -1;

        /* Search directory entries for component */
        int found = 0;
        uint32_t pos = 0;

        while (pos < inode.i_size) {
            uint32_t file_block = pos / s_block_size;
            uint32_t blk = ext2_block_num(&inode, file_block);
            if (blk == 0)
                break;
            uint8_t *data = cache_get_slot(blk);
            if (!data)
                return -1;
            uint32_t block_pos = pos % s_block_size;
            while (block_pos < s_block_size) {
                ext2_dirent_t *de =
                    (ext2_dirent_t *)(data + block_pos);
                if (de->rec_len < 8 || block_pos + de->rec_len > s_block_size)
                    break;
                if (de->inode == 0) {
                    block_pos += de->rec_len;
                    pos += de->rec_len;
                    continue;
                }
                if (de->name_len == (uint8_t)clen) {
                    /* manual name compare */
                    uint32_t k;
                    int match = 1;
                    for (k = 0; k < clen; k++) {
                        if (de->name[k] != component[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        current_ino = de->inode;
                        found = 1;
                        break;
                    }
                }
                block_pos += de->rec_len;
                pos += de->rec_len;
            }
            if (found)
                break;
            /* Advance to next block if we didn't break out */
            if (!found) {
                uint32_t block_end = (file_block + 1) * s_block_size;
                if (pos < block_end)
                    pos = block_end;
            }
        }

        if (!found)
            return -1;
    }

    *inode_out = current_ino;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ext2_read                                                           */
/* ------------------------------------------------------------------ */

int ext2_read(uint32_t inode_num, void *buf, uint32_t offset, uint32_t len)
{
    if (!s_mounted)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) < 0)
        return -1;

    if (offset >= inode.i_size)
        return 0;

    if (offset + len > inode.i_size)
        len = inode.i_size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    while (bytes_read < len) {
        uint32_t cur_off = offset + bytes_read;
        uint32_t file_block = cur_off / s_block_size;
        uint32_t in_block   = cur_off % s_block_size;
        uint32_t can_copy   = s_block_size - in_block;
        if (can_copy > len - bytes_read)
            can_copy = len - bytes_read;

        uint32_t blk = ext2_block_num(&inode, file_block);
        if (blk == 0) {
            /* sparse block — fill zeros */
            uint32_t i;
            for (i = 0; i < can_copy; i++)
                out[bytes_read + i] = 0;
        } else {
            uint8_t *data = cache_get_slot(blk);
            if (!data)
                return (int)bytes_read;
            uint32_t i;
            for (i = 0; i < can_copy; i++)
                out[bytes_read + i] = data[in_block + i];
        }
        bytes_read += can_copy;
    }

    return (int)bytes_read;
}

/* ------------------------------------------------------------------ */
/* ext2_file_size                                                       */
/* ------------------------------------------------------------------ */

int ext2_file_size(uint32_t inode_num)
{
    if (!s_mounted)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) < 0)
        return -1;

    return (int)inode.i_size;
}

/* ------------------------------------------------------------------ */
/* ext2_readdir — stub (Task 3)                                        */
/* ------------------------------------------------------------------ */

int ext2_readdir(uint32_t dir_inode, void *buf, uint32_t buf_size)
{
    (void)dir_inode;
    (void)buf;
    (void)buf_size;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Write path stubs — implemented in Task 3                            */
/* ------------------------------------------------------------------ */

int ext2_write(uint32_t inode_num, const void *buf,
               uint32_t offset, uint32_t len)
{
    (void)inode_num;
    (void)buf;
    (void)offset;
    (void)len;
    return -1;
}

int ext2_create(const char *path, uint16_t mode)
{
    (void)path;
    (void)mode;
    return -1;
}

int ext2_unlink(const char *path)
{
    (void)path;
    return -1;
}

int ext2_mkdir(const char *path, uint16_t mode)
{
    (void)path;
    (void)mode;
    return -1;
}

int ext2_rename(const char *old_path, const char *new_path)
{
    (void)old_path;
    (void)new_path;
    return -1;
}

/* ------------------------------------------------------------------ */
/* ext2_sync — flush all dirty cache slots to disk                     */
/* ------------------------------------------------------------------ */

void ext2_sync(void)
{
    int i;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].dirty && s_cache[i].block_num != 0) {
            uint64_t lba = (uint64_t)s_cache[i].block_num *
                           (s_block_size / 512);
            s_dev->write(s_dev, lba, s_block_size / 512,
                         s_cache[i].data);
            s_cache[i].dirty = 0;
        }
    }
}
