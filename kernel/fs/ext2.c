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

static int ext2_write_inode(uint32_t ino, const ext2_inode_t *inode)
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
/* Block and inode allocation                                          */
/* ------------------------------------------------------------------ */

/* ext2_alloc_block — scan block bitmaps for a free bit.
 * preferred_group: start scanning from this group (locality hint).
 * Returns allocated block number, or 0 on failure. */
static uint32_t ext2_alloc_block(uint32_t preferred_group)
{
    uint32_t g, i;
    for (g = 0; g < s_num_groups; g++) {
        uint32_t grp = (g + preferred_group) % s_num_groups;
        if (s_bgd[grp].bg_free_blocks_count == 0)
            continue;
        uint8_t *bitmap = cache_get_slot(s_bgd[grp].bg_block_bitmap);
        if (!bitmap)
            continue;
        uint32_t blocks_in_group = (grp == s_num_groups - 1)
            ? (s_sb.s_blocks_count - grp * s_sb.s_blocks_per_group)
            : s_sb.s_blocks_per_group;
        for (i = 0; i < blocks_in_group; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                cache_mark_dirty(s_bgd[grp].bg_block_bitmap);
                s_bgd[grp].bg_free_blocks_count--;
                s_sb.s_free_blocks_count--;
                return grp * s_sb.s_blocks_per_group + i + s_sb.s_first_data_block;
            }
        }
    }
    return 0; /* no free block */
}

/* ext2_alloc_inode — scan inode bitmaps for a free bit.
 * preferred_group: start scanning from this group.
 * Returns allocated inode number (1-based), or 0 on failure. */
