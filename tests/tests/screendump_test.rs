// Smoke test for monitor_socket + screendump.
// Run: cargo test --manifest-path tests/Cargo.toml screendump -- --nocapture

use aegis_tests::{aegis_pc, iso, wait_for_line, AegisHarness};
use std::time::Duration;

#[tokio::test]
async fn screendump_on_sched_ready() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let mut opts = aegis_pc();
    opts.monitor_socket = true;
    // VNC backend renders the VGA surface headlessly; screendump needs it.
    // display = "none" skips pixel rendering, producing a black screendump.
    opts.display = "vnc=127.0.0.1:15".into();

    let (mut stream, mut proc) = AegisHarness::boot_stream(opts, &iso)
        .await
        .expect("QEMU failed to start");

    // Wait for scheduler — reliable late-boot signal available on q35.
    wait_for_line(&mut stream, "[SCHED] OK", Duration::from_secs(30))
        .await
        .expect("timed out waiting for [SCHED] OK");

    // Use absolute path — QEMU resolves the screendump path itself.
    let out = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("screenshots/sched-ready.ppm");
    eprintln!("writing screenshot to {}", out.display());
    proc.screendump(&out).await.expect("screendump failed");

    proc.kill().await.unwrap();

    assert!(out.exists(), "screenshot file not created at {}", out.display());
    assert!(out.metadata().unwrap().len() > 0, "screenshot file is empty");
    eprintln!("screenshot written ({} bytes)", out.metadata().unwrap().len());
}
