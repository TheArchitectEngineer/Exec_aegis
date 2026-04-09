// Smoke test for monitor_socket + screendump.
// Run: AEGIS_ISO=build/aegis.iso cargo test --manifest-path tests/Cargo.toml screendump -- --nocapture
//
// NOTE: screenshot trigger is [BASTION] greeter ready, emitted from
// draw_form() after the first blit_to_fb(). This requires:
//   1. boot=graphical in kernel cmdline (GRUB default entry)
//   2. GRUB gfxmode=800x600x32 so the kernel gets a 32bpp VBE framebuffer
//      (current ISO has gfxmode=auto which may fall back to text mode on pc)
//   3. display = vnc=... (not none) so QEMU maintains a rendered surface
//
// Until a new ISO is built with gfxmode=800x600x32, this test is a
// functional check that the monitor socket and screendump plumbing work;
// the captured image will be black if sys_fb_map fails in the guest.

use aegis_tests::{aegis_pc, iso, wait_for_line, AegisHarness};
use std::time::Duration;

#[tokio::test]
async fn screendump_on_bastion_greeter() {
    let iso = iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found", iso.display());
        return;
    }

    let mut opts = aegis_pc();
    opts.monitor_socket = true;
    // VNC backend renders the VGA surface headlessly; -display none skips it.
    opts.display = "vnc=127.0.0.1:15".into();

    let (mut stream, mut proc) = AegisHarness::boot_stream(opts, &iso)
        .await
        .expect("QEMU failed to start");

    // [BASTION] greeter ready fires from draw_form() after the first blit_to_fb().
    // Falls back to timing out gracefully if the ISO predates this log line.
    let triggered = wait_for_line(&mut stream, "[BASTION] greeter ready", Duration::from_secs(30))
        .await
        .is_ok();

    if !triggered {
        eprintln!("WARN: [BASTION] greeter ready not seen (old ISO or sys_fb_map failed)");
    }

    let out = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("screenshots/bastion-greeter.ppm");
    proc.screendump(&out).await.expect("screendump failed");
    proc.kill().await.unwrap();

    assert!(out.exists(), "screenshot file not created");
    assert!(out.metadata().unwrap().len() > 0, "screenshot file is empty");
    eprintln!("screenshot written to {} ({} bytes)", out.display(), out.metadata().unwrap().len());
}
