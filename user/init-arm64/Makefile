CC = aarch64-elf-gcc
LD = aarch64-elf-ld

all: init.elf

init.elf: init.S
	$(CC) -nostdlib -static -o $@ $<

clean:
	rm -f init.elf
