# Aegis Phase 25 Context — Orchestrator Q&A

*Generated: 2026-03-23*

---

## 1. PCI Infrastructure

PCI config space enumeration and MMIO BAR mapping are fully implemented as of Phase 19.

**Files:** `kernel/arch/x86_64/pcie.c`, `kernel/arch/x86_64/pcie.h`

What exists:
- ACPI MCFG parsing (`kernel/arch/x86_64/acpi.c`) provides the ECAM base address and bus range.
- `pcie_init()` maps ECAM MMIO into kernel VA via `kva_alloc_pages` + `vmm_map_page` with PWT+PCD (no-cache) flags. Capped at 8 buses (`PCIE_MAX_SCAN_BUSES`), which equals 8MB of MMIO space.
- `pcie_read8/16/32()` and `pcie_write32()` provide raw config space access. The `config_addr()` function computes the ECAM offset as `(bus<<20)|(dev<<15)|(fn<<12)|off`.
- `decode_bar()` in `pcie.c` (lines 64–88) handles both 32-bit and 64-bit MMIO BARs. I/O BARs are skipped. All six BARs are decoded and stored as `uint64_t` in `pcie_device_t.bar[6]`.
- `pcie_find_device(class, subclass, progif)` and `pcie_get_devices()` expose the enumerated device table (max 64 entries, `PCIE_MAX_DEVICES`).
- On `-machine pc` (no MCFG), `pcie_init()` prints `[PCIE] OK: skipped (no ECAM)` and returns zero devices. All drivers must handle `NULL` from `pcie_find_device()`.

**Phase 26 does not need to land PCI infrastructure before e1000.** It is already present. The only gap relevant to e1000: Bus Master Enable (PCI command register bit 2) is not explicitly set by the kernel — QEMU SeaBIOS sets it at boot, so QEMU testing works, but bare-metal UEFI may not. See Phase 24 forward-looking constraint in CLAUDE.md.

---

## 2. Phases 17–21 — What Was Built

All five phases are complete and in the build status table. The worktree is named `phase17-signals` but all phases through 25 (in-progress) are present.

### Phase 17 — Signal subsystem
- **Files:** `kernel/signal/signal.c`, `kernel/signal/signal.h`, `kernel/syscall/sys_signal.c`
- Signal delivery via both iretq path (`signal_deliver()`, called from `isr.asm`) and sysret path (`signal_deliver_sysret()`, called from `syscall_entry.asm`).
- `rt_sigframe_t` (560 bytes) placed on the user stack; matches Linux x86-64 ABI exactly.
- `signal_send_pid()` safe from ISR context (no allocation). Used by `kbd_handler` for Ctrl-C → SIGINT.
- Syscalls added: `sys_rt_sigaction` (13), `sys_rt_sigprocmask` (14), `sys_rt_sigreturn` (15), `sys_kill` (62), `sys_setfg` (360, Aegis-private for foreground PID).
- SIGPIPE delivery implemented in security audit post-Phase 22: `pipe_write_fn` calls `signal_send_pid(pid, SIGPIPE)`.

