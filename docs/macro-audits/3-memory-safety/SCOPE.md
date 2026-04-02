# Audit 3: Memory Safety

## Priority: HIGH

Kernel memory bugs are instant privilege escalation. The PMM, VMM, and KVA
allocators are the foundation everything else relies on.

## Files to review

| File | LOC | Focus |
|------|-----|-------|
| `kernel/mm/vmm.c` | 942 | Page table manipulation, window allocator, user PML4 teardown |
| `kernel/mm/pmm.c` | — | Bitmap allocator, double-free detection, OOM handling |
| `kernel/mm/kva.c` | — | Bump allocator, kva_free_pages (VA not reclaimed) |
| `kernel/mm/vma.c` | 320 | VMA tracking, 170-slot table |
| `kernel/syscall/sys_memory.c` | 438 | mmap, munmap, mprotect, brk |
| `kernel/proc/proc.c` | 575 | Process creation, PML4 setup, ELF loading |

## Checklist

### Use-after-free
- [ ] vmm_free_user_pml4 doesn't free pages still mapped in another process
- [ ] KVA pages freed in sched_exit aren't still referenced
- [ ] Pipe/socket ring buffers freed only when both ends closed
- [ ] Process TCB not accessed after sched_exit removes it from run queue

### Double-free
- [ ] PMM bitmap cleared exactly once per free
- [ ] No path where pmm_free_page is called twice for the same frame
- [ ] fork failure path doesn't free pages that were never allocated

### OOM handling
- [ ] alloc_table (vmm.c) returns error, not panic — verify all callers check
- [ ] pmm_alloc_page failure propagated (not silently ignored)
- [ ] mmap returns -ENOMEM, not NULL dereference
- [ ] The `ls /` OOM bug: trace brk/mmap allocations during `ls /` to find the leak

### Page table integrity
- [ ] Only user-half entries (PML4[0-255]) freed in vmm_free_user_pml4
- [ ] Kernel-half entries (PML4[256-511]) never modified per-process
- [ ] TLB invalidation after every PTE change (invlpg or full flush)
- [ ] Window allocator PTE is volatile (no compiler reordering)

### Integer overflow
- [ ] brk growth calculation doesn't overflow
- [ ] mmap size + alignment doesn't wrap around
- [ ] copy_from_user/copy_to_user length checks
- [ ] VMA start + size doesn't exceed user address space

## Output format

Same as Audit 1.
