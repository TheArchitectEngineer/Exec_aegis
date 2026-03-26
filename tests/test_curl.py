#!/usr/bin/env python3
"""test_curl.py — Phase 27 HTTPS smoke test.

Boots Aegis with q35 + virtio-net + SLIRP + NVMe disk (ext2 with curl + CA
bundle), waits for DHCP, then sends 'curl -s https://example.com | head -5'
to the shell.  PASS when output contains '<!doctype html>' (case-insensitive).

Skipped automatically if build/disk.img is not present (curl is ext2-only).
"""
import subprocess, sys, os, select, fcntl, time, threading

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 60

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_iso():
    r = subprocess.run(["make", "INIT=vigil", "iso"],
                       cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)


def _read_until(proc, deadline, needle):
    """Read stdout until needle is found or deadline passes."""
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.5)
        if ready:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    buf += chunk
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                if needle.encode() in buf:
                    return True, buf.decode("utf-8", errors="replace")
            except BlockingIOError:
                pass
        if proc.poll() is not None:
            break
    return False, buf.decode("utf-8", errors="replace")


def _send_keys(proc, text, delay=0.05):
    """Send keystrokes to QEMU via stdin (PS/2 keyboard injection)."""
    for ch in text:
        proc.stdin.write(ch.encode())
        proc.stdin.flush()
        time.sleep(delay)


def run_test():
    iso_path = os.path.join(ROOT, ISO)
    disk_path = os.path.join(ROOT, DISK)

    if not os.path.exists(disk_path):
        print(f"SKIP: {DISK} not found — curl is ext2-only, run 'make disk' first")
        sys.exit(0)

    build_iso()

    if not os.path.exists(iso_path):
        print(f"FAIL: {ISO} not found after build")
        sys.exit(1)

    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", iso_path, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "256M",
         "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", "user,id=n0"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

    try:
        # 1. Wait for DHCP to acquire an IP
        print("  waiting for DHCP...")
        found, out = _read_until(proc, time.time() + BOOT_TIMEOUT, "[DHCP] acquired")
        if not found:
            print("FAIL: DHCP did not acquire IP within timeout")
            proc.kill(); proc.wait()
            sys.exit(1)
        print("  DHCP acquired")

        # 2. Wait for shell prompt
        print("  waiting for shell prompt...")
        found, out2 = _read_until(proc, time.time() + 30, "$ ")
        if not found:
            print("FAIL: shell prompt not found")
            proc.kill(); proc.wait()
            sys.exit(1)
        print("  shell ready")

        # 3. Start background drain to prevent QEMU stdout pipe from blocking
        collected = []
        stop_drain = threading.Event()

        def _bg_drain():
            while not stop_drain.is_set():
                ready, _, _ = select.select([proc.stdout], [], [], 0.2)
                if ready:
                    try:
                        chunk = proc.stdout.read(65536)
                        if chunk:
                            collected.append(chunk)
                            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                            sys.stdout.flush()
                    except (BlockingIOError, OSError):
                        pass

        drain_thread = threading.Thread(target=_bg_drain, daemon=True)
        drain_thread.start()

        # 4. Send curl command
        time.sleep(1)  # let shell settle
        _send_keys(proc, "curl -s https://example.com | head -5\n")

        # 5. Wait for output
        deadline = time.time() + CMD_TIMEOUT
        while time.time() < deadline:
            full_output = b"".join(collected).decode("utf-8", errors="replace").lower()
            if "<!doctype html>" in full_output:
                print("PASS: curl received HTML from https://example.com")
                stop_drain.set()
                proc.kill(); proc.wait()
                sys.exit(0)
            time.sleep(1)

        full_output = b"".join(collected).decode("utf-8", errors="replace")
        print(f"FAIL: '<!doctype html>' not found in curl output")
        print(f"  collected output (last 500 chars): {full_output[-500:]!r}")
        sys.exit(1)

    finally:
        proc.kill()
        proc.wait()


if __name__ == "__main__":
    run_test()