### Phase 18 — stat / getdents64 / utility programs
- Syscalls added: `sys_stat` (4), `sys_fstat` (5), `sys_lstat` (6 aliased to stat), `sys_lseek` (8), `sys_access` (21), `sys_nanosleep` (35), `sys_getdents64` (217).
- `syscall_entry.asm` gained rdi/rsi/rdx save/restore across `syscall_dispatch` (Linux ABI correctness fix required by musl's `readdir` using rsi after `getdents64`).
- New user programs: `wc`, `grep`, `sort`.

### Phase 19 — PCIe enumeration + ACPI
- `kernel/arch/x86_64/acpi.c`: RSDP scan, MCFG + MADT parsing; exports `g_mcfg_base`, `g_mcfg_start_bus`, `g_mcfg_end_bus`.
- `kernel/arch/x86_64/pcie.c`: ECAM enumeration (described in section 1 above).
- Graceful skip on `-machine pc` (no MCFG).

### Phase 20 — NVMe driver + blkdev abstraction
- **Files:** `kernel/drivers/nvme.c`, `kernel/fs/blkdev.c`, `kernel/fs/blkdev.h`
- NVMe 1.4 driver: admin queue (64 slots), I/O queue (64 slots), synchronous doorbell+poll.
- `blkdev_t` abstraction: `read/write` callbacks, `block_count`, `name[16]`.
- `nvme_init()` registers `"nvme0"` as a blkdev; `gpt_scan("nvme0")` registers `"nvme0p1"` and `"nvme0p2"`.
- Transfer capped at 4096 bytes (one PRP entry). No MSI/MSI-X.

### Phase 21 — ext2 read-write filesystem
- **Files:** `kernel/fs/ext2.c`, `kernel/fs/ext2_cache.c`, `kernel/fs/ext2_dir.c`
- Mounts from `"nvme0p1"`. Supports read/write/create/unlink/mkdir/rename.
- 16-slot LRU block cache. No double/triple indirect blocks. No timestamps.
- New user programs: `mkdir`, `touch`, `rm`, `cp`, `mv`.
- Syscalls added: `sys_rename` (82), `sys_mkdir` (83), `sys_unlink` (87).

---

## 3. PIT Tick Handler Contents

**File:** `kernel/arch/x86_64/pit.c`, `pit_handler()` (line 47)

At 100 Hz, each tick executes in order:
1. `s_ticks++` — increment tick counter.
2. `sched_tick()` — preemptive round-robin scheduler; may context-switch.
3. `xhci_poll()` — poll xHCI event ring for USB HID keyboard reports (no-op if no xHCI device).
4. `netdev_poll_all()` — call `dev->poll(dev)` for each registered netdev (currently virtio-net eth0). Delivers received frames via `netdev_rx_deliver()` → `eth_rx()`.
5. `tcp_tick()` — TCP retransmit timer; walks the 32-entry connection table, handles `TCP_RTO_*` and `TCP_TIMEWAIT_TICKS`.
6. Shutdown check — if `s_shutdown` is set, calls `arch_debug_exit(0x01)`.

**NIC RX is polled, not interrupt-driven.** `virtio_net_init()` does not configure MSI-X. All RX happens in step 4 above at 100 Hz. No MSI or MSI-X support exists in the codebase.

---

## 4. Network Stack Scope (Phases 24/25)

**Files:** `kernel/net/eth.c`, `kernel/net/ip.c`, `kernel/net/udp.c`, `kernel/net/tcp.c`

Layer coverage:
- **Ethernet (Layer 2):** `eth.h`/`eth.c` — frame parsing, ARP request/reply, ARP cache. `eth_rx()` dispatches by EtherType (0x0806 ARP, 0x0800 IPv4).
- **IP (Layer 3):** `ip.h`/`ip.c` — IPv4 header parsing, checksum, ICMP echo request/reply, `ip_send()`, `ip_rx()` dispatching to TCP/UDP/ICMP handlers. Static IP configuration via `net_set_config()`.
- **UDP (Layer 4):** `udp.h`/`udp.c` — UDP header; DHCP-capable framing. No socket API yet.
- **TCP (Layer 4):** `tcp.h`/`tcp.c` — full TCP state machine (CLOSED through TIME_WAIT, all 11 states). 32-connection table (`TCP_MAX_CONNS`). 8KB RX ring buffer + 8KB TX send buffer per connection. Retransmit with exponential backoff (initial 1s, max 8s, 3 retransmits then RST). TIME_WAIT timeout (4s shortened 2MSL).

**NIC/stack boundary:** `netdev_rx_deliver()` in `kernel/net/netdev.c` (line 47) calls `eth_rx(dev, frame, len)`. This is the exact crossing point: below is the driver (virtio-net polling raw frames), above is `eth_rx()` which owns EtherType dispatch and all protocol logic. TX boundary: `eth_send()` calls `dev->send(dev, pkt, len)` on the registered `netdev_t`.

**Phase 25 status:** `make test` is GREEN (boot oracle unaffected), but `test_net_stack.py` is NOT yet passing. Two open bugs:
1. `%d` in `net_set_config` printk in `kernel/net/ip.c` (must be `%u`; `printk` does not support `%d`).
2. ICMP echo reply from SLIRP is not received during the 1000-tick poll in `net_init()`.

There is no BSD socket API (`sys_socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `epoll`) — that is Phase 26 work.

---

## 5. PMM Capabilities

**File:** `kernel/mm/pmm.c`

Still single-page only. `pmm_alloc_page()` (line 133) returns one 4KB page via linear bitmap scan. `pmm_free_page()` frees one 4KB page. There is no `pmm_alloc_pages(n)` multi-page API and no buddy allocator.

The CLAUDE.md build status table entry reads: "Bitmap allocator; single-page (4KB) only; multi-page deferred to buddy allocator."

**Implication for e1000:** The e1000 descriptor ring requires physically contiguous pages. With the current PMM, contiguous allocation is only possible by calling `pmm_alloc_page()` repeatedly and hoping physical pages are consecutive — which they will be in a fresh QEMU boot with little fragmentation, but is not guaranteed. Phase 26 should implement `pmm_alloc_contig(n)` (linear scan for n consecutive free bits in the bitmap) before the e1000 descriptor ring allocation.

---

## 6. KVA Bump Allocator

**File:** `kernel/mm/kva.c`

The KVA allocator remains a pure bump allocator as of Phase 25. `kva_alloc_pages(n)` advances `s_kva_next` by `n * 4096` with no way to reclaim VA. `kva_free_pages(va, n)` unmaps and frees the underlying PMM pages but does NOT return the VA range to a free list — the VA is permanently consumed.

From CLAUDE.md Phase 16 forward-looking constraint: "No `sys_munmap` VA reclaim for pipe_t. `kva_free_pages` is called on pipe close, but the kva VA range is not returned to a free list (bump allocator). Negligible at Phase 16 scale."

This has not been addressed. Network buffer allocation (Phase 24 RX/TX rings) uses `kva_alloc_pages` and those pages are never freed. At Phase 25 scale (256 RX + 256 TX pages = 2MB of VA permanently allocated at virtio-net init) this is negligible. For Phase 26 with a socket API and potentially many sockets, a freelist or slab over the KVA range will be needed if sockets allocate KVA-backed buffers.

---

## 7. Open Deferred Constraints — Current Status

### Partition LBA bounds not enforced in `gpt_part_read`/`gpt_part_write`
**Status: Still open.** Confirmed by reading `kernel/fs/gpt.c` lines 81–92. `gpt_part_read` and `gpt_part_write` add `lba_offset` to the caller's LBA without any clamping against `block_count`. A call with `lba + lba_offset > partition_end_lba` will silently read/write past the partition boundary. The CLAUDE.md Phase 23 constraint accurately describes this: "Add bounds checking when the blkdev interface gains a per-device bounds hook."

### Backup GPT header never verified
**Status: Still open.** `gpt_scan()` in `kernel/fs/gpt.c` (lines 149–158) reads the backup header only as a fallback when the primary is invalid — it does not cross-check both headers for consistency when the primary is valid. CLAUDE.md Phase 23 constraint: "deferred to a future hardening phase."

### USB transfer ring memory never freed on disconnect
**Status: Still open.** Per CLAUDE.md Phase 22 constraint: "Transfer ring memory is never freed." xHCI does not implement hot-remove or device disconnect handling.

### `nvme0p2` registered but unmounted
**Status: Still open.** `gpt_scan("nvme0")` registers `"nvme0p2"` as a blkdev, but no filesystem driver calls `blkdev_get("nvme0p2")`. It exists as a recognized partition with no consumer. CLAUDE.md Phase 23 constraint: "A future phase can format and mount it as swap or a second ext2 volume."

---

## 8. QEMU Machine Type

**Split behavior: `-machine pc` for `make test`, `-machine q35` for `make run`.**

`tests/run_tests.sh` (line 26):
```
timeout 10s qemu-system-x86_64 \
    -machine pc \
    -cpu Broadwell \
    ...
```

`Makefile` `run` target (line 375):
```
qemu-system-x86_64 \
    -machine q35 \
    -cdrom $(BUILD)/aegis.iso -boot order=d \
    -serial stdio -vga std -no-reboot -m 128M \
    $(NVME_FLAGS) \
    -device qemu-xhci -device usb-kbd \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

The Python test scripts (`test_nvme.py`, `test_xhci.py`, `test_gpt.py`, `test_virtio_net.py`, `test_net_stack.py`) each spawn their own QEMU instances with `-machine q35` and the appropriate devices.

**`-machine pc` is canonical only for the boot oracle test** (the `diff` against `tests/expected/boot.txt`). This machine type has no ECAM, so `[PCIE] OK: skipped (no ECAM)` is the expected output. PCIe/NVMe/xHCI/net subsystems are tested separately by the Python scripts on `q35`.

**e1000 works on both machine types.** However, since virtio-net is already the CI-tested NIC on q35, any e1000 driver test should also use q35 for consistency with the existing test infrastructure and to coexist with NVMe and xHCI devices.
