#pragma once
#include <stdint.h>
#include <stddef.h>

/* Basic VMM helpers for mapping/unmapping 4KiB pages in the active page tables */
#define VMM_PTE_P 0x001
#define VMM_PTE_W 0x002
#define VMM_PTE_U 0x004
#define VMM_PTE_PS  (1ULL << 7)
/* Initialize VMM (captures current CR3 / PML4 physical address) */
void vmm_init(void);

/* Map a single 4KiB page (returns 0 on success, -1 on error) */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap a single 4KiB page */
int vmm_unmap_page(uint64_t virt);

/* Translate virtual -> physical by walking page tables (returns phys or 0) */
uint64_t vmm_translate(uint64_t virt);

/* Helper to allocate and map a kernel page (map a newly allocated physical frame) */
uint64_t vmm_map_alloc_page(uint64_t virt, uint64_t flags);

/* Return current PML4 physical address */
uint64_t vmm_get_pml4_phys(void);
