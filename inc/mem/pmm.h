#pragma once
#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096

/* Initialize the PMM using the Multiboot2 memory map address (physical) */
void pmm_init(uint64_t multiboot_phys_addr);

/* Allocate a single page frame; returns physical address or 0 on failure */
uint64_t pmm_alloc_frame(void);

/* Free a single frame previously allocated (phys is page-aligned) */
void pmm_free_frame(uint64_t phys);

/* Return count of free frames currently available */
size_t pmm_free_count(void);
