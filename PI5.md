# Booting Aegis on Raspberry Pi 5

Step-by-step guide to flashing Aegis onto a Pi 5 and getting it booted over a serial console. This is the practical "I have the hardware in front of me" companion to [ARM64.md](ARM64.md) §17-18, which has the technical deep dive on GICv3, DTB parsing, and memory layout.

**Hardware status as of 2026-04-12:** untested on real silicon. Everything in this guide has been verified under `qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a76` which emulates the BCM2712's GIC-600 closely but not perfectly. First-boot diagnosis on real hardware requires a USB-TTL serial cable.

---

## 1. Shopping list

You need three things besides the Pi 5:

| Item | Cost | Notes |
|------|------|-------|
| **USB-TTL serial cable** (Adafruit #954 or equivalent) | ~$10 | See §2 for the exact cable |
| **SD card, any size ≥ 128 MB, FAT32-formatted** | ~$5 | The Aegis boot image is 6.1 MB. Any microSD card works. |
| **USB-C power supply for the Pi 5** | ~$15 | 27W official Pi 5 PSU recommended. 5V 3A minimum. |

You do **not** need: HDMI cable, monitor, USB keyboard, Ethernet cable, Wi-Fi. All interaction with Aegis during first boot is through the serial console.

---

## 2. The serial cable

### Primary recommendation: Adafruit #954

**[adafruit.com/product/954](https://www.adafruit.com/product/954)** — $9.95, in stock, uses Silicon Labs CP2102 chip, 3.3V logic levels (correct for the Pi), and comes with pre-terminated female sockets that drop directly onto the Pi GPIO header pins.

**Wire colors** (this is the canonical Adafruit 954 layout):

| Wire | Function |
|------|----------|
| **Red** | +5V power from USB — **DO NOT CONNECT TO THE PI** |
| **Black** | GND |
| **White** | RX into the USB port (data flowing *from* the Pi) |
| **Green** | TX out of the USB port (data flowing *to* the Pi) |

### Amazon alternatives (if you need it faster or prefer Prime)

- **[Waveshare Industrial USB to TTL (D) on Amazon](https://www.amazon.com/waveshare-USB-Serial-Adapter-Cable/dp/B0CYGL421W)** — uses FT232RNL chip, Pi 5-specific, switchable 3.3V/5V logic, better ESD/fuse protection than Adafruit. ~$10-15. Slightly bulkier but rock-solid. Comes with SH1.0 3-pin cable for Pi 5's dedicated UART header AND a separated 4-pin header for the 40-pin GPIO (you'll use the GPIO version).
- **[FTDI TTL-232R-3V3 on Amazon](https://www.amazon.com/FTDI-TTL-232R-RPI-DEBUG-RASPBERRY-MODULE/dp/B00HKJOVK4)** — the "original" FTDI-branded cable, ~$25. Overkill but if you already trust FTDI, it works.

### What NOT to buy

- **Generic $3 CP2102 modules with breakaway pin headers** — you'd also need female-to-female jumper wires, and those modules are poorly QC'd. False economy.
- **RS-232 serial adapters (DB9 connectors)** — these are +12V/-12V logic, will destroy the Pi if connected directly. You want **TTL serial**, not RS-232.
- **Prolific PL2303 cables** — work on most platforms but have recurring macOS driver issues. CP2102 and FT232RNL are more reliable on macOS.

---

## 3. Pi 5 GPIO pinout

The Pi 5 uses the same 40-pin GPIO header as Pi 4, Pi 3, and Pi B+. The debug UART (UART0 / PL011) is on **physical pins 8 and 10**.

Viewing the Pi 5 with the **GPIO header at the top edge**, **USB/Ethernet at the bottom edge**, **HDMI on the left**:

```
                  ┌─────────────── 40-pin GPIO header ───────────────┐
                  │                                                  │
     Pin 1 (3.3V) ┤ ● ●                                              ├ Pin 2 (5V)    ── DO NOT USE
    Pin 3 (SDA1)  ┤ ● ●                                              ├ Pin 4 (5V)    ── DO NOT USE
    Pin 5 (SCL1)  ┤ ● ●                                              ├ Pin 6 (GND)   ◄── Black wire
    Pin 7 (GPIO4) ┤ ● ●                                              ├ Pin 8 (TXD0)  ◄── White wire
    Pin 9 (GND)   ┤ ● ●                                              ├ Pin 10 (RXD0) ◄── Green wire
    Pin 11...     ┤ ● ●                                              ├ Pin 12...
                  │  ⋮    (remaining 28 pins not used)  ⋮             │
                  └──────────────────────────────────────────────────┘
```

**Pin 1 is the corner closest to the SD card slot and micro-HDMI ports.** Pins are numbered: odd on the outer row (closer to the board edge), even on the inner row.

| Physical pin | GPIO | Function | Cable wire |
|--------------|------|----------|-----------|
| Pin 2 | — | +5V | **Leave disconnected** |
| Pin 4 | — | +5V | **Leave disconnected** |
| Pin 6 | — | GND | **Black** (GND) |
| Pin 8 | GPIO 14 | UART0 TXD (Pi transmits) | **White** (cable RX) |
| Pin 10 | GPIO 15 | UART0 RXD (Pi receives) | **Green** (cable TX) |

Pin 6 and Pin 8 are adjacent. Pin 8 and Pin 10 are adjacent. All three cable connections land on three consecutive positions on the inner row — easy.

### Critical: the red wire

**Do not connect the red +5V wire from the cable to anything on the Pi.** The Pi is self-powered from its USB-C port. The red wire carries +5V from your laptop's USB port. If you connect it to a 5V pin on the Pi, you have two independent power supplies feeding the same rail, which can:

1. Back-feed current through your laptop's USB port
2. Interfere with the Pi's own power-on-reset timing
3. In extreme cases, damage the laptop USB port or the Pi

Just fold the red wire back along the cable body and tape it down, or snip it off at the connector with wire cutters. It's the one wire you never touch.

### Cross-over wiring

Notice that **Pi TX connects to cable RX**, and **Pi RX connects to cable TX**. The signals cross over. This is because "TX" and "RX" are named from whichever device is talking — data flowing *out of* one device's TX pin flows *into* the other device's RX pin.

If you wire them straight-through (Pi TX → cable TX, Pi RX → cable RX), you'll see nothing. No smoke, just silence. This is the single most common wiring mistake.

---

## 4. Building the Pi 5 image

From the project root on branch `arm64-port`:

```bash
# 1. Build the ARM64 kernel (Linux arm64 Image format)
make -C kernel/arch/arm64 image

# 2. Package it with Pi firmware into a ready-to-flash directory
bash tools/build-pi5-image.sh
```

The first run downloads Pi firmware blobs (~6 MB total) from [github.com/raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot) and caches them in `references/pi-firmware/`. Subsequent runs are offline.

**Output:** `build/pi5-image/` containing:

```
bcm2712-rpi-5-b.dtb    78 KB   # Pi 5 device tree blob
bootcode.bin           52 KB   # early bootloader (harmless on Pi 5, not strictly used)
config.txt            831 B    # boot config (kernel name, UART enable, load address)
fixup.dat              7 KB    # memory pre-load config
fixup4.dat             5 KB    # memory pre-load config (newer firmware)
kernel8.img           869 KB   # Aegis kernel in Linux arm64 Image format
start.elf             3.0 MB   # GPU firmware (main bootloader)
start4.elf            2.3 MB   # GPU firmware (newer, used on Pi 5)
```

Total: 6.1 MB.

### config.txt contents

The build script writes this exact file:

```
arm_64bit=1
kernel=kernel8.img
device_tree=bcm2712-rpi-5-b.dtb
kernel_address=0x200000
enable_uart=1
uart_2ndstage=1
disable_commandline_tags=1
```

The key settings:

- **`arm_64bit=1`** — boot the kernel as aarch64, not 32-bit ARM
- **`kernel=kernel8.img`** — tells the firmware which file to load
- **`device_tree=bcm2712-rpi-5-b.dtb`** — the Pi 5 device tree that `arch_mm.c` parses for memory layout and GIC-600 detection
- **`kernel_address=0x200000`** — overrides the firmware's default load address (0x80000). Aegis is linked with `KERN_LMA = 0x40200000`, which equals Pi 5's RAM base (0x40000000) + 0x200000 offset. Without this override, the Image header branch at offset 0 would land at the wrong address.
- **`enable_uart=1`** + **`uart_2ndstage=1`** — enables the debug UART on GPIO 14/15 at 115200 8N1 and makes the GPU firmware itself emit status info over serial (which gives you a diagnosis channel even if the kernel never starts).
- **`disable_commandline_tags=1`** — suppresses the firmware appending its own cmdline to the kernel, which would violate the arm64 boot protocol.

---

## 5. Flashing the SD card

### macOS

```bash
# Identify the SD card device (will be /dev/diskN where N is the number)
diskutil list

# Format as FAT32, single partition. Replace 'diskN' with your actual device.
diskutil eraseDisk FAT32 AEGISBOOT MBRFormat /dev/diskN

# Copy everything from build/pi5-image/ to the card root
cp -v build/pi5-image/* /Volumes/AEGISBOOT/

# Flush and eject
diskutil eject /dev/diskN
```

**Warning:** `diskutil eraseDisk` will destroy everything on whatever disk you point it at. Double-check `diskN` is your SD card and not your internal drive. Your Mac's internal SSD is almost always `disk0` or `disk1`, and SD cards are usually `disk3` or higher.

### Linux

```bash
# Identify the SD card
lsblk

# Partition and format (replace /dev/sdX with your actual device)
sudo parted /dev/sdX --script mklabel msdos mkpart primary fat32 1MiB 100%
sudo mkfs.vfat -F 32 -n AEGISBOOT /dev/sdX1

# Mount and copy
sudo mkdir -p /mnt/sd
sudo mount /dev/sdX1 /mnt/sd
sudo cp -v build/pi5-image/* /mnt/sd/
sudo umount /mnt/sd
```

Same warning: verify `/dev/sdX` before running. `sudo fdisk -l` will show sizes so you can tell your SSD apart from the SD card.

---

## 6. Physical setup

1. **Insert the SD card** into the Pi 5's slot (underside of the board, near the USB-C port).
2. **Wire the serial cable** to the GPIO header per §3. Three wires:
   - Black → Pin 6 (GND)
   - White → Pin 8 (GPIO 14 TXD)
   - Green → Pin 10 (GPIO 15 RXD)
   - Red → **not connected** (tape it back or snip it off)
3. **Plug the USB-TTL cable's USB end into your laptop.** Don't power the Pi yet.
4. **Open a serial terminal** on your laptop (see §7). Leave it running, ready to capture output.
5. **Plug the Pi 5 into USB-C power.** Boot starts immediately.

---

## 7. Serial terminal on your laptop

### macOS

```bash
# Find the serial device (cable must be plugged in)
ls /dev/tty.usbserial-*
# Output like: /dev/tty.usbserial-1410

# Open terminal
screen /dev/tty.usbserial-1410 115200
```

Or if you prefer `minicom`:

```bash
brew install minicom
minicom -b 115200 -o -D /dev/tty.usbserial-1410
```

### Linux

```bash
# Find the serial device
ls /dev/ttyUSB*
# Output: /dev/ttyUSB0

# You may need to add yourself to the dialout group first:
sudo usermod -aG dialout $USER   # then log out + back in

# Open terminal
screen /dev/ttyUSB0 115200
```

Or:

```bash
sudo apt install minicom
minicom -b 115200 -o -D /dev/ttyUSB0
```

### Exiting `screen`

Press **Ctrl-A**, then **K**, then **y** to confirm. If your terminal refuses to close cleanly: `killall screen` from another terminal.

### Settings

Baud rate **115200**, **8 data bits**, **no parity**, **1 stop bit** (8N1). No flow control. These are the defaults for both `screen` and `minicom` with the commands above, so you shouldn't need to change anything.

---

## 8. Expected boot output

### Success

If everything works, within a second of powering on the Pi you'll see output like:

```
[firmware banner from start4.elf if uart_2ndstage=1]

[SERIAL] OK: PL011 UART initialized
[PMM] OK: 4096MB usable across 1 regions
[VMM] OK: ARM64 4KB-granule page tables active
[VMM] OK: mapped-window allocator active
[KVA] OK: kernel virtual allocator active
[GIC] OK: GICv3 initialized                     ← critical line
[TIMER] OK: ARM generic timer at 100 Hz
[CAP] OK: capability subsystem initialized
[RNG] OK: ChaCha20 CSPRNG seeded
[VFS] OK: initialized
[INITRD] OK: 33 files registered
[CAP] OK: 7 baseline capabilities granted to init
[SCHED] OK: scheduler started, 2 tasks
vigil: starting
vigil: boot mode: text
vigil: getty
vigil: httpd
vigil: dhcp

 _______ _______  ______ _____ _______
 |_____| |______ |  ____   |   |______
 |     | |______ |_____| __|__ ______|

 WARNING: This system is restricted to authorized users.
 All activity is monitored and logged. Unauthorized access
 will be investigated and may result in prosecution.


login:
```

The `[GIC] OK: GICv3 initialized` line is the most important indicator that the new driver works on real silicon — it means the GIC-600 distributor and redistributor came up correctly, system registers are configured, and interrupts will flow.

If you reach the `login:` prompt, **everything works**. Try `root` + `forevervigilant` for login credentials. (The password comes from `tests/tests/login_flow_test.rs` — not a secret, just a reminder.) See §12 for known userland limitations.

### Partial success: firmware talks, kernel doesn't

```
Raspberry Pi Bootloader
Boot mode: SD (...)
...
[some firmware output]
[then nothing]
```

Means the Pi firmware loaded, found `config.txt`, loaded `kernel8.img`, and jumped to it — but our kernel isn't printing anything. Most likely cause: the PL011 MMIO probe in `uart_pl011.c` isn't finding the UART at 0xFE201000. Possible fixes:

1. Verify the Pi firmware output shows `uart_2ndstage=1` succeeded. If you see NO output at all, the cable or wiring is wrong — the firmware's own UART output is using the same pins and we'd see it.
2. If the firmware banner appears but the kernel banner doesn't, the kernel is running but PL011 probe failed. Edit `kernel/arch/arm64/uart_pl011.c` and hardcode `s_uart_base = (volatile uint32_t *)(0xFE201000UL + KERN_VA_OFFSET);` as the first line of `uart_init()`. Rebuild and reflash only `kernel8.img`.

### Complete silence

No output at all, not even the Pi firmware banner. The Pi power LED lights up (so power is fine) but nothing appears on the serial terminal.

**Most likely: cable wiring issue.** Go back to §3 and verify:

1. Black → Pin 6 (GND)
2. White → Pin 8 (GPIO 14 TX)
3. Green → Pin 10 (GPIO 15 RX)
4. Red → **not connected to anything**

The most common mistake is swapping TX/RX (straight-through instead of crossover). Also check the terminal baud rate is 115200, not 9600 or 38400.

**If wiring is definitely correct:** try the Pi's ACT LED (the green one next to the power LED). On boot it should blink briefly. If it doesn't blink at all, the firmware isn't loading — check that `start4.elf`, `fixup4.dat`, and `config.txt` are all in the SD card root (not in a subdirectory). If the ACT LED blinks a pattern (e.g., 4 long + 4 short), that's a firmware error code — [Pi Foundation docs](https://www.raspberrypi.com/documentation/computers/configuration.html#led-warning-flash-codes) explain the patterns.

### Kernel crashes partway through

If you see some `[XXXX] OK` lines but then a panic or hang:

- **Stops after `[SERIAL] OK`, before `[PMM] OK`:** DTB parsing is failing. The kernel can't find memory regions. Capture the exact last line and report back.
- **Stops after `[KVA] OK`, before `[GIC] OK`:** GICv3 driver is hanging, probably in `gicr_wake` (spinning on the redistributor's `ChildrenAsleep` bit). The register write/read sequence may need adjustment for real silicon vs QEMU's emulation. Capture the exact last line.
- **Stops after `[GIC] OK`, before `[TIMER] OK`:** timer PPI routing through the redistributor isn't working. Capture the exact last line.
- **`[PANIC]` followed by `ELR=...`, `ESR=...`:** a synchronous exception. **Paste the entire panic block back** and it's directly resolvable — ELR resolves to a source line via `addr2line`.

---

## 9. Iterating on a failed boot

If the kernel fails, you don't need to rebuild the entire SD card. Only `kernel8.img` changes between builds. Workflow:

```bash
# 1. Edit kernel source (fix the bug)
vim kernel/arch/arm64/uart_pl011.c

# 2. Rebuild the Image
make -C kernel/arch/arm64 image

# 3. Copy just the new kernel to the SD card
cp kernel/arch/arm64/build/aegis.img /Volumes/AEGISBOOT/kernel8.img
# or on Linux: sudo cp build/pi5-image/kernel8.img /mnt/sd/kernel8.img

# 4. Eject, reinsert into Pi, power cycle
```

A full cycle (edit → build → copy → reboot → read serial) is 30-60 seconds once you have the rhythm.

---

## 10. Reporting failures for remote debugging

If you can copy text out of your serial terminal: paste the entire boot trace from the first line back to me, and I can usually point at the bug in one round-trip.

Most useful details:

1. **Everything from the start of output through the last line before the hang or panic.** Don't trim — sometimes a subtle earlier line is the real clue.
2. **The Pi firmware version line** (e.g., `Raspberry Pi Bootloader ... 2025-XX-XX`). Firmware versions sometimes change boot behavior.
3. **Which Pi 5 model** — 4 GB or 8 GB. Memory map differs.
4. **Your cable model** (Adafruit 954, Waveshare, etc.) — different chips have slightly different timing characteristics.

If your terminal can't copy text (e.g., you're in `screen` in a VM and the clipboard is weird): take a phone photo of the screen. Legible is enough.

---

## 11. Reflashing the firmware from scratch

If the `build/pi5-image/` directory is missing or outdated and `tools/build-pi5-image.sh` fails:

```bash
# Nuke the cache and re-fetch
rm -rf references/pi-firmware/ build/pi5-image/
bash tools/build-pi5-image.sh
```

This re-downloads from GitHub. Requires internet.

If GitHub is blocked or you're offline, the firmware files can also be extracted from any Raspberry Pi OS image — mount the boot partition and copy `bootcode.bin`, `start4.elf`, `fixup4.dat`, `bcm2712-rpi-5-b.dtb` into `references/pi-firmware/` manually, then re-run the script.

---

## 12. What works and what doesn't on Pi 5

### Works (or expected to work, pending hardware verification)

- ARM64 kernel boot from Linux arm64 Image format
- PL011 debug UART at GPIO 14/15 for console
- ARM generic timer at 100 Hz
- GIC-600 interrupt controller (new GICv3 driver)
- Memory discovery from DTB (up to 8 GB)
- Kernel scheduler, VFS, initrd, capability subsystem (Rust)
- Vigil init system + service supervision
- Interactive login prompt

### Does not work yet

- **USB** — Pi 5's USB is behind the RP1 south bridge, which needs a PCIe host controller driver + RP1 driver. Far future (ARM64.md §17.6 deferred).
- **Ethernet** — same RP1 dependency. No network on Pi 5 until RP1 is done.
- **Wi-Fi / Bluetooth** — Pi 5's wireless is on a PCIe attached chip too. Deferred.
- **HDMI / display** — no display driver. Serial-only.
- **GPIO as I/O pins** — RP1 also owns GPIO on Pi 5. You can use the debug UART on GPIO 14/15 because that's routed through the legacy peripheral base (`0xFE201000`), but other GPIO pins need RP1.
- **SD card as read/write storage** — only used as boot media; we don't have an SD host controller driver.
- **Multi-core** — kernel runs single-core only on ARM64. Pi 5's 3 extra cores sit idle.
- **Interactive shell after login** — as of commit `f180295` the kernel and `execve` path work, but a userland crypt/termios issue prevents password acceptance. You'll reach `login:` but credentials won't validate. See ARM64.md §14-15 for the current state.

What works is useful: you can verify the kernel runs on real silicon, measure boot performance, and test GICv3 + UART driver correctness. That's the foundation for everything else.

---

## 13. Hardware references

- Pi 5 hardware overview — [raspberrypi.com/documentation/computers/raspberry-pi-5.html](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html)
- config.txt reference — [raspberrypi.com/documentation/computers/config_txt.html](https://www.raspberrypi.com/documentation/computers/config_txt.html)
- Pi firmware repository — [github.com/raspberrypi/firmware](https://github.com/raspberrypi/firmware)
- Pi 5 DTS source — [github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts](https://github.com/raspberrypi/linux)
- BCM2712 peripherals datasheet — [datasheets.raspberrypi.com/bcm2712/bcm2712-peripherals.pdf](https://datasheets.raspberrypi.com/bcm2712/bcm2712-peripherals.pdf)
- ARM GICv3 architecture spec — [developer.arm.com/documentation/ihi0069](https://developer.arm.com/documentation/ihi0069/latest)
- GIC-600 TRM — [developer.arm.com/documentation/101206](https://developer.arm.com/documentation/101206/latest)
- Pi ACT LED flash codes — [raspberrypi.com/documentation/computers/configuration.html](https://www.raspberrypi.com/documentation/computers/configuration.html#led-warning-flash-codes)

---

## 14. Quick-reference card

```
Cable:     Adafruit #954 (CP2102, 3.3V logic)
           $9.95 at adafruit.com/product/954

Wiring:    Black  → Pin 6  (GND)
           White  → Pin 8  (GPIO 14 TXD)
           Green  → Pin 10 (GPIO 15 RXD)
           Red    → NOT CONNECTED

Terminal:  screen /dev/tty.usbserial-* 115200      (macOS)
           screen /dev/ttyUSB0 115200              (Linux)
           Exit: Ctrl-A, K, y

Build:     make -C kernel/arch/arm64 image
           bash tools/build-pi5-image.sh

Flash:     diskutil eraseDisk FAT32 AEGISBOOT MBRFormat /dev/diskN
           cp build/pi5-image/* /Volumes/AEGISBOOT/
           diskutil eject /dev/diskN

Boot:      Insert SD, wire serial, USB-TTL into laptop, power Pi.
           Expect output within 1 second.

Iterate:   Only kernel8.img changes between rebuilds. Copy just that
           file to the SD card to save time.

Success:   [GIC] OK: GICv3 initialized → [SCHED] OK → login:

Login:     root / forevervigilant  (from login_flow_test.rs)
```
