# Vigil — Aegis Init System

Vigil is the PID 1 init system for Aegis. It supervises services defined
in `/etc/vigil/services/<name>/` and handles graceful shutdown.

## Service Declaration

Each service lives in a directory under `/etc/vigil/services/`:

    /etc/vigil/services/<name>/
        run       — command string passed to /bin/sh -c
        policy    — respawn | oneshot; optional: max_restarts=N (default 5)
        caps      — space-separated capability names (future use)
        user      — username to run as (root only for now)

## IPC

vigictl communicates with vigil via:
1. Write command to `/run/vigil.cmd`
2. Send SIGUSR1 to vigil (PID from `/run/vigil.pid`)

Commands: `status`, `start <svc>`, `stop <svc>`, `restart <svc>`, `shutdown`.

AF_UNIX socket IPC is deferred to Phase 26 (socket API).

## Logging

Vigil logs to stdout with format: `vigil: <message>`

The `vigil:` prefix is intentional — `[VIGIL]` would contaminate the
boot oracle (grep `^[` filter in run_tests.sh keeps only lines starting
with `[`).

## Capability Delegation

Services requiring elevated capabilities use sys_cap_grant_exec (361):
vigil reads the `caps` file and calls sys_cap_grant_exec before fork/exec.
The exec_caps[] array in the child PCB is applied by sys_execve and cleared.

## Syscalls Added

| Number | Name | Purpose |
|--------|------|---------|
| 162 | sys_sync | Flush ext2 block cache |
| 228 | sys_clock_gettime | POSIX timespec from 100 Hz PIT ticks |
| 361 | sys_cap_grant_exec | Pre-register capabilities to grant across execve |

## Constraints

- Max 16 services (VIGIL_MAX_SERVICES)
- No AF_UNIX (Phase 26)
- No cgroup isolation (future)
- No dependency ordering (services start in readdir order)
- CLOCK_REALTIME = CLOCK_MONOTONIC (RTC not implemented; deferred)
- PID 1 exit triggers arch_request_shutdown (QEMU isa-debug-exit)

## Testing

`python3 tests/test_vigil.py` — boots INIT=vigil on q35+NVMe, verifies
caps are granted and getty/login service starts. Skipped if build/disk.img
is absent.
