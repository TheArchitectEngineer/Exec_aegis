CC      = musl-gcc
CFLAGS  = -static -O2 -Wall -Wextra
TARGET  = vigictl

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