static uint32_t ext2_alloc_inode(uint32_t preferred_group)
{
    uint32_t g, i;
    for (g = 0; g < s_num_groups; g++) {
        uint32_t grp = (g + preferred_group) % s_num_groups;
        if (s_bgd[grp].bg_free_inodes_count == 0)
            continue;
        uint8_t *bitmap = cache_get_slot(s_bgd[grp].bg_inode_bitmap);
        if (!bitmap)
            continue;
        for (i = 0; i < s_sb.s_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1u << (i % 8)))) {
                bitmap[i / 8] |= (1u << (i % 8));
                cache_mark_dirty(s_bgd[grp].bg_inode_bitmap);
                s_bgd[grp].bg_free_inodes_count--;
                s_sb.s_free_inodes_count--;
                return grp * s_sb.s_inodes_per_group + i + 1; /* 1-based */
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

/* ext2_lookup_parent — split "/path/to/file" into parent inode and basename.
 * On success, *parent_ino_out and *basename_out are set; returns 0.
 * Returns -1 if the parent directory cannot be opened. */
static int ext2_lookup_parent(const char *path, uint32_t *parent_ino_out,
                               const char **basename_out)
{
    const char *last_slash = path;
    const char *p = path;
    while (*p) {
        if (*p == '/')
            last_slash = p;
        p++;
    }
    if (last_slash == path) {
        /* file sits directly in root */
        *parent_ino_out = EXT2_ROOT_INODE;
        *basename_out = path + 1; /* skip leading '/' */
    } else {
        char parent_path[256];
        uint32_t plen = (uint32_t)(last_slash - path);
        if (plen == 0)
            plen = 1; /* "/" */
        if (plen >= sizeof(parent_path))
            return -1;
        uint32_t ci;
        for (ci = 0; ci < plen; ci++)
            parent_path[ci] = path[ci];
        parent_path[plen] = '\0';
        if (ext2_open(parent_path, parent_ino_out) != 0)
            return -1;
        *basename_out = last_slash + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Directory entry helpers                                             */
/* ------------------------------------------------------------------ */

/* ext2_dir_add_entry — append a directory entry to dir_ino.
 * Reuses a deleted slot or splits an oversized entry before allocating
 * a new block.  Returns 0 on success, -1 on failure. */
static int ext2_dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                               const char *name, uint8_t file_type)
{
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0)
        return -1;

    uint8_t name_len = 0;
    while (name[name_len])
        name_len++;
    uint16_t needed = (uint16_t)(8u + name_len);
    /* round up to 4-byte boundary */
    needed = (uint16_t)((needed + 3u) & ~3u);

    /* scan existing directory blocks looking for a slot */
    uint32_t pos = 0;
    uint32_t file_block_idx = 0;
    while (pos <= dir.i_size) {
        uint32_t blk;
        if (pos == dir.i_size) {
            /* need a new block */
            blk = ext2_alloc_block(0);
            if (blk == 0)
                return -1;
            if (file_block_idx < 12) {
                dir.i_block[file_block_idx] = blk;
            } else {
                return -1; /* indirect not supported for dirs in this impl */
            }
            dir.i_size += s_block_size;
            dir.i_blocks += s_block_size / 512;
            /* zero the new block and write a single spanning entry */
            uint8_t *newdata = cache_get_slot(blk);
            if (!newdata)
                return -1;
            uint32_t zi;
            for (zi = 0; zi < s_block_size; zi++)
                newdata[zi] = 0;
            ext2_dirent_t *de = (ext2_dirent_t *)newdata;
            de->inode = child_ino;
            de->rec_len = (uint16_t)s_block_size;
            de->name_len = name_len;
            de->file_type = file_type;
            uint32_t ni;
            for (ni = 0; ni < name_len; ni++)
                de->name[ni] = name[ni];
            cache_mark_dirty(blk);
            ext2_write_inode(dir_ino, &dir);
            return 0;
        }
        blk = ext2_block_num(&dir, file_block_idx);
        if (blk == 0) {
            pos += s_block_size;
            file_block_idx++;
            continue;
        }
        uint8_t *data = cache_get_slot(blk);
        if (!data)
            return -1;
        uint32_t block_pos = 0;
        while (block_pos < s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(data + block_pos);
            if (de->rec_len < 8 || block_pos + de->rec_len > s_block_size)
                break;
            uint16_t actual = (uint16_t)((8u + de->name_len + 3u) & ~3u);
            uint16_t free_space = (uint16_t)(de->rec_len - actual);
            if (de->inode == 0) {
                /* unused entry — reuse whole slot */
                if (de->rec_len >= needed) {
                    de->inode = child_ino;
                    de->name_len = name_len;
                    de->file_type = file_type;
                    uint32_t ni;
                    for (ni = 0; ni < name_len; ni++)
                        de->name[ni] = name[ni];
                    cache_mark_dirty(blk);
                    ext2_write_inode(dir_ino, &dir);
                    return 0;
                }
            } else if (free_space >= needed) {
                /* split existing entry */
                uint8_t *next = data + block_pos + actual;
                ext2_dirent_t *nde = (ext2_dirent_t *)next;
                nde->inode = child_ino;
                nde->rec_len = free_space;
                nde->name_len = name_len;
                nde->file_type = file_type;
                uint32_t ni;
                for (ni = 0; ni < name_len; ni++)
                    nde->name[ni] = name[ni];
                de->rec_len = actual;
                cache_mark_dirty(blk);
                ext2_write_inode(dir_ino, &dir);
                return 0;
            }
            block_pos += de->rec_len;
        }
        pos += s_block_size;
        file_block_idx++;
    }
    return -1;
}

/* ext2_dir_remove_entry — zero-out the inode field of the named entry in
 * dir_ino (or merge its rec_len into the previous entry).
 * Returns 0 on success, -1 if the name was not found. */
static int ext2_dir_remove_entry(uint32_t dir_ino, const char *name)
{
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0)
        return -1;

    uint8_t name_len = 0;
    while (name[name_len])
        name_len++;

    uint32_t pos = 0;
    uint32_t file_block_idx = 0;
    while (pos < dir.i_size) {
        uint32_t blk = ext2_block_num(&dir, file_block_idx);
        if (blk == 0) {
            pos += s_block_size;
            file_block_idx++;
            continue;
        }
        uint8_t *data = cache_get_slot(blk);
        if (!data)
            return -1;
        uint32_t block_pos = 0;
        ext2_dirent_t *prev = (void *)0;
        while (block_pos < s_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(data + block_pos);
            if (de->rec_len < 8 || block_pos + de->rec_len > s_block_size)
                break;
            if (de->inode != 0 && de->name_len == name_len) {
                uint32_t ni;
                int match = 1;
                for (ni = 0; ni < name_len; ni++) {
                    if (de->name[ni] != name[ni]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    if (prev) {
                        prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                    } else {
                        de->inode = 0;
                    }
                    cache_mark_dirty(blk);
                    return 0;
                }
            }
            prev = de;
            block_pos += de->rec_len;
        }
        pos += s_block_size;
        file_block_idx++;
    }
    return -1; /* not found */
}

/* ------------------------------------------------------------------ */
/* Write path                                                          */
/* ------------------------------------------------------------------ */

int ext2_write(uint32_t inode_num, const void *buf,
               uint32_t offset, uint32_t len)
{
    if (!s_mounted || len == 0)
        return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(inode_num, &inode) != 0)
        return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bytes_written = 0;

    while (bytes_written < len) {
        uint32_t cur_offset = offset + bytes_written;
        uint32_t file_block = cur_offset / s_block_size;
        uint32_t in_block   = cur_offset % s_block_size;
        uint32_t can_write  = s_block_size - in_block;
        if (can_write > len - bytes_written)
            can_write = len - bytes_written;

        uint32_t blk = ext2_block_num(&inode, file_block);
        if (blk == 0) {
            /* allocate a new direct block (indirect not yet supported) */
            if (file_block < 12) {
                blk = ext2_alloc_block(0);
                if (blk == 0)
                    break;
                inode.i_block[file_block] = blk;
                /* zero the new block */
                uint8_t *newdata = cache_get_slot(blk);
                if (!newdata)
                    break;
                uint32_t zi;
                for (zi = 0; zi < s_block_size; zi++)
                    newdata[zi] = 0;
                cache_mark_dirty(blk);
            } else {
                break; /* indirect not yet supported for write */
            }
        }

        uint8_t *data = cache_get_slot(blk);
        if (!data)
            break;
        uint32_t wi;
        for (wi = 0; wi < can_write; wi++)
            data[in_block + wi] = src[bytes_written + wi];
        cache_mark_dirty(blk);
        bytes_written += can_write;
    }

    /* update inode size and 512-byte sector count */
    uint32_t end = offset + bytes_written;
    if (end > inode.i_size) {
        inode.i_size = end;
        inode.i_blocks = (inode.i_size + 511u) / 512u;
    }
    ext2_write_inode(inode_num, &inode);
    return (int)bytes_written;
}

