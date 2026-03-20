# Phase 5 — User Space Foundation Design

## Goal

Run a statically-linked ELF64 binary in ring 3 under a preemptive scheduler. The binary loops three times printing a message, then calls `exit`. Verified by `make test` diffing serial output against `tests/expected/boot.txt`.

## Scope

Phase 5 delivers the minimum to prove the kernel/user privilege boundary:

- Ring 3 GDT descriptors + TSS + SYSCALL/SYSRET MSRs
- Per-process page tables (one PML4 per process, kernel high entries shared)
- `aegis_process_t` embedding `aegis_task_t` at offset 0
- ELF64 loader for static executables
- Two syscalls: `write` (1) and `exit` (60), Linux-compatible numbers
- A user binary embedded as a C byte array in the kernel image
- `sched_exit()` for task self-removal

**Out of scope for Phase 5 (deferred to Phase 6):**
- Real capability tokens and `cap/` implementation (CLAUDE.md: "do not implement capability logic until the syscall layer is solid")
- Mapped-window allocator / identity map teardown (all Phase 5 allocations stay within the 4MB identity window)
- Process memory cleanup on exit (page tables, stacks leaked — acceptable for a single-shot test binary)
- Multiple user processes or `fork`/`exec`
- `read` syscall, keyboard input from user space

---

## Architecture

### New Directories

```
kernel/arch/x86_64/    — gdt.c/h, tss.c/h, syscall_entry.asm (new files)
kernel/proc/           — proc.h, proc.c
kernel/elf/            — elf.h, elf.c
kernel/syscall/        — syscall.h, syscall.c
user/init/             — main.c, start.asm (or inline _start), Makefile
```

### Modified Files

```
kernel/arch/x86_64/arch.h  — add arch_gdt_init, arch_tss_init,
                               arch_syscall_init, arch_set_kernel_stack
kernel/sched/sched.h       — add kernel_stack_top to aegis_task_t; sched_exit()
kernel/sched/sched.c       — call arch_set_kernel_stack before ctx_switch;
                               add sched_exit(); update sched_spawn
kernel/mm/vmm.h            — add vmm_create_user_pml4, vmm_map_user_page,
                               vmm_switch_to
kernel/mm/vmm.c            — implement above
kernel/core/main.c         — add gdt/tss/syscall init; proc_spawn(init_elf)
Makefile                   — new source dirs; user binary build + embed step
tests/expected/boot.txt    — add GDT/TSS/SYSCALL/USER lines
```

---

## Section 1: GDT / TSS / SYSCALL MSRs

### Runtime GDT (`kernel/arch/x86_64/gdt.c`)

The boot.asm GDT (null + kernel code + kernel data) is replaced at runtime.
`arch_gdt_init()` builds a new static GDT, installs it with `lgdt`, reloads
segment registers, and calls `ltr` to load the TSS selector.

GDT layout:

| Index | Selector | Descriptor     | DPL |
|-------|----------|----------------|-----|
| 0     | `0x00`   | null           | —   |
| 1     | `0x08`   | kernel code L=1| 0   |
| 2     | `0x10`   | kernel data    | 0   |
| 3     | `0x18`   | user data      | 3   |
| 4     | `0x20`   | user code L=1  | 3   |
| 5+6   | `0x28`   | TSS (16 bytes) | 0   |

SYSCALL/SYSRET selector constraints (from Intel SDM):
- `STAR[47:32] = 0x08` → SYSCALL: CS=0x08, SS=0x10 (kernel code/data) ✓
- `STAR[63:48] = 0x10` → SYSRET: SS=(0x10+8)|3=0x1B (user data), CS=(0x10+16)|3=0x23 (user code) ✓

User selectors used in IRETQ frames: CS=0x23, SS=0x1B.

### TSS (`kernel/arch/x86_64/tss.c`)

Static 104-byte `aegis_tss_t` struct. Only RSP0 is used (set by `arch_set_kernel_stack`).
IOMAP_BASE set to 104 (disables I/O permission bitmap). IST entries zeroed.

