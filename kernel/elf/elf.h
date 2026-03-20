#ifndef AEGIS_ELF_H
#define AEGIS_ELF_H

#include <stdint.h>
#include <stddef.h>

/* Load a static ELF64 into pml4_phys's address space.
 * Maps all PT_LOAD segments; allocates PMM pages.
 * Returns e_entry (virtual entry point) on success, 0 on parse error.
 * Panics if PMM is exhausted. */
uint64_t elf_load(uint64_t pml4_phys, const uint8_t *data, size_t len);

#endif /* AEGIS_ELF_H */