int ext2_create(const char *path, uint16_t mode)
{
    if (!s_mounted)
        return -1;

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(path, &parent_ino, &basename) != 0)
        return -1;

    uint32_t new_ino = ext2_alloc_inode(0);
    if (new_ino == 0)
        return -1;

    ext2_inode_t inode;
    uint32_t ci;
    for (ci = 0; ci < sizeof(inode); ci++)
        ((uint8_t *)&inode)[ci] = 0;
    inode.i_mode = EXT2_S_IFREG | (mode & 0x1FFu);
    inode.i_links_count = 1;
    ext2_write_inode(new_ino, &inode);
    return ext2_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_REG_FILE);
}

int ext2_unlink(const char *path)
{
    if (!s_mounted)
        return -1;

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(path, &parent_ino, &basename) != 0)
        return -1;

    uint32_t ino;
    if (ext2_open(path, &ino) != 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if (inode.i_links_count > 0)
        inode.i_links_count--;

    if (inode.i_links_count == 0) {
        /* free direct data blocks */
        uint32_t bi;
        for (bi = 0; bi < 12; bi++) {
            if (inode.i_block[bi] != 0) {
                uint32_t blk = inode.i_block[bi];
                uint32_t grp = 0;
                if (s_sb.s_blocks_per_group > 0)
                    grp = (blk - s_sb.s_first_data_block) / s_sb.s_blocks_per_group;
                if (grp < s_num_groups) {
                    uint8_t *bitmap = cache_get_slot(s_bgd[grp].bg_block_bitmap);
                    if (bitmap) {
                        uint32_t bit = (blk - s_sb.s_first_data_block)
                                       % s_sb.s_blocks_per_group;
                        bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
                        cache_mark_dirty(s_bgd[grp].bg_block_bitmap);
                        s_bgd[grp].bg_free_blocks_count++;
                        s_sb.s_free_blocks_count++;
                    }
                }
                inode.i_block[bi] = 0;
            }
        }
        /* free inode */
        uint32_t igrp = 0;
        if (s_sb.s_inodes_per_group > 0)
            igrp = (ino - 1) / s_sb.s_inodes_per_group;
        if (igrp < s_num_groups) {
            uint8_t *ibitmap = cache_get_slot(s_bgd[igrp].bg_inode_bitmap);
            if (ibitmap) {
                uint32_t ibit = (ino - 1) % s_sb.s_inodes_per_group;
                ibitmap[ibit / 8] &= (uint8_t)~(1u << (ibit % 8));
                cache_mark_dirty(s_bgd[igrp].bg_inode_bitmap);
                s_bgd[igrp].bg_free_inodes_count++;
                s_sb.s_free_inodes_count++;
            }
        }
        inode.i_dtime = 1; /* mark deleted */
    }
    ext2_write_inode(ino, &inode);
    return ext2_dir_remove_entry(parent_ino, basename);
}