```c
void arch_set_kernel_stack(uint64_t rsp0);  /* declared in arch.h */
```

Also maintains `g_kernel_rsp` global (used by SYSCALL entry stub for stack switch).
Both `tss.rsp0` and `g_kernel_rsp` are updated together.

### SYSCALL Entry (`kernel/arch/x86_64/syscall_entry.asm`)

MSR programming in `arch_syscall_init()`:
- `IA32_EFER (0xC0000080)` bit 0 (SCE) set
- `IA32_STAR (0xC0000081)` = `(0x10ULL << 48) | (0x08ULL << 32)`
- `IA32_LSTAR (0xC0000082)` = address of `syscall_entry`
- `IA32_SFMASK (0xC0000084)` = `0x300` (clears IF and TF on entry)

`syscall_entry` stub:
1. CPU has saved RCX=return RIP, R11=RFLAGS; RSP is still user RSP
2. Save user RSP to `g_user_rsp` global
3. Load kernel RSP from `g_kernel_rsp`
4. Push user RSP, RCX (return RIP), R11 (RFLAGS) onto kernel stack
5. Move RAX (syscall number) into RDI; RDI/RSI/RDX already hold args 1-3
6. Call `syscall_dispatch(num, arg1, arg2, arg3)`
7. Pop R11, RCX, RSP (user stack restored via `pop rsp`)
8. `sysretq`

`g_user_rsp` and `g_kernel_rsp` are BSS globals (single-core; no per-CPU needed in Phase 5).

### Kernel_main init order

```c
arch_gdt_init();    /* [GDT] OK: ring 3 descriptors installed */
arch_tss_init();    /* [TSS] OK: RSP0 initialized             */
arch_syscall_init();/* [SYSCALL] OK: SYSCALL/SYSRET enabled   */
```

These three calls happen after `kbd_init()` and before `sched_init()`.

---

## Section 2: Address Space & Process Type

### VMM Extensions (`kernel/mm/vmm.c`)

```c
/* Allocate a new PML4; copy kernel high entries [256..511] from master.
 * Returns physical address. Valid while identity map is active. */
uint64_t vmm_create_user_pml4(void);

/* Map a single page in the given PML4 (not the active kernel PML4).
 * flags: VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE as needed. */
void vmm_map_user_page(uint64_t pml4_phys, uint64_t virt,
                       uint64_t phys, uint64_t flags);

/* Load pml4_phys into CR3. Flushes TLB. */
void vmm_switch_to(uint64_t pml4_phys);
```

`vmm_create_user_pml4` copies the master PML4's high 256 entries so the kernel
higher-half is accessible in every user process's address space — required for
syscall handlers to execute after SYSCALL (no CR3 switch on syscall entry).

User virtual memory layout (per process):
- `0x400000` — ELF PT_LOAD segments mapped here
- `0x7FFFFFF000` — user stack top (one 4KB page for Phase 5)

### `aegis_task_t` Addition

One field added to the existing struct in `kernel/sched/sched.h`:

```c
typedef struct aegis_task_t {
    uint64_t             rsp;               /* MUST be first */
    uint8_t             *stack_base;
    uint64_t             kernel_stack_top;  /* NEW: used to update TSS.RSP0 */
    uint32_t             tid;
    struct aegis_task_t *next;
} aegis_task_t;
```

`sched_spawn` sets `kernel_stack_top = (uint64_t)(stack + STACK_SIZE)`.

### `aegis_process_t` (`kernel/proc/proc.h`)

```c
typedef struct {
    aegis_task_t  task;          /* MUST be first — scheduler casts to this */
    uint64_t      pml4_phys;     /* 0 for kernel tasks */
} aegis_process_t;
```

`kernel_stack_top` lives in `task` (inherited). `pml4_phys == 0` distinguishes
kernel tasks from user processes for CR3 switch logic.

### Scheduler Integration

In `sched_tick()` and `sched_start()`, before `ctx_switch`:

```c
arch_set_kernel_stack(s_current->kernel_stack_top);
if (((aegis_process_t *)s_current)->pml4_phys)
    vmm_switch_to(((aegis_process_t *)s_current)->pml4_phys);
```

