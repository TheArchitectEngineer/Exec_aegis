# Aegis Test Harness Design

**Date:** 2026-04-09
**Status:** Approved

## Problem

`make test` is a no-op. The historical Python test suite drove QEMU via monitor socket and
diffed serial output against a static expected file. It was too slow, too brittle, and is
no longer running. Vortex now has a `QemuBackend` with serial capture and structured exit
handling. The Aegis side needs to be built on top of it.

## Approach

Two independent test surfaces with no shared runtime:

```
cargo test                           vortex stack up <stack>
     │                                       │
     ▼                                       ▼
tests/Cargo.toml                    .vortex/config.toml
  └─ vortex (library)                 └─ templates: aegis-pc, aegis-q35
       └─ QemuBackend                 └─ stacks: aegis-install-test
       └─ ConsoleStream                    stage 1: boot ISO → install
       └─ VmExitStatus                    stage 2: boot NVMe → verify
```

- **Library path**: `cargo test` — per-subsystem assertions, boot oracle, driver checks.
  Each `#[tokio::test]` gets a fresh QEMU instance. Fast, isolated, composable.
- **Tool path**: `vortex stack up` — multi-stage pipelines where disk artifacts pass
  between sequential VMs. Not expressible as a single test function.

`make test` runs only the Rust crate. The install-test stack is a separate invocation.

## Rust Test Crate

### Layout

```
tests/
  Cargo.toml              # vortex = { path = "../vortex", features = ["qemu"] }
  src/
    lib.rs                # re-exports AegisHarness, assertion API
    harness.rs            # AegisHarness::boot(preset, iso) → ConsoleOutput
    presets.rs            # aegis_pc() / aegis_q35() → QemuOpts
    assert.rs             # assertion functions
  tests/
    boot_oracle.rs        # first test: ports historical/expected/boot.txt
  historical/             # untouched — reference during development
```

### harness.rs

`AegisHarness` is the only place that touches `QemuBackend` directly. Responsibilities:

- Resolve ISO path from `$AEGIS_ISO` env var, falling back to `build/aegis.iso`
- Apply `isa-debug-exit` mappings: QEMU exit 33 = pass, 35 = fail
- Enforce boot timeout (30s default, overridable via `$AEGIS_BOOT_TIMEOUT`)
- Kill QEMU after test completes regardless of outcome
- Return `ConsoleOutput` on success, typed `HarnessError` on timeout or harness failure

QEMU lifecycle: harness waits up to `boot_timeout` for QEMU to exit on its own (via
`isa-debug-exit`). If QEMU is still running at timeout, it is killed and `ConsoleOutput`
is returned from whatever was captured — this is the normal case for most tests, which
don't use `isa-debug-exit`. Tests that do use `isa-debug-exit` get an explicit
`HarnessError::KernelFail` if the kernel signals failure.

```rust
pub struct AegisHarness;

impl AegisHarness {
    // Boot, collect all serial output, kill QEMU when done or on timeout.
    pub async fn boot(opts: QemuOpts, iso: &Path) -> Result<ConsoleOutput, HarnessError>;

    // Boot and return live stream + process handle for wait_for_line tests.
    pub async fn boot_stream(opts: QemuOpts, iso: &Path)
        -> Result<(ConsoleStream, QemuProcess), HarnessError>;
}

pub enum HarnessError {
    Timeout(Duration),             // QEMU killed after boot_timeout, no output
    KernelFail(VmExitStatus),      // isa-debug-exit: kernel explicitly signalled fail
    SpawnError(anyhow::Error),     // QEMU failed to start
}
```

### presets.rs

```rust
pub fn aegis_pc() -> QemuOpts   // -machine pc, 2G, vga std, isa-debug-exit
pub fn aegis_q35() -> QemuOpts  // -machine q35, 2G, xhci, nvme, virtio-net
```

`AEGIS_PRESET=q35` env var overrides the preset used by `iso()` helper so the same test
binary can run against q35 without recompilation.

### assert.rs

All functions except `wait_for_line` take `&ConsoleOutput` (post-boot). Failures panic
with the full captured serial output included in the message.

