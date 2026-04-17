// coreutils_test.rs — scaffold for coreutils-1.0.3 per-utility tests.
//
// Boots the text-mode aegis-test.iso, logs in as root, and waits for
// stsh to emit `[STSH] ready` (a one-shot marker printed once the REPL
// is about to draw its first prompt). After that, future tasks add
// per-utility assertions (head, tail, env, ...) using `assert_cmd`.
//
// Why text-mode (aegis-test.iso) and not aegis-installer-test.iso:
// the installer-test ISO boots into Bastion → Lumen, where stsh only
// runs inside an opened terminal window. Text-mode puts stsh directly
// on the console, which is what coreutils tests need to drive.
//
// Run: AEGIS_TEST_ISO=build/aegis-test.iso \
//      cargo test --manifest-path tests/Cargo.toml \
//                 --test coreutils_test -- --nocapture

use aegis_tests::{aegis_q35_installer, wait_for_line, AegisHarness};
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

const DISK_SIZE_MB: u64 = 32;
const BOOT_TIMEOUT_SECS: u64 = 120;
const ROOT_PW: &str = "forevervigilant";
const STSH_READY_MARKER: &str = "[STSH] ready";

fn test_iso() -> PathBuf {
    let val = std::env::var("AEGIS_TEST_ISO")
        .unwrap_or_else(|_| "build/aegis-test.iso".into());
    PathBuf::from(val)
}

fn disk_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/coreutils_disk.img")
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
async fn coreutils_scaffold_boots_to_stsh_ready() {
    let iso = test_iso();
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

    // Anchor on the pre-login banner to know /bin/login has drawn the prompt.
    wait_for_line(&mut stream, "WARNING: This system",
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .expect("pre-login banner never appeared");

    // Let /bin/login draw its `login:` prompt before we type.
    tokio::time::sleep(Duration::from_millis(700)).await;
    proc.send_keys("root\n").await.expect("send username");
    tokio::time::sleep(Duration::from_millis(500)).await;
    proc.send_keys(&format!("{ROOT_PW}\n")).await.expect("send password");

    // Wait for stsh to come up. The marker is emitted once on REPL entry.
    wait_for_line(&mut stream, STSH_READY_MARKER,
                  Duration::from_secs(BOOT_TIMEOUT_SECS))
        .await
        .unwrap_or_else(|e| panic!(
            "stsh ready marker '{STSH_READY_MARKER}' never appeared: {e:?}"));

    // Per-utility assertions live below, added by Tasks 4–23.
    // Helper signature suggested:
    //
    //   assert_cmd(&mut proc, &mut stream, "head -2 /etc/hostname",
    //              &["aegis"]).await;
    //
    // It would send the command, then read until the next prompt and
    // assert each `expected` line appears in the captured slice.

    let _ = proc.kill().await;
}
