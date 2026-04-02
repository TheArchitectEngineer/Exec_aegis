# BUG: Kernel boot splash requires GRUB menu delay to function

**Date:** 2026-04-01
**Severity:** MEDIUM — graphical boot broken without GRUB menu visible
**Status:** Backlogged
**Workaround:** `set timeout=3` in grub.cfg (currently applied)

## Symptom

With `set timeout=0` or `timeout=1`, the GRUB menu auto-selects before the display is fully initialized. The kernel boot splash (`fb_boot_splash`) then either:
- Fails to draw (framebuffer not ready)
- Holds `s_fb_locked` or leaves FB state that prevents Bastion/Lumen from acquiring it

With `timeout=3`, the GRUB menu renders, the display pipeline initializes, and the kernel splash + GUI compositor work correctly.

## Likely root cause

The framebuffer initialization in `fb_init()` depends on the GRUB/BIOS video mode being fully established. When GRUB auto-selects instantly (`timeout=0`), the display hardware may not have completed mode-setting before the kernel starts drawing. The GRUB menu rendering itself acts as implicit proof the video mode is ready.

## Files to investigate

- `kernel/drivers/fb.c` — `fb_init`, `fb_boot_splash`, `fb_boot_splash_end`, `s_fb_locked`
- `kernel/core/main.c` — boot splash call sequence
- `tools/grub.cfg` — timeout value

## What to check

1. Does `fb_init` validate that the framebuffer is actually mapped and writable?
2. Is there a race between `fb_boot_splash` drawing and the display pipeline being ready?
3. Does `fb_boot_splash_end` properly clear all lock state?
4. Should `fb_init` add a small delay or readback check to confirm mode-set completion?
