# Audit: Memory Management — VMM/PMM/KVA/VMA/sys_memory

**Date:** 2026-04-01 | **Auditor:** Agent C (Opus)

## CRITICAL

### C1 — waitpid reaps thread zombie, destroys shared PML4
**File:** sys_process.c:760
**Description:** vmm_free_user_pml4 called unconditionally on zombie reap. CLONE_VM threads share PML4. Reaping a thread frees the entire address space while parent+siblings still run.
**Impact:** Instant kernel crash or page table use-after-free.
**Fix:** Check child->pml4_phys == leader->pml4_phys && child->pid != child->tgid → skip free. Or refcount PML4.

### C2 — sys_clone kstack alloc failure leaks fd_table + corrupts vma_refcount
**File:** sys_process.c:338-340
**Description:** On kva_alloc_pages failure, error path only frees child PCB. Does not: unref fd_table, free VMA, decrement thread_count.
**Fix:** Full cleanup in error path.

## HIGH

### H1 — sys_munmap shared-VMA detection only checks exact base match
**File:** sys_memory.c:366-372
**Description:** munmap on sub-range of MAP_SHARED mapping misses the shared check. pmm_free_page frees memfd pages still mapped in other processes.
**Impact:** Cross-process memory corruption via freed shared pages.
**Fix:** Range overlap check instead of exact base match.

### H2 — vma_share refcount stored per-thread, not shared
**File:** vma.c:306-307
**Description:** Each thread stores its own copy of vma_refcount. With 3+ threads, counts diverge. Table freed while still in use or never freed.
**Fix:** Single shared refcount location (in VMA page or separate struct).

### H3 — sys_mmap freelist-returned base has no overflow validation
**File:** sys_memory.c:235-240
**Description:** Freelist path skips the base+len > USER_ADDR_MAX check. Corrupted freelist entry could map into kernel space.
**Fix:** Add overflow check after freelist allocation too.

### H4 — PROT_NONE pages leak physical frames permanently (DoS)
**File:** vmm.c:632-635, sys_memory.c:376-381
**Description:** vmm_phys_of_user only returns PRESENT pages. After mprotect(PROT_NONE), munmap can't find phys to free. Trivially exploitable: mmap/mprotect(PROT_NONE)/munmap loop exhausts RAM.
**Fix:** Walk page table directly for non-zero PTEs regardless of PRESENT bit.

### H5 — sys_munmap inserts kernel addresses into mmap freelist
**File:** sys_memory.c:353-391
**Description:** No user-address-space validation. munmap(0xFFFFFFFF80000000) inserts kernel VA into freelist. Next mmap returns it → user pages mapped at kernel addresses.
**Impact:** Privilege escalation.
**Fix:** `if (arg1 >= USER_ADDR_MAX || arg1 + len > USER_ADDR_MAX) return -EINVAL;`

## MEDIUM

### M1 — mmap freelist per-thread copy diverges under CLONE_VM
**File:** sys_memory.c:106-168
**Description:** Each thread has own mmap_free[] copy. CLONE_VM threads allocate overlapping VAs.
**Fix:** Shared mmap_state_t with spinlock, like fd_table.

### M2 — sys_brk has no lower bound check
**File:** sys_memory.c:28
**Description:** Can shrink brk below brk_base, unmapping ELF segments.
**Fix:** Store initial brk in proc->brk_base, reject arg1 < brk_base.

### M3 — vmm_free_user_pml4 vs munmap PROT_NONE asymmetry (latent double-free)

## LOW

### L1 — PMM double-free silently succeeds (masks bugs)
### L2 — kva_alloc_pages panics on OOM instead of returning NULL
### L3 — vma_insert silently drops entries when table full (enables shared-VMA bypass)
