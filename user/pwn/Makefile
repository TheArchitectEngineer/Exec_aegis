CC      = musl-gcc
CFLAGS  = -O2 -static -Wall -Wextra -Wno-unused-parameter

pwn.elf: pwn.c
	$(CC) $(CFLAGS) -o pwn.elf pwn.c

clean:
	rm -f pwn.elf *.o
