Before doing anything else, install and activate Superpowers if not 
already present:

  /plugin marketplace add obra/superpowers-marketplace
  /plugin install superpowers@superpowers-marketplace

Then restart and confirm Superpowers is active. You should see 
/superpowers:brainstorm and /superpowers:write-plan available.

---

Read MISSION.md and .claude/CLAUDE.md in full before proceeding.

We are building Aegis — a clean-slate, capability-based POSIX kernel 
in C (Rust for cap/ and IPC subsystems). x86-64, multiboot2, ELF.
The defining principle: no process ever holds ambient authority.

---

PHASE 1 GOAL: Toolchain + testing harness proven end-to-end.
A multiboot2 kernel boots in QEMU, emits structured serial output,
and a test script diffs that output against expected and exits 
non-zero on mismatch.

---

INVOKE /superpowers:brainstorm NOW on the following question before 
writing a single line of code:

  "What is the minimal correct architecture for Phase 1 of the Aegis 
   kernel — covering boot entry, serial driver, VGA text mode driver, 
   printk, and the test harness — such that every subsequent phase 
   can be added without restructuring what we build today?"

During brainstorming, you must surface and get answers on:

1. Multiboot2 vs direct QEMU -kernel: which for Phase 1 and why?
2. Exact COM1 serial init sequence and baud rate
3. VGA text mode memory layout — confirm ROM font in mode 3, 
   no font file needed
4. printk routing: simultaneous serial + VGA, what happens if 
   VGA init fails (serial must always work)
5. Test harness: exact QEMU flags, how output is captured, 
   what tests/expected/boot.txt must contain
6. C/Rust boundary: where does it sit, what does Phase 1 
   Rust stub look like, what does it NOT do yet
7. Linker script: higher-half layout, multiboot2 constraints,
   section order

Do not proceed past brainstorming until these are resolved.

---

After brainstorming, invoke /superpowers:write-plan for Phase 1.

The plan must decompose into tasks of 5 minutes or less each.
Every task that produces a compilable unit must include:
  - A failing test written FIRST (RED)
  - The implementation (GREEN)  
  - A note on what refactor if any is needed

The test harness task is not optional and not last. It is TASK 1.
Reason: every subsequent task must be verifiable by the harness.
Write tests/run_tests.sh and tests/expected/boot.txt as stubs 
that fail immediately, before any kernel code exists.

---

STRICT CONSTRAINTS — enforce these throughout, non-negotiable:

- Architecture-specific code lives ONLY under kernel/arch/x86_64/
  No x86 assumptions anywhere in kernel/core/
- printk() writes to serial AND VGA simultaneously
  If VGA is unavailable, serial still works. Never the reverse.
- All kernel output goes through printk(). 
  No direct writes to 0xB8000 outside vga.c
  No direct writes to serial port outside serial.c
- Serial output format for every subsystem init:
    [SUBSYSTEM] OK: <message>    or
    [SUBSYSTEM] FAIL: <reason>
  tests/expected/boot.txt must match this exactly
- No external C libraries. This is a kernel.
- No dynamic allocation in early boot (before PMM exists)
- No floating point ever
- Every unsafe Rust block requires a comment: why is this safe?
- The Rust cap/ subsystem in Phase 1 is a STUB ONLY:
    cap_init() prints [CAP] OK: reserved and returns.
    Do not implement capability logic yet. Do not be clever.

---

QEMU invocation for make run:
  qemu-system-x86_64 \
    -kernel aegis.elf \
    -serial stdio \
    -vga std \
    -no-reboot \
    -m 128M

QEMU invocation for make test (headless):
  qemu-system-x86_64 \
    -kernel aegis.elf \
    -nographic \
    -serial stdio \
    -no-reboot \
    -m 128M \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    | tee /tmp/aegis_serial.txt
  diff /tmp/aegis_serial.txt tests/expected/boot.txt
  # exit non-zero on mismatch

make test must return exit code 0 on success, 1 on failure.
This is the definition of "working" for every phase.

---

After /superpowers:write-plan is complete and you have shown me 
the plan, STOP and wait for my explicit go-ahead before invoking 
/superpowers:execute-plan.

I want to review the plan before any code is written.