The cast is safe because `aegis_process_t` embeds `aegis_task_t` at offset 0.
For kernel tasks `pml4_phys` would be garbage — the `pml4_phys == 0` guard
prevents a CR3 load for kernel tasks. (Kernel tasks allocated via `sched_spawn`
never have a `pml4_phys` field; the cast reads beyond the struct. To avoid UB,
`sched.c` should call `arch_set_kernel_stack` unconditionally but CR3 switch
only when the task was registered as a user process via a flag in `aegis_task_t`.)

**Revised approach** — add `uint8_t is_user` to `aegis_task_t` (1 byte, no
struct layout impact). `sched_spawn` sets it 0; `proc_spawn` sets it 1.
`sched_tick` checks `is_user` before calling `vmm_switch_to`. This avoids the
UB cast and keeps `sched.c` free of `aegis_process_t` knowledge.

### First-Time Ring 3 Entry

`proc_spawn` constructs the user process's kernel stack to look like it was
interrupted while in ring 3. From RSP upward (low to high address):

```
[r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0]  ← ctx_switch pops these
[proc_enter_user]                             ← ctx_switch's ret lands here
[user_entry_rip]                              ← iretq frame: RIP
[0x23]                                        ← iretq frame: CS (user code)
[0x202]                                       ← iretq frame: RFLAGS (IF=1)
[user_stack_top]                              ← iretq frame: RSP
[0x1B]                                        ← iretq frame: SS (user data)
```

`proc_enter_user` is a 1-instruction asm function: `iretq`.

After the first preemption, the normal interrupt frame path takes over and
subsequent context switches use the standard isr_common_stub→iretq path.

---

## Section 3: ELF Loader (`kernel/elf/elf.c`)

```c
/* Load a static ELF64 into pml4_phys. Returns entry virtual address,
 * or 0 on parse error. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len);
```

Algorithm:
1. Check magic, class (ELFCLASS64), machine (EM_X86_64=0x3E), type (ET_EXEC)
2. For each program header where `p_type == PT_LOAD`:
   a. Allocate `ceil(p_memsz / 4096)` pages from PMM
   b. Copy `p_filesz` bytes from `data + p_offset` into the physical pages
   c. Zero bytes `[p_filesz .. p_memsz)` (BSS)
   d. Map `[p_vaddr .. p_vaddr + p_memsz)` → allocated pages in `pml4_phys`
      with flags `USER | PRESENT | (WRITABLE if p_flags & PF_W)`
3. Return `e_entry`

No relocations. No dynamic segments. No `.interp`. Error on any unrecognised
required segment.

---

## Section 4: Syscall Dispatch (`kernel/syscall/syscall.c`)

```c
/* Called from syscall_entry.asm with IF=0, on kernel stack.
 * Returns value placed in RAX for userland. */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3);
```

Handlers:

**`write` (1)**: `arg1`=fd (ignored), `arg2`=user virtual address of buffer,
`arg3`=length. Copies bytes from the user address (valid because user PML4
shares kernel higher-half — the buffer is in user space which is readable from
ring 0 in Phase 5, no SMAP). Calls `printk("%.*s", (int)arg3, (char *)arg2)`.
Returns `arg3`.

Note: SMAP (Supervisor Mode Access Prevention) is not enabled in Phase 5, so
direct pointer dereference of user addresses from kernel code is safe.

**`exit` (60)**: `arg1`=exit code. Calls `sched_exit()`. Does not return.

Unknown syscall numbers: return `-1` (ENOSYS equivalent).

### `sched_exit()` (`kernel/sched/sched.c`)

