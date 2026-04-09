#ifndef AEGIS_PRINTK_H
#define AEGIS_PRINTK_H

#include <stdint.h>

/* printk — route formatted output to serial and VGA.
 * Supports: %s (string), %c (char), %u (uint32_t), %lu (uint64_t),
 *           %x (hex uint32_t), %lx (hex uint64_t), %% (literal %). */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* printk_set_quiet — suppress VGA+FB output in printk, serial only.
 * Console device (user output) bypasses this via direct serial+fb writes. */
void printk_set_quiet(int q);
int  printk_get_quiet(void);

/* klog_read — copy up to bufsz bytes of the kernel log ring buffer into buf.
 * Returns the number of bytes written. Safe to call from any context. */
uint32_t klog_read(char *buf, uint32_t bufsz);

#endif /* AEGIS_PRINTK_H */
