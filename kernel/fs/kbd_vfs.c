#include "kbd_vfs.h"
#include "vfs.h"
#include "kbd.h"
#include <stdint.h>

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
	(void)priv; (void)off;	/* stream device — offset ignored */
	uint64_t i;
	char *kbuf = (char *)buf;
	/* kbd_read() blocks until a key is available. buf is a kernel buffer
	 * (sys_read's stack); sys_read copies to user space via copy_to_user. */
	for (i = 0; i < len; i++)
		kbuf[i] = kbd_read();
	return (int)len;
}

static int
kbd_vfs_write_fn(void *priv, const void *buf, uint64_t len)
{
	(void)priv; (void)buf; (void)len;
	return -38; /* ENOSYS — stdin is not writable */
}

static void
kbd_vfs_close_fn(void *priv)
{
	(void)priv; /* stateless singleton — nothing to release */
}

static const vfs_ops_t s_kbd_ops = {
	.read  = kbd_vfs_read_fn,
	.write = kbd_vfs_write_fn,
	.close = kbd_vfs_close_fn,
};

static vfs_file_t s_kbd_file = {
	.ops    = &s_kbd_ops,
	.priv   = (void *)0,
	.offset = 0,
};

vfs_file_t *
kbd_vfs_open(void)
{
	return &s_kbd_file;
}
