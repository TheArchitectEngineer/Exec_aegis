MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -I../glyph

SRCS = main.c cursor.c compositor.c dock.c taskbar.c terminal.c
OBJS = $(SRCS:.c=.o)
GLYPH = ../glyph/libglyph.a

all: lumen.elf

$(GLYPH):
	$(MAKE) -C ../glyph

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lumen.elf: $(OBJS) $(GLYPH)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -L../glyph -lglyph

clean:
	rm -f *.elf *.o
