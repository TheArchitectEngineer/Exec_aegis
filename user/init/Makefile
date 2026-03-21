CC = x86_64-elf-gcc

.PHONY: all clean

all: init.elf

init.elf: main.c
	$(CC) -ffreestanding -nostdlib -nostdinc -static \
	    -Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
	    -o init.elf main.c

clean:
	rm -f init.elf init_bin.c
