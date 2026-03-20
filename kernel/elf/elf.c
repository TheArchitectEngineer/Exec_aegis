#include "elf.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../core/printk.h"
#include <stdint.h>
#include <stddef.h>

/* ELF64 header */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* ELF64 program header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD    1
#define PF_W       2      /* program header write flag */
#define ELFCLASS64 2
#define ET_EXEC    2
#define EM_X86_64  0x3E

uint64_t
elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len)
{
    (void)len;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Verify ELF magic */
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        printk("[ELF] FAIL: bad magic\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_type != ET_EXEC ||
        eh->e_machine != EM_X86_64) {
        printk("[ELF] FAIL: not a static ELF64 x86-64 executable\n");
        return 0;
    }

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(data + eh->e_phoff);
    uint16_t i;
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;

        /* Allocate physically contiguous pages for this segment.
         *
         * CONTIGUITY ASSUMPTION: The Phase 3 bitmap PMM allocates sequentially,
         * so successive pmm_alloc_page() calls return physically adjacent frames.
         * This allows treating the pages as a single region for memcpy.
         * If the PMM becomes non-sequential (Phase 6+ buddy allocator),
         * replace this with a page-by-page copy. Same assumption as sched_spawn. */
        uint64_t page_count = (ph->p_memsz + 4095UL) / 4096UL;
        uint64_t first_phys = 0;
        uint64_t j;
        for (j = 0; j < page_count; j++) {
            uint64_t p = pmm_alloc_page();
            if (!p) {
                printk("[ELF] FAIL: OOM loading segment\n");
                for (;;) {}
            }
            if (j == 0)
                first_phys = p;
        }

        /* Copy file bytes into physical memory */
        uint8_t *dst = (uint8_t *)(uintptr_t)first_phys;
        const uint8_t *src = data + ph->p_offset;
        uint64_t k;
        for (k = 0; k < ph->p_filesz; k++)
            dst[k] = src[k];

        /* Zero BSS (bytes past p_filesz up to p_memsz) */
        for (k = ph->p_filesz; k < ph->p_memsz; k++)
            dst[k] = 0;

        /* Map each page into the user address space */
        uint64_t map_flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            map_flags |= VMM_FLAG_WRITABLE;

        for (j = 0; j < page_count; j++) {
            vmm_map_user_page(pml4_phys,
                              ph->p_vaddr + j * 4096UL,
                              first_phys  + j * 4096UL,
                              map_flags);
        }
    }

    return eh->e_entry;
}
