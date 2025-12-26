#include <mem/vmm.h>
#include <mem/pmm.h>
#include <common/boot.h>
#include <kernel/kprintf.h>
#include <lib/string.h>

/* Internal state */
static uint64_t pml4_phys = 0;

/* Helpers for indexes */
static inline int idx_pml4(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline int idx_pdpt(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline int idx_pd(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline int idx_pt(uint64_t v)   { return (v >> 12) & 0x1FF; }

/* Read CR3 to get active PML4 physical address */
static uint64_t read_cr3_phys(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r" (v));
    return v;
}

void vmm_init(void)
{
    pml4_phys = read_cr3_phys();
    kprintf("vmm_init: pml4_phys=0x%016llx\n", (unsigned long long)pml4_phys);
}

uint64_t vmm_get_pml4_phys(void) { return pml4_phys; }

/* Ensure a page table page exists at a given entry pointer; allocates frame using PMM if needed */
static uint64_t ensure_table(uint64_t *entry)
{
    if ((*entry) & VMM_PTE_P) return (*entry) & ~0xFFFULL;

    uint64_t new_frame = pmm_alloc_frame();
    if (!new_frame) return 0;
    /* zero table */
    void *ptr = PHYS_TO_VIRT(new_frame);
    memset(ptr, 0, 4096);
    *entry = (new_frame & ~0xFFFULL) | VMM_PTE_P | VMM_PTE_W;
    return new_frame;
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!pml4_phys) return -1;
    uint64_t *pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);

    int i4 = idx_pml4(virt);
    int i3 = idx_pdpt(virt);
    int i2 = idx_pd(virt);
    int i1 = idx_pt(virt);

    uint64_t pdpt_phys = ensure_table(&pml4[i4]);
    if (!pdpt_phys) return -1;
    uint64_t *pdpt = (uint64_t*)PHYS_TO_VIRT(pdpt_phys);

    uint64_t pd_phys = ensure_table(&pdpt[i3]);
    if (!pd_phys) return -1;
    uint64_t *pd = (uint64_t*)PHYS_TO_VIRT(pd_phys);

    uint64_t pt_phys = ensure_table(&pd[i2]);
    if (!pt_phys) return -1;
    uint64_t *pt = (uint64_t*)PHYS_TO_VIRT(pt_phys);

    /* set final PTE */
    pt[i1] = (phys & ~0xFFFULL) | (flags & 0xFFFULL) | VMM_PTE_P;
    __asm__ volatile ("invlpg (%0)" :: "r" ((void*)virt));
    return 0;
}

int vmm_unmap_page(uint64_t virt)
{
    if (!pml4_phys) return -1;
    uint64_t *pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);

    int i4 = idx_pml4(virt);
    int i3 = idx_pdpt(virt);
    int i2 = idx_pd(virt);
    int i1 = idx_pt(virt);

    if (!(pml4[i4] & VMM_PTE_P)) return -1;
    uint64_t pdpt_phys = pml4[i4] & ~0xFFFULL;
    uint64_t *pdpt = (uint64_t*)PHYS_TO_VIRT(pdpt_phys);

    if (!(pdpt[i3] & VMM_PTE_P)) return -1;
    uint64_t pd_phys = pdpt[i3] & ~0xFFFULL;
    uint64_t *pd = (uint64_t*)PHYS_TO_VIRT(pd_phys);

    if (!(pd[i2] & VMM_PTE_P)) return -1;
    uint64_t pt_phys = pd[i2] & ~0xFFFULL;
    uint64_t *pt = (uint64_t*)PHYS_TO_VIRT(pt_phys);

    pt[i1] = 0;
    __asm__ volatile ("invlpg (%0)" :: "r" ((void*)virt));
    return 0;
}

uint64_t vmm_translate(uint64_t virt)
{
    if (!pml4_phys) return 0;
    uint64_t *pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);

    int i4 = idx_pml4(virt);
    int i3 = idx_pdpt(virt);
    int i2 = idx_pd(virt);
    int i1 = idx_pt(virt);

    if (!(pml4[i4] & VMM_PTE_P)) return 0;
    uint64_t pdpt_phys = pml4[i4] & ~0xFFFULL;
    uint64_t *pdpt = (uint64_t*)PHYS_TO_VIRT(pdpt_phys);

    if (!(pdpt[i3] & VMM_PTE_P)) return 0;

    /* 1GiB page */
    if (pdpt[i3] & VMM_PTE_PS) {
        uint64_t base = pdpt[i3] & 0xFFFFFC0000000ULL;   // bits 51..30
        return base | (virt & 0x3FFFFFFFULL);            // low 30 bits
    }

    uint64_t pd_phys = pdpt[i3] & ~0xFFFULL;
    uint64_t *pd = (uint64_t*)PHYS_TO_VIRT(pd_phys);

    if (!(pd[i2] & VMM_PTE_P)) return 0;

    /* 2MiB page */
    if (pd[i2] & VMM_PTE_PS) {
        uint64_t base = pd[i2] & 0xFFFFFFE00000ULL;      // bits 51..21
        return base | (virt & 0x1FFFFFULL);              // low 21 bits
    }

    uint64_t pt_phys = pd[i2] & ~0xFFFULL;
    uint64_t *pt = (uint64_t*)PHYS_TO_VIRT(pt_phys);

    if (!(pt[i1] & VMM_PTE_P)) return 0;
    return (pt[i1] & ~0xFFFULL) | (virt & 0xFFFULL);
}

uint64_t vmm_map_alloc_page(uint64_t virt, uint64_t flags)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    if (vmm_map_page(virt, phys, flags) < 0) {
        pmm_free_frame(phys);
        return 0;
    }
    return phys;
}
