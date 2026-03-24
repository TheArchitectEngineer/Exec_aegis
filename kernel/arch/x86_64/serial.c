#include "serial.h"

/* COM1 base I/O port and register offsets */
#define COM1_BASE   0x3F8
#define COM1_DATA   (COM1_BASE + 0)   /* Data register (DLAB=0) / DLL (DLAB=1) */
#define COM1_IER    (COM1_BASE + 1)   /* Interrupt Enable (DLAB=0) / DLH (DLAB=1) */
#define COM1_FCR    (COM1_BASE + 2)   /* FIFO Control */
#define COM1_LCR    (COM1_BASE + 3)   /* Line Control */
#define COM1_MCR    (COM1_BASE + 4)   /* Modem Control */
#define COM1_LSR    (COM1_BASE + 5)   /* Line Status */

#define LSR_TXEMPTY (1 << 5)          /* Transmit-hold-register empty */

/* outb — write byte to I/O port.
 * Clobbers: none (volatile prevents reordering). */
static inline void outb(unsigned short port, unsigned char val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* inb — read byte from I/O port. */
static inline unsigned char inb(unsigned short port)
{
    unsigned char val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void)
{
    outb(COM1_IER, 0x00);   /* disable all interrupts */
    outb(COM1_LCR, 0x80);   /* enable DLAB to set baud rate divisor */
    outb(COM1_DATA, 0x01);  /* divisor low byte: 1 → 115200 baud */
    outb(COM1_IER,  0x00);  /* divisor high byte: 0 */
    outb(COM1_LCR, 0x03);   /* 8 data bits, no parity, 1 stop bit; clear DLAB */
    outb(COM1_FCR, 0xC7);   /* enable FIFO, clear TX/RX, 14-byte threshold */
    outb(COM1_MCR, 0x0B);   /* assert DTR, RTS, OUT2 */

    serial_write_string("[SERIAL] OK: COM1 initialized at 115200 baud\n");
}

void serial_write_char(char c)
{
    /* Emit \r before every \n so terminal emulators render correctly. */
    if (c == '\n') {
        while ((inb(COM1_LSR) & LSR_TXEMPTY) == 0) {}
        outb(COM1_DATA, '\r');
    }
    /* Spin until transmit-hold register is empty */
    while ((inb(COM1_LSR) & LSR_TXEMPTY) == 0) {}
    outb(COM1_DATA, (unsigned char)c);
}

void serial_write_string(const char *s)
{
    while (*s != '\0') {
        serial_write_char(*s);
        s++;
    }
}
