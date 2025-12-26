#include <mem/pmm.h>
#include <common/boot.h>
#include <kernel/kprintf.h>
#include <common/multiboot2.h>
#include <lib/string.h>

/* Bitmap size (bytes) to support up to 8 * PMM_BITMAP_BYTES pages. 256KiB -> ~2M pages (~8GiB)
 * Keep it reasonable in .bss */
#define PMM_BITMAP_BYTES (256 * 1024)
static uint8_t pmm_bitmap[PMM_BITMAP_BYTES];
static uint64_t total_frames = 0;
static uint64_t max_phys = 0;
static size_t free_frames = 0;

static inline void set_frame_used(uint64_t frame)
{
    uint64_t byte = frame >> 3;
    uint8_t bit = frame & 7;
    pmm_bitmap[byte] |= (1 << bit);
}
static inline void set_frame_free(uint64_t frame)
{
    uint64_t byte = frame >> 3;
    uint8_t bit = frame & 7;
    pmm_bitmap[byte] &= ~(1 << bit);
}
static inline int frame_is_free(uint64_t frame)
{
    uint64_t byte = frame >> 3;
    uint8_t bit = frame & 7;
    return (pmm_bitmap[byte] & (1 << bit)) == 0;
}

void pmm_init(uint64_t multiboot_phys_addr)
{
    /* Zero bitmap (mark everything used first) */
    memset(pmm_bitmap, 0xFF, PMM_BITMAP_BYTES);

    /* Parse multiboot memory map */
    uint64_t mb = multiboot_phys_addr;
    if (!mb) {
        kprintf("pmm_init: no multiboot info\n");
        return;
    }

    struct multiboot_tag *tag = (struct multiboot_tag*)(mb + 8);

    uint64_t highest = 0;

    for (; tag->type != MULTIBOOT_TAG_TYPE_END; tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mm = (struct multiboot_tag_mmap*)tag;
            uint32_t entries_len = mm->size - sizeof(struct multiboot_tag_mmap);
            uint8_t *ptr = (uint8_t*)mm->entries;
            for (uint32_t off = 0; off + mm->entry_size <= entries_len; off += mm->entry_size) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry*)(ptr + off);
                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    uint64_t end = e->addr + e->len;
                    if (end > highest) highest = end;
                }
            }
        }
    }

    max_phys = highest;
    total_frames = (max_phys + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t needed_bytes = (total_frames + 7) / 8;
    if (needed_bytes > PMM_BITMAP_BYTES) {
        kprintf("pmm_init: memory too large, clipping bitmap to %llu bytes (supports %llu frames)\n",
                (unsigned long long)PMM_BITMAP_BYTES, (unsigned long long)(PMM_BITMAP_BYTES * 8));
        total_frames = (PMM_BITMAP_BYTES * 8);
    }

    /* Mark all frames used by default */
    for (uint64_t f = 0; f < total_frames; ++f) set_frame_used(f);

    /* Now mark available regions as free excluding kernel and reserved ranges */
    tag = (struct multiboot_tag*)(mb + 8);
    for (; tag->type != MULTIBOOT_TAG_TYPE_END; tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mm = (struct multiboot_tag_mmap*)tag;
            uint32_t entries_len = mm->size - sizeof(struct multiboot_tag_mmap);
            uint8_t *ptr = (uint8_t*)mm->entries;
            for (uint32_t off = 0; off + mm->entry_size <= entries_len; off += mm->entry_size) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry*)(ptr + off);
                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    uint64_t start = e->addr;
                    uint64_t end = e->addr + e->len;
                    if (start >= max_phys) continue;
                    if (end > max_phys) end = max_phys;
                    uint64_t fstart = start / PMM_PAGE_SIZE;
                    uint64_t fend = (end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
                    for (uint64_t f = fstart; f < fend && f < total_frames; ++f) {
                        set_frame_free(f);
                    }
                }
            }
        }
    }

    /* Reserve frames used by the kernel image */
    extern char _kernel_phys_start[];
    extern char _kernel_load_end[];
    extern char _kernel_bss_end[];
    uint64_t kstart = (uint64_t)_kernel_phys_start;
    uint64_t kend = (uint64_t)_kernel_bss_end;
    if (kend == 0) kend = (uint64_t)_kernel_load_end;
    if (kstart < kend) {
        uint64_t fstart = kstart / PMM_PAGE_SIZE;
        uint64_t fend = (kend + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        for (uint64_t f = fstart; f < fend && f < total_frames; ++f) set_frame_used(f);
    }

    /* Reserve low memory below 1MiB (BIOS/IVT/EBDA areas) */
    uint64_t low_limit_frames = 0x100000 / PMM_PAGE_SIZE;
    for (uint64_t f = 0; f < low_limit_frames && f < total_frames; ++f) set_frame_used(f);

    /* Reserve the multiboot info block itself (so we don't clobber tags/modules) */
    uint32_t mb_total_size = *(uint32_t*)mb;
    if (mb_total_size > 0) {
        uint64_t mb_start = mb;
        uint64_t mb_end = mb + mb_total_size;
        uint64_t fstart = mb_start / PMM_PAGE_SIZE;
        uint64_t fend = (mb_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        kprintf("pmm_init: reserving multiboot info frames 0x%llx-0x%llx (frames %llu..%llu)\n",
                (unsigned long long)mb_start, (unsigned long long)mb_end, (unsigned long long)fstart, (unsigned long long)(fend-1));
        for (uint64_t f = fstart; f < fend && f < total_frames; ++f) set_frame_used(f);
    }

    /* Reserve any multiboot modules (initrd, etc) so they won't be allocated */
    for (struct multiboot_tag *t = (struct multiboot_tag*)(mb + 8);
         t->type != MULTIBOOT_TAG_TYPE_END;
         t = (struct multiboot_tag*)((uint8_t*)t + ((t->size + 7) & ~7))) {
        if (t->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module *m = (struct multiboot_tag_module*)t;
            uint64_t mstart = (uint64_t)m->mod_start;
            uint64_t mend = (uint64_t)m->mod_end;
            if (mstart < mend) {
                uint64_t fstart = mstart / PMM_PAGE_SIZE;
                uint64_t fend = (mend + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
                kprintf("pmm_init: reserving module frames 0x%llx-0x%llx (frames %llu..%llu)\n",
                        (unsigned long long)mstart, (unsigned long long)mend, (unsigned long long)fstart, (unsigned long long)(fend-1));
                for (uint64_t f = fstart; f < fend && f < total_frames; ++f) set_frame_used(f);
            }
        }
    }

    /* Count free frames */
    free_frames = 0;
    for (uint64_t f = 0; f < total_frames; ++f) if (frame_is_free(f)) free_frames++;

    kprintf("pmm_init: max_phys=0x%llx total_frames=%llu free_frames=%zu\n", (unsigned long long)max_phys, (unsigned long long)total_frames, free_frames);
}

uint64_t pmm_alloc_frame(void)
{
    if (free_frames == 0) return 0;
    uint64_t frames = total_frames;
    for (uint64_t b = 0; b < (total_frames + 7) / 8; ++b) {
        uint8_t v = pmm_bitmap[b];
        if (v == 0xFF) continue; /* all used */
        /* find first zero bit */
        for (int bit = 0; bit < 8; ++bit) {
            uint64_t f = b * 8 + bit;
            if (f >= frames) return 0;
            if (frame_is_free(f)) {
                set_frame_used(f);
                free_frames--;
                return f * PMM_PAGE_SIZE;
            }
        }
    }
    return 0;
}

void pmm_free_frame(uint64_t phys)
{
    if (phys % PMM_PAGE_SIZE) return; /* not aligned */
    uint64_t f = phys / PMM_PAGE_SIZE;
    if (f >= total_frames) return;
    if (!frame_is_free(f)) {
        set_frame_free(f);
        free_frames++;
    }
}

size_t pmm_free_count(void)
{
    return free_frames;
}