Removes `s_current` from the circular list, decrements `s_task_count`, then
`ctx_switch`es to the next task. O(n) predecessor search (acceptable for
Phase 5's small task count).

```c
void sched_exit(void) {
    aegis_task_t *prev = s_current;
    while (prev->next != s_current)
        prev = prev->next;

    aegis_task_t *dying = s_current;
    s_current = dying->next;
    prev->next = s_current;
    s_task_count--;

    if (s_current == dying) {          /* last task */
        arch_request_shutdown();
        for (;;) __asm__ volatile ("hlt");
    }

    arch_set_kernel_stack(s_current->kernel_stack_top);
    ctx_switch(dying, s_current);
    __builtin_unreachable();
}
```

Memory for the exited process (TCB, kernel stack, user page tables, ELF pages)
is leaked. Phase 6 adds cleanup.

---

## Section 5: User Binary (`user/init/`)

```c
/* user/init/main.c — no libc, syscall stubs via inline asm */
static inline long sys_write(int fd, const char *buf, long len) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(1L), "D"((long)fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline void sys_exit(int code) {
    __asm__ volatile ("syscall"
        : : "a"(60L), "D"((long)code) : "rcx", "r11");
    __builtin_unreachable();
}

void _start(void) {
    for (int i = 0; i < 3; i++)
        sys_write(1, "[USER] hello from ring 3\n", 25);
    sys_write(1, "[USER] done\n", 12);
    sys_exit(0);
}
```

Compiled with:
```
x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc -static \
  -Wl,-Ttext=0x400000 -Wl,-e,_start -o user/init/init.elf user/init/main.c
```

**Embedding**: Makefile rule runs `xxd -i user/init/init.elf > user/init/init_bin.c`
after building the ELF. This generates `extern const uint8_t user_init_init_elf[]`
and `extern const unsigned int user_init_init_elf_len`. `proc_spawn` receives
these as `(init_elf, init_elf_len)`.

**Build order dependency**: `init_bin.c` must be built before kernel objects that
include it. The Makefile uses an explicit prerequisite.

---

## Section 6: Test Harness

### Updated `tests/expected/boot.txt`

```
[SERIAL] OK: COM1 initialized at 115200 baud
[VGA] OK: text mode 80x25
[PMM] OK: 127MB usable across 2 regions
[VMM] OK: kernel mapped to 0xFFFFFFFF80000000
[CAP] OK: capability subsystem reserved
[IDT] OK: 48 vectors installed
[PIC] OK: IRQ0-15 remapped to vectors 0x20-0x2F
[PIT] OK: timer at 100 Hz
[KBD] OK: PS/2 keyboard ready
[GDT] OK: ring 3 descriptors installed
[TSS] OK: RSP0 initialized
[SYSCALL] OK: SYSCALL/SYSRET enabled
[SCHED] OK: scheduler started, 3 tasks
[USER] hello from ring 3
[USER] hello from ring 3
[USER] hello from ring 3
[USER] done
[AEGIS] System halted.
```

Note: `[SCHED]` now reports 3 tasks (task_kbd, task_heartbeat, init process).
`task_heartbeat` remains as the shutdown mechanism (500-tick watchdog).
The user process completes and calls `sched_exit` well before 500 ticks.
Output ordering is deterministic: user process runs to completion (tight loop,
completes in <1ms real time) long before task_heartbeat's 500-tick deadline.

### `make test` exit condition

Unchanged: QEMU exits via `isa-debug-exit` when `task_heartbeat` calls
`arch_request_shutdown()` after 500 ticks. The user process will have already
completed and called `sched_exit` by then.

---

## Identity Map Constraint

All Phase 5 PMM allocations (process TCB, kernel stack, user page tables,
ELF segment pages, user stack) happen during early boot before the scheduler
starts, or in `proc_spawn` which is called before `sched_start`. The total
allocation is well under 4MB. The identity-map constraint from CLAUDE.md
is satisfied without changes; the mapped-window allocator is deferred to
Phase 6.

---

## CLAUDE.md Build Status After Phase 5

| Subsystem | Status |
|-----------|--------|
| User space (ring 3) | ✅ Done |
| ELF loader (static) | ✅ Done |
| Syscall dispatch (write, exit) | ✅ Done |
| Capability system | ⬜ Phase 6 |
| Mapped-window allocator | ⬜ Phase 6 |
| VFS | ⬜ Phase 6+ |
| musl port + shell | ⬜ Phase 6+ |
