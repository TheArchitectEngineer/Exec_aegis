#ifndef AEGIS_KBD_H
#define AEGIS_KBD_H

#include <stdint.h>

/* Initialize PS/2 keyboard. Unmasks IRQ1.
 * Prints [KBD] OK: PS/2 keyboard ready. */
void kbd_init(void);

/* Called by isr_dispatch on vector 0x21.
 * Reads scancode from port 0x60, converts to ASCII, pushes to ring buffer.
 * Break codes (bit 7 set) and 0xE0 extended scancodes are silently dropped. */
void kbd_handler(void);

/* Blocking read — spins until a character is available in the ring buffer. */
char kbd_read(void);

/* Non-blocking read. Returns 1 and writes to *out if a char is available.
 * Returns 0 if the buffer is empty. */
int kbd_poll(char *out);

/* Register the foreground process PID for Ctrl-C delivery.
 * Called by the shell via sys_setfg (syscall 360) before waitpid.
 * Call with pid=0 to clear (no foreground process). */
void kbd_set_foreground_pid(uint32_t pid);

/* Like kbd_read() but returns '\0' and sets *interrupted=1 if a signal
 * is pending for the current process. Returns the character otherwise. */
char kbd_read_interruptible(int *interrupted);

#endif /* AEGIS_KBD_H */