```rust
// Subsystem-level
pub fn assert_subsystem_ok(out: &ConsoleOutput, subsystem: &str)
pub fn assert_subsystem_fail(out: &ConsoleOutput, subsystem: &str)

// Ordered subsequence — gaps allowed, order enforced
pub fn assert_boot_subsequence(out: &ConsoleOutput, expected: &[&str])

// Arbitrary line matching
pub fn assert_line_contains(out: &ConsoleOutput, substr: &str)
pub fn assert_no_line_contains(out: &ConsoleOutput, substr: &str)

// Live stream — blocks until match or timeout
pub async fn wait_for_line(
    stream: &mut ConsoleStream,
    pattern: &str,
    timeout: Duration,
) -> Result<String, WaitTimeout>
```

No proc macros. Tests are plain `#[tokio::test]`. The only helpers a test needs are
`iso()` (resolves ISO path) and a preset function.

### Example test

```rust
#[tokio::test]
async fn pmm_and_vmm_ok() {
    let out = AegisHarness::boot(aegis_pc(), &iso()).await.unwrap();
    assert_subsystem_ok(&out, "PMM");
    assert_subsystem_ok(&out, "VMM");
}

#[tokio::test]
async fn no_net_lines_on_pc() {
    let out = AegisHarness::boot(aegis_pc(), &iso()).await.unwrap();
    assert_no_line_contains(&out, "[NET]");
}
```

## .vortex/config.toml

### Templates

```toml
default_backend = "qemu"

[templates.aegis-pc]
description = "Aegis base boot — kernel + subsystem tests"
memory = 2048
cpus = 1
boot_source = { type = "cdrom", iso = "build/aegis.iso" }

[templates.aegis-pc.qemu]
machine = "pc"
display = "none"
serial_capture = true
extra_args = ["-vga", "std", "-no-reboot",
              "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]

[[templates.aegis-pc.exit_mappings]]
raw = 33
meaning = "pass"

[[templates.aegis-pc.exit_mappings]]
raw = 35
meaning = "fail"

[templates.aegis-q35]
description = "Aegis q35 — NVMe, xHCI, virtio-net, network stack"
memory = 2048
cpus = 1
boot_source = { type = "cdrom", iso = "build/aegis.iso" }

[templates.aegis-q35.qemu]
machine = "q35"
display = "none"
serial_capture = true
devices = [
  "qemu-xhci,id=xhci", "usb-kbd,bus=xhci.0", "usb-mouse,bus=xhci.0",
  "nvme,drive=nvme0,serial=aegis0",
  "virtio-net-pci,netdev=net0",
]
drives = ["file=build/disk.img,format=raw,if=none,id=nvme0"]
extra_args = ["-netdev", "user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80",
              "-vga", "std", "-no-reboot",
              "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]

[[templates.aegis-q35.exit_mappings]]
raw = 33
meaning = "pass"

[[templates.aegis-q35.exit_mappings]]
raw = 35
meaning = "fail"
```

### Install-test stack

```toml
[stacks.aegis-install-test]
description = "Boot ISO → install to disk → reboot from NVMe → verify"

[[stacks.aegis-install-test.stages]]
name = "install"
template = "aegis-q35"
disk_artifact = { create = "512M", id = "rootdisk" }

[[stacks.aegis-install-test.stages]]
name = "verify"
template = "aegis-q35"
boot_source = { type = "disk", path = "${rootdisk}" }
```

## make targets

```makefile
test: iso
	cargo test --manifest-path tests/Cargo.toml -- --nocapture

test-q35: iso disk
	AEGIS_PRESET=q35 cargo test --manifest-path tests/Cargo.toml -- --nocapture

install-test: iso disk
	vortex stack up aegis-install-test
```

`test` depends on `iso` so the ISO is current. Nuclear clean remains a manual/CI step —
not embedded in `make test`. `AEGIS_ISO` overrides the ISO path if pre-built.

## First test: boot oracle

`tests/tests/boot_oracle.rs` ports `tests/historical/expected/boot.txt` using
`assert_boot_subsequence`. Lines in the expected file become the ordered subsequence —
gaps allowed so incidental extra output doesn't break it. This is strictly more robust
than the old exact-diff approach.

## Out of scope

- GUI framebuffer capture
- krunvm backend
- ARM guest
- Snapshot/restore
- Windows host
