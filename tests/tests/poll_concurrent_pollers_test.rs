//! Two clients block in poll() on separate AF_UNIX fds; server writes a
//! byte to each. Pre-fix, only one client wakes (single g_poll_waiter
//! starvation). Post-fix, both wake within seconds.
//!
//! Requires build/aegis-test.iso (`make test-iso`) — this is the
//! text-mode ISO that drops into /bin/login on COM1, where we can drive
//! a shell directly. The graphical ISO would land in Bastion instead.
//!
//! Run: AEGIS_INSTALLER_ISO=build/aegis-test.iso \
//!      cargo test --manifest-path tests/Cargo.toml \
//!      --test poll_concurrent_pollers_test -- --nocapture

use aegis_tests::{aegis_q35_installer, wait_for_line, AegisHarness};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 32;
const BOOT_TIMEOUT_SECS: u64 = 120;
const ROOT_PW: &str = "forevervigilant";

fn installer_iso() -> PathBuf {
    let val = std::env::var("AEGIS_INSTALLER_ISO")
        .unwrap_or_else(|_| "build/aegis-test.iso".into());
    PathBuf::from(val)
}

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/poll_concurrent_pollers_disk.img")
}

fn make_fresh_disk(path: &std::path::Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let _ = std::fs::remove_file(path);
    let status = Command::new("truncate")
        .arg(format!("-s{}M", DISK_SIZE_MB))
        .arg(path)
        .status()?;
    if !status.success() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::Other,
            format!("truncate failed with status {status}"),
        ));
    }
    Ok(())
}

#[tokio::test]
async fn two_pollers_both_receive() {
    let iso = installer_iso();
    if !iso.exists() {
        eprintln!("SKIP: {} not found (run `make test-iso`)", iso.display());
        return;
    }

    let disk = disk_path();
    make_fresh_disk(&disk).expect("create fresh disk");

    let (mut stream, mut proc) =
        AegisHarness::boot_stream(aegis_q35_installer(&disk), &iso)
            .await
            .expect("boot spawn failed");

    // Anchor on the pre-login banner.
    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("pre-login banner never appeared");

    // Let /bin/login draw its `login: ` prompt.
    tokio::time::sleep(Duration::from_millis(700)).await;

    proc.send_keys("root\n").await.expect("send username");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{ROOT_PW}\n"))
        .await
        .expect("send password");
    tokio::time::sleep(Duration::from_millis(1500)).await;

    // Spawn server + two clients in the same shell line. The 1s sleep
    // after server-background gives it time to bind/listen before the
    // clients attempt to connect — `connect_to` retries on
    // ECONNREFUSED so even without it this would converge, but keep
    // it for determinism.
    proc.send_keys(
        "poll-test server /tmp/p.sock & sleep 1; \
         poll-test client /tmp/p.sock & poll-test client /tmp/p.sock & wait\n",
    )
    .await
    .expect("send command line");

    let mut got_a = false;
    let mut got_b = false;
    let deadline = tokio::time::Instant::now() + Duration::from_secs(20);
    let mut trace: Vec<String> = Vec::new();
    loop {
        match tokio::time::timeout_at(deadline, stream.next_line()).await {
            Ok(Some(line)) => {
                eprintln!("LINE: {line}");
                trace.push(line.clone());
                if line.contains("[POLL-TEST] got: A") {
                    got_a = true;
                }
                if line.contains("[POLL-TEST] got: B") {
                    got_b = true;
                }
                if got_a && got_b {
                    break;
                }
            }
            _ => break,
        }
    }

    let _ = proc.kill().await;

    if !(got_a && got_b) {
        eprintln!("--- trace ({} lines) ---", trace.len());
        for l in &trace { eprintln!("  {l}"); }
    }
    assert!(got_a, "client A never received its byte (g_poll_waiter starvation?)");
    assert!(got_b, "client B never received its byte (g_poll_waiter starvation?)");
}
