CC     = musl-gcc
CFLAGS = -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none

whoami.elf: whoami.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f whoami.elf
