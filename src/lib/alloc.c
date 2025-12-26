#include <lib/alloc.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <mem/pmm.h>
#include <common/boot.h>
#include <kernel/kprintf.h>
#include <stdint.h>
#include <stddef.h>

#define KALLOC_MAGIC 0x4B4D414C /* 'KMAL' */

typedef struct {
    uint32_t magic;
    uint32_t pages;
} kalloc_header_t;

extern char _kernel_bss_end[];
static uintptr_t heap_cur = 0;

static void kmalloc_init(void)
{
    if (heap_cur) return;
    heap_cur = PAGE_ALIGN_UP((uintptr_t)_kernel_bss_end);
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;

    /* small allocations -> slab */
    if (size <= 2048) {
        return slab_alloc(size);
    }

    kmalloc_init();

    size_t tot = size + sizeof(kalloc_header_t);
    size_t npages = DIV_ROUND_UP(tot, PAGE_SIZE);

    uintptr_t start = heap_cur;

    size_t mapped = 0;
    for (size_t i = 0; i < npages; ++i) {
        uint64_t phys = vmm_map_alloc_page(start + i * PAGE_SIZE, VMM_PTE_W);
        if (!phys) break;
        mapped++;
    }

    if (mapped != npages) {
        /* unwind
         * for each mapped page: unmap and free frame
         */
        for (size_t i = 0; i < mapped; ++i) {
            uintptr_t v = start + i * PAGE_SIZE;
            uint64_t phys = vmm_translate(v);
            if (phys) {
                vmm_unmap_page(v);
                pmm_free_frame(phys);
            }
        }
        return NULL;
    }

    /* Write header at start of region */
    kalloc_header_t *h = (kalloc_header_t*)start;
    h->magic = KALLOC_MAGIC;
    h->pages = (uint32_t)npages;

    heap_cur += npages * PAGE_SIZE;

    return (void*)(start + sizeof(kalloc_header_t));
}

void kfree(void *ptr)
{
    if (!ptr) return;

    uintptr_t page_base = (uintptr_t)ptr & ~(PAGE_SIZE - 1);
    kalloc_header_t *h = (kalloc_header_t*)page_base;

    if (h->magic == KALLOC_MAGIC) {
        for (uint32_t i = 0; i < h->pages; ++i) {
            uintptr_t v = page_base + (uintptr_t)i * PAGE_SIZE;
            uint64_t phys = vmm_translate(v);
            if (phys) {
                vmm_unmap_page(v);
                pmm_free_frame(phys);
            }
        }
        return;
    }

    /* otherwise assume slab object */
    slab_free(ptr);
}
