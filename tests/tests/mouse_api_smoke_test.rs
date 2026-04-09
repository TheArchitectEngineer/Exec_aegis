// Smoke test: verify vortex's new mouse_move / mouse_button methods
// round-trip successfully against a real QEMU instance. This test
// doesn't verify that the mouse event reached the guest — just that
// the HMP command was accepted without error. The dock_click_test
// covers end-to-end delivery.

use aegis_tests::{iso, AegisHarness};
use std::time::Duration;
use vortex::core::config::QemuOpts;

fn opts() -> QemuOpts {
    QemuOpts {
        machine: "q35".into(),
        display: "vnc=127.0.0.1:16".into(),
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
async fn mouse_move_and_button_round_trip() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let (_stream, mut proc) = AegisHarness::boot_stream(opts(), &iso)
        .await
        .expect("QEMU failed to start");

    // Give QEMU a moment to fully initialize before hitting the monitor.
    tokio::time::sleep(Duration::from_millis(500)).await;

    proc.mouse_move(10, 10).await.expect("mouse_move failed");
    proc.mouse_button(0).await.expect("mouse_button(0) failed");

    proc.kill().await.unwrap();
}
