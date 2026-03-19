#include "vga.h"
#include "serial.h"

#define VGA_BASE    ((unsigned short *)0xB8000)
#define VGA_COLS    80
#define VGA_ROWS    25
#define VGA_ATTR    0x07    /* light grey on black */

int vga_available = 0;

static int vga_col = 0;
static int vga_row = 0;

/* vga_cell — encode a character + attribute as a 16-bit VGA cell. */
static inline unsigned short vga_cell(char c, unsigned char attr)
{
    return (unsigned short)((unsigned char)c) | ((unsigned short)attr << 8);
}

/* vga_scroll — shift all rows up by one, clear the bottom row. */
static void vga_scroll(void)
{
    int i;
    /* Move rows 1..24 up to rows 0..23 */
    for (i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
        VGA_BASE[i] = VGA_BASE[i + VGA_COLS];
    }
    /* Clear bottom row */
    for (i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++) {
        VGA_BASE[i] = vga_cell(' ', VGA_ATTR);
    }
    vga_row = VGA_ROWS - 1;
}

void vga_init(void)
{
    int i;
    /* Clear entire screen */
    for (i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        VGA_BASE[i] = vga_cell(' ', VGA_ATTR);
    }
    vga_col = 0;
    vga_row = 0;
    vga_available = 1;

    /* Print directly — printk is not yet up at this point */
    vga_write_string("[VGA] OK: text mode 80x25\n");
    serial_write_string("[VGA] OK: text mode 80x25\n");
}

void vga_write_char(char c)
{
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_ROWS) {
            vga_scroll();
        }
        return;
    }

    VGA_BASE[vga_row * VGA_COLS + vga_col] = vga_cell(c, VGA_ATTR);
    vga_col++;
    if (vga_col >= VGA_COLS) {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_ROWS) {
            vga_scroll();
        }
    }
}

void vga_write_string(const char *s)
{
    while (*s != '\0') {
        vga_write_char(*s);
        s++;
    }
}
