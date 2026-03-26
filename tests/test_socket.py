#!/usr/bin/env python3
"""test_socket.py — Phase 26 socket API integration test.
Boots Aegis with q35 + virtio-net + NVMe, uses static IP 10.0.2.15 (Phase 25),
launches httpd via vigil service, verifies HTTP response via host port forward."""

import subprocess, time, http.client, sys, os, select, fcntl

QEMU         = "qemu-system-x86_64"
ISO          = "build/aegis.iso"
DISK         = "build/disk.img"
BOOT_TIMEOUT = 120
HOST_PORT    = 18080

def _read_until(proc, deadline, needle):
    buf = b""
    while time.time() < deadline:
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if ready:
            try:
                chunk = proc.stdout.read(4096)
                if chunk:
                    buf += chunk
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                if needle.encode() in buf:
                    return buf.decode("utf-8", errors="replace")
            except BlockingIOError:
                pass
        if proc.poll() is not None:
            break
    return buf.decode("utf-8", errors="replace")

def build_iso():
    r = subprocess.run("make INIT=vigil iso", shell=True, capture_output=True)
    if r.returncode != 0:
        print("[FAIL] make INIT=vigil iso failed")
        print(r.stderr.decode())
        sys.exit(1)

def run_test():
    if not os.path.exists(DISK):
        print(f"SKIP: {DISK} not found — run 'make disk' first")
        sys.exit(0)

    build_iso()

    proc = subprocess.Popen(
        [QEMU,
         "-machine", "q35", "-cpu", "Broadwell",
         "-cdrom", ISO, "-boot", "order=d",
         "-display", "none", "-vga", "std", "-nodefaults",
         "-serial", "stdio", "-no-reboot", "-m", "128M",
         "-drive", f"file={DISK},if=none,id=nvme0",
         "-device", "nvme,drive=nvme0,serial=aegis0",
         "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
         "-netdev", f"user,id=n0,hostfwd=tcp::{HOST_PORT}-:80"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
    fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

    try:
        # Wait for vigil init capabilities to confirm kernel is up
        print("  waiting for vigil init caps...")
        out = _read_until(proc, time.time() + BOOT_TIMEOUT, "[CAP] OK: 8 capabilities")
        if "[CAP] OK: 8 capabilities" not in out:
            print("FAIL: vigil did not receive init capabilities within timeout")
            proc.kill(); proc.wait()
            sys.exit(1)
        print("  vigil started as init (caps granted)")

        # Drain more serial output for 3 seconds to let vigil start services
        print("  draining serial output (3s) for httpd startup...")
        out2 = _read_until(proc, time.time() + 3, "\x00\x00\x00NEVER")  # never matches
        sys.stdout.write(out2)
        sys.stdout.flush()

        # Try HTTP request via forwarded port
        for attempt in range(15):
            try:
                conn = http.client.HTTPConnection("localhost", HOST_PORT, timeout=3)
                conn.request("GET", "/")
                resp = conn.getresponse()
                body = resp.read().decode()
                conn.close()
                if resp.status == 200 and "Aegis" in body:
                    print(f"PASS: HTTP {resp.status}, body: {body.strip()!r}")
                    proc.kill(); proc.wait()
                    sys.exit(0)
                print(f"  Attempt {attempt+1}: status={resp.status} body={body.strip()!r}")
            except Exception as e:
                print(f"  Attempt {attempt+1}: {e}")
            time.sleep(1)

        print("FAIL: no valid HTTP response after 15 attempts")
        sys.exit(1)

    finally:
        proc.kill()
        proc.wait()

if __name__ == "__main__":
    run_test()
