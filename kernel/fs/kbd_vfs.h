#ifndef KBD_VFS_H
#define KBD_VFS_H

#include "vfs.h"

/* kbd_vfs_open — return a pointer to the static keyboard vfs_file_t.
 * Used by proc_spawn to pre-populate fd 0 (stdin).
 * The keyboard hardware is already initialised by kbd_init() in kernel_main;
 * no separate kbd_vfs_init() step is needed. */
vfs_file_t *kbd_vfs_open(void);

#endif /* KBD_VFS_H */
