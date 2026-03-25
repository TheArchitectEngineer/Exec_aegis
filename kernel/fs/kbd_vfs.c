#include "kbd_vfs.h"
#include "vfs.h"
#include "kbd.h"
#include "proc.h"
#include "sched.h"
#include "printk.h"
#include <stdint.h>

/* Simple line discipline — echoes characters as typed, handles backspace.
 * Buffers one line at a time; returns characters one-by-one to the caller.
 * No TTY termios layer: Enter → \n, DEL/BS → destructive backspace. */
#define KBD_LINE_MAX 512
static char s_linebuf[KBD_LINE_MAX];
static int  s_linebuf_len = 0;  /* total bytes ready in buffer (incl. \n) */
static int  s_linebuf_pos = 0;  /* next byte to return to caller */

static int
kbd_vfs_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
	(void)priv; (void)off;
	if (len == 0) return 0;
	char *kbuf = (char *)buf;

	/* Return buffered bytes from a previously-read line */
	if (s_linebuf_pos < s_linebuf_len) {
		kbuf[0] = s_linebuf[s_linebuf_pos++];
		if (s_linebuf_pos >= s_linebuf_len) {
			s_linebuf_len = 0;
			s_linebuf_pos = 0;
		}
		return 1;
	}

	/* Read and echo characters until Enter */
	for (;;) {
		int  interrupted;
		char c = kbd_read_interruptible(&interrupted);
		if (interrupted) {
			s_linebuf_len = 0;
			s_linebuf_pos = 0;
			return -4; /* EINTR */
		}

		if (c == '\r' || c == '\n') {
			/* Enter: store \n, echo newline, return line */
			if (s_linebuf_len < KBD_LINE_MAX - 1)
				s_linebuf[s_linebuf_len++] = '\n';
			printk("\n");
			break;
		} else if (c == '\x7f' || c == '\x08') {
			/* Backspace / DEL: remove last char, destructive echo */
			if (s_linebuf_len > 0) {
				s_linebuf_len--;
				printk("\b \b");
			}
		} else if ((uint8_t)c >= 0x20 && s_linebuf_len < KBD_LINE_MAX - 1) {
			/* Printable: append and echo */
			s_linebuf[s_linebuf_len++] = c;
			printk("%c", c);
		}
		/* Ignore other control chars */
	}

	/* Return first byte of the completed line */
	kbuf[0] = s_linebuf[s_linebuf_pos++];
	if (s_linebuf_pos >= s_linebuf_len) {
		s_linebuf_len = 0;
		s_linebuf_pos = 0;
	}
	return 1;
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

static int
kbd_stat_fn(void *priv, k_stat_t *st)
{
	(void)priv;
	__builtin_memset(st, 0, sizeof(*st));
	st->st_mode  = S_IFCHR | 0400;
	st->st_ino   = 3;
	st->st_rdev  = makedev(4, 0);  /* /dev/tty: major=4 minor=0 */
	st->st_dev   = 1;
	st->st_nlink = 1;
	return 0;
}

static const vfs_ops_t s_kbd_ops = {
	.read    = kbd_vfs_read_fn,
	.write   = kbd_vfs_write_fn,
	.close   = kbd_vfs_close_fn,
	.readdir = (void *)0,
	.dup     = (void *)0,
	.stat    = kbd_stat_fn,
};

static vfs_file_t s_kbd_file = {
	.ops    = &s_kbd_ops,
	.priv   = (void *)0,
	.offset = 0,
	.size   = 0,
};

vfs_file_t *
kbd_vfs_open(void)
{
	return &s_kbd_file;
}
