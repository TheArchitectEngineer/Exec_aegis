#include "pic.h"
#include "arch.h"
#include "printk.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20

/* ICW1: start init sequence, ICW4 needed */
#define ICW1_INIT 0x11
/* ICW4: 8086 mode */
#define ICW4_8086 0x01

/* Small delay via port 0x80 (POST diagnostic port — always safe to write) */
static void
io_wait(void)
{
    outb(0x80, 0);
}

void
pic_init(void)
{
    /* Start init sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT); io_wait();
    outb(PIC2_CMD,  ICW1_INIT); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20);      io_wait(); /* master: IRQ0-7 → 0x20-0x27 */
    outb(PIC2_DATA, 0x28);      io_wait(); /* slave:  IRQ8-15 → 0x28-0x2F */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);      io_wait(); /* master: slave on IRQ2 (bit 2) */
    outb(PIC2_DATA, 0x02);      io_wait(); /* slave: cascade identity = 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQs — drivers call pic_unmask() when ready */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    printk("[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F\n");
}

void
pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void
pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}

void
pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = irq & 7;
    outb(port, inb(port) | (1 << bit));
}