int ext2_mkdir(const char *path, uint16_t mode)
{
    if (!s_mounted)
        return -1;

    uint32_t parent_ino;
    const char *basename;
    if (ext2_lookup_parent(path, &parent_ino, &basename) != 0)
        return -1;

    uint32_t new_ino = ext2_alloc_inode(0);
    if (new_ino == 0)
        return -1;

    uint32_t blk = ext2_alloc_block(0);
    if (blk == 0)
        return -1;

    ext2_inode_t inode;
    uint32_t ci;
    for (ci = 0; ci < sizeof(inode); ci++)
        ((uint8_t *)&inode)[ci] = 0;
    inode.i_mode = EXT2_S_IFDIR | (mode & 0x1FFu);
    inode.i_links_count = 2; /* "." + parent's ".." */
    inode.i_size = s_block_size;
    inode.i_blocks = s_block_size / 512;
    inode.i_block[0] = blk;
    ext2_write_inode(new_ino, &inode);

    /* initialise "." and ".." entries in the new block */
    uint8_t *data = cache_get_slot(blk);
    if (!data)
        return -1;
    uint32_t zi;
    for (zi = 0; zi < s_block_size; zi++)
        data[zi] = 0;

    /* "." entry */
    ext2_dirent_t *dot = (ext2_dirent_t *)data;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    /* ".." entry */
    ext2_dirent_t *dotdot = (ext2_dirent_t *)(data + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(s_block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';
    cache_mark_dirty(blk);

    /* increment parent link count for ".." back-reference */
    ext2_inode_t parent;
    if (ext2_read_inode(parent_ino, &parent) == 0) {
        parent.i_links_count++;
        ext2_write_inode(parent_ino, &parent);
    }

    /* update block group directory count */
    uint32_t grp = (new_ino - 1) / s_sb.s_inodes_per_group;
    if (grp < s_num_groups)
        s_bgd[grp].bg_used_dirs_count++;

    return ext2_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_DIR);
}

int ext2_rename(const char *old_path, const char *new_path)
{
    if (!s_mounted)
        return -1;

    uint32_t ino;
    if (ext2_open(old_path, &ino) != 0)
        return -1;

    uint32_t old_parent_ino;
    const char *old_basename;
    if (ext2_lookup_parent(old_path, &old_parent_ino, &old_basename) != 0)
        return -1;

    uint32_t new_parent_ino;
    const char *new_basename;
    if (ext2_lookup_parent(new_path, &new_parent_ino, &new_basename) != 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;
    uint8_t ftype = ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                    ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

    if (ext2_dir_remove_entry(old_parent_ino, old_basename) != 0)
        return -1;
    return ext2_dir_add_entry(new_parent_ino, ino, new_basename, ftype);
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
