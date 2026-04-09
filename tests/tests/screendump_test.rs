// Smoke test for monitor_socket + screendump.
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml screendump -- --nocapture
//
// Machine config mirrors `make run-fb`: q35 + virtio-vga.
// -machine pc / -vga std does NOT provide a usable linear framebuffer on QEMU —
// GRUB can't set VBE mode and sys_fb_map returns -1. virtio-vga does.

use aegis_tests::{iso, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn graphical_opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:15".into(), // headless but maintains render surface
        devices: vec![
            "virtio-vga".into(),
            "qemu-xhci,id=xhci".into(),
            "usb-kbd,bus=xhci.0".into(),
        ],
        drives: vec![],
        extra_args: vec![
            "-cpu".into(), "Broadwell".into(),
            "-no-reboot".into(),
        ],
        serial_capture: true,
        monitor_socket: true,
    }
}

#[tokio::test]
async fn screendump_on_bastion_greeter() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (mut stream, mut proc) = AegisHarness::boot_stream(graphical_opts(), &iso)
        .await
        .expect("QEMU failed to start");

    // Stream serial lines, taking screenshot when greeter is ready or on timeout.
    let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
    let mut triggered = false;
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                eprintln!("SERIAL: {}", line);
                if line.contains("[BASTION] greeter ready") {
                    eprintln!("trigger: [BASTION] greeter ready");
                    triggered = true;
                    // Give one extra frame to render.
                    tokio::time::sleep(Duration::from_millis(500)).await;
                    break;
                }
            }
            Ok(None) => {
                eprintln!("WARN: serial stream closed (QEMU exited?)");
                break;
            }
            Err(_) => {
                eprintln!("WARN: timed out at 30s — screenshotting whatever is on screen");
                break;
            }
        }
    }
    let _ = triggered;

    let out = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("screenshots/bastion-greeter.ppm");
    proc.screendump(&out).await.expect("screendump failed");
    proc.kill().await.unwrap();

    assert!(out.exists(), "screenshot file not created at {}", out.display());
    assert!(out.metadata().unwrap().len() > 0, "screenshot file is empty");
    eprintln!("screenshot: {} ({} bytes)", out.display(), out.metadata().unwrap().len());
}
