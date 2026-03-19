#!/usr/bin/env bash
set -e

KERNEL=build/aegis.elf
EXPECTED=tests/expected/boot.txt
ACTUAL=/tmp/aegis_serial.txt

# Boot headless. QEMU exits with code 3 when kernel writes 0x01 to
# port 0xf4 (isa-debug-exit: exit_code = (value << 1) | 1 = 3).
# timeout prevents make test from hanging if kernel never signals exit.
timeout 10s qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -nographic \
    -nodefaults \
    -serial stdio \
    -no-reboot \
    -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > "$ACTUAL" 2>/dev/null || true
# || true: QEMU exits 3 on clean kernel exit; set -e would treat that as failure.
# -nodefaults: prevents firmware messages from contaminating serial output.

diff "$EXPECTED" "$ACTUAL"
# diff exits 0 on match, 1 on mismatch. Expected first: missing lines show as -,
# unexpected lines as +.
