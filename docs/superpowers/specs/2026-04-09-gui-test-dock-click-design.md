# GUI Test Harness — Dock Click Automation Design

**Date:** 2026-04-09
**Status:** Draft

## Problem

The test harness can boot Lumen, wait for `[LUMEN] ready`, and capture a screenshot,
but it cannot *interact* with the GUI. Every visual regression to date has been of the
fade-in desktop state. We have no way to drive a click → launch → screenshot cycle, so
regressions in Terminal, Widget Test, Settings, and About go uncaught.

Phase 47 (GUI installer) will be impossible to test end-to-end without this.

## Approach

Extend the existing test harness with three additions — no new machinery:

1. **Lumen emits dock geometry to serial** on `ready`. One line per icon with a stable
   key, pixel center, and half-extent. The test scrapes these lines the same way it
   scrapes `[LUMEN] ready` today.

2. **Vortex gains `mouse_move(x, y)` and `mouse_button(btn)` HMP methods** — thin wrappers
   on the same monitor socket that already carries `screendump` and `sendkey`. QEMU's
   PS/2 emulated mouse receives the events; Lumen reads them through `/dev/mouse`.

3. **New test `dock_click_test.rs`** that boots, parses dock coords from serial, then for
   each dock item: moves the mouse to the center, clicks, waits for a `[LUMEN] window_opened=<key>`
   acknowledgement, screendumps to `tests/golden/dock_<key>.ppm`, and fuzzy-diffs against
   a golden image.

The PS/2 mouse must be present in the QEMU config. The current `screendump_test.rs`
strips `usb-mouse` from the q35 preset; the new test keeps the q35 default plus an
explicit `ps2-mouse` (or relies on the ICH9 i8042 aux channel, whichever QEMU exposes).

## Flow

```
  test                        vortex                    QEMU                 lumen
   │   boot(aegis_q35)          │                         │                     │
   ├───────────────────────────>│ spawn qemu              │                     │
   │                            ├────────────────────────>│                     │
   │                            │                         │   load, run init    │
   │                            │  serial stream          │                     │
   │   <──[LUMEN] ready──────────────────────────────────────────────────────────
   │   <──[DOCK] item=terminal cx=820 cy=1020 hw=32 hh=32 ─────────────────────
   │   <──[DOCK] item=widgets cx=870 cy=1020 ... ───────────────────────────────
   │                            │                         │                     │
   │   mouse_move(820, 1020)    │                         │                     │
   ├───────────────────────────>│ HMP mouse_move          │                     │
   │                            ├────────────────────────>│                     │
   │   mouse_button(1)          │                         │ PS/2 byte           │
   ├───────────────────────────>│ HMP mouse_button        ├────────────────────>│
   │                            │                         │                     │ dock_hit_test
   │                            │                         │                     │ spawn_terminal
   │   <──[LUMEN] window_opened=terminal ─────────────────────────────────────
   │   screendump(tests/out/dock_terminal.ppm)            │                     │
   ├───────────────────────────>│ HMP screendump          │                     │
   │                            ├────────────────────────>│                     │
   │                            │                         │ write ppm           │
   │   fuzzy_diff(golden, out)  │                         │                     │
   │                            │                         │                     │
```

## Components

### Vortex — mouse control

`src/core/qemu.rs`: add two methods to `QemuProcess`, mirroring `send_keys` exactly.

```rust
pub async fn mouse_move(&self, x: i32, y: i32) -> Result<(), QemuError>;
pub async fn mouse_button(&self, button: MouseButton) -> Result<(), QemuError>;

pub enum MouseButton { Left, Right, Middle }
```

Implementation writes `mouse_move <x> <y>\n` / `mouse_button <n>\n` to the monitor
socket and awaits a blank-line response, exactly like `screendump()`. QEMU coordinates
are absolute for `mouse_move` on the PS/2 aux device (no trackball deltas).

No new dependencies. No new types that leak into public API besides `MouseButton`.

### Lumen — dock geometry emission

`user/bin/lumen/main.c` at startup, right after `[LUMEN] ready`: iterate the dock's
items, call `dock_item_rect(i, &x, &y, &w, &h)` for each, and print:

```
[DOCK] item=<key> idx=<i> cx=<x+w/2> cy=<y+h/2> hw=<w/2> hh=<h/2>
```

Keys come from a new static table `const char *DOCK_ITEM_KEYS[]` in `dock.c` matching
the existing `DOCK_ITEM_TERMINAL`/`DOCK_ITEM_WIDGETS`/... enum order. One key per enum
value. Exposed via `dock_item_key(idx)`.

On successful app launch, Lumen's click dispatch in `main.c` prints:

```
[LUMEN] window_opened=<key>
```

The test uses this as its wait-for-window signal. No sleeps.

### Test — `tests/tests/dock_click_test.rs`

Structure:

```rust
#[tokio::test]
async fn dock_icons_launch_apps() {
    let (qemu, console) = harness::boot(aegis_q35_with_mouse()).await?;
    console.wait_for("[LUMEN] ready", 30s).await?;

    let dock = parse_dock_items(&console.lines_so_far())?;  // HashMap<String, (i32, i32)>

    for (key, (cx, cy)) in &dock {
        qemu.mouse_move(*cx, *cy).await?;
        qemu.mouse_button(MouseButton::Left).await?;
        console.wait_for(&format!("[LUMEN] window_opened={}", key), 5s).await?;

        // Small settle to let alpha fade complete.
        tokio::time::sleep(Duration::from_millis(500)).await;

        let out = format!("tests/out/dock_{}.ppm", key);
        qemu.screendump(&out).await?;
        image::fuzzy_diff(&out, &format!("tests/golden/dock_{}.ppm", key), 0.5)?;

        // Close the window before clicking the next item so screenshots are clean.
        qemu.send_keys("esc").await?;
    }
}
```

A helper `aegis_q35_with_mouse()` in `tests/src/presets.rs` clones `aegis_q35()` and
asserts the PS/2 mouse path (does not strip it the way screendump_test does).

Goldens live under `tests/golden/dock_<key>.ppm`. First run captures them by hand with
`AEGIS_UPDATE_GOLDENS=1` and commits.

### Scope — what's NOT in this spec

- Topbar menu automation (two-step click). Deferred to a follow-up.
- Window drag / resize tests.
- Mouse-over / hover tests.
- Right-click / middle-click tests (only `Left` is wired in this iteration, though the
  `MouseButton` enum includes all three).
- Any change to the kernel PS/2 mouse driver. We rely on what's already shipped.

## Error Handling

- **Vortex mouse methods** return `QemuError` on socket write failure or HMP parse
  error. Same pattern as `screendump`.
- **Missing `[DOCK]` lines** in the serial stream: test fails with
  `"no dock items found in serial output"`. This catches cases where Lumen crashed
  before emission.
- **Missing `window_opened=<key>`** after click: 5s timeout → test fails with
  `"clicked {key} but no window opened"`. This catches regressions in `dock_hit_test`
  or `sys_spawn`.
- **Goldens missing**: test fails with a specific "run with AEGIS_UPDATE_GOLDENS=1"
  hint instead of a generic diff error.

## Testing Strategy

Self-test the harness first: boot → parse dock → click Terminal → screendump. Commit
the golden. Then extend to all dock items. Then run the full test on the x86 build box
to confirm it passes green before landing.

A failure of `dock_click_test` after landing indicates one of:
1. Dock layout changed (update golden).
2. App launch regressed (fix the kernel / lumen).
3. Screendump pipeline broken (fix harness).

Classification is by which wait-for line failed.

## Open questions

None. Everything above is concretely specified.
