/* kernel/fs/ramfs.h — in-memory filesystem for /run */
#ifndef AEGIS_RAMFS_H
#define AEGIS_RAMFS_H

#include "vfs.h"

/* ramfs_init — zero the file table. Call from vfs_init() before any open. */
void ramfs_init(void);

/* ramfs_open — open or create (if flags & VFS_O_CREAT) a file under /run.
 * name: short name without the "/run/" prefix (e.g. "vigil.pid").
 * Returns 0 on success and fills *out; -2 (ENOENT) if not found and
 * VFS_O_CREAT not set; -12 (ENOMEM) if the 16-slot table is full. */
int ramfs_open(const char *name, int flags, vfs_file_t *out);

/* ramfs_stat_path — fill *st for a ramfs file given its short name.
 * Caller passes the name without the "/run/" prefix.
 * Returns 0 on success, -2 (ENOENT) if file has not been created yet. */
int ramfs_stat_path(const char *name, k_stat_t *st);

#endif /* AEGIS_RAMFS_H */
