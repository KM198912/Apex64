#include <common/boot.h>
#include <drivers/fb.h>
#include <kernel/kprintf.h>
#include <common/multiboot2.h>

const char* memory_type_to_string(uint32_t type) {
    switch (type) {
        case MULTIBOOT_MEMORY_AVAILABLE: return "Available";
        case MULTIBOOT_MEMORY_RESERVED: return "Reserved";
        case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
        case MULTIBOOT_MEMORY_NVS: return "NVS";
        case MULTIBOOT_MEMORY_BADRAM: return "Bad RAM";
        default: return "Unknown";
    }
}

static void print_memory_map(void) {
    uint64_t mb = TitanBootInfo.mb2_addr;
    if (!mb) {
        kprintf("No multiboot info available\n");
        return;
    }
    uint64_t total_usable = 0;
    struct multiboot_tag *tag = (struct multiboot_tag*)PHYS_TO_VIRT(mb + 8);

    /* compute tag sizes using padded tag sizes and include the end tag */
    uint32_t computed = 0;
    int tag_count = 0;
    for (struct multiboot_tag *t = tag; ; t = (struct multiboot_tag*)((uint8_t*)t + ((t->size + 7) & ~7))) {
        uint32_t padded = (t->size + 7) & ~7U;
        computed += padded;
        tag_count++;
        if (t->type == MULTIBOOT_TAG_TYPE_END) break;
    }
    /* optional warning for mismatches */
    if (computed != ((*(uint32_t*)PHYS_TO_VIRT(mb)) - 8)) {
        kprintf("WARNING: tag sizes mismatch by %d bytes\n", (int)((*(uint32_t*)PHYS_TO_VIRT(mb)) - 8 - computed));
    }

    for (; tag->type != MULTIBOOT_TAG_TYPE_END; tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mm = (struct multiboot_tag_mmap*)tag;
            kprintf("Memory map: entry_size=%u entry_version=%u\n", mm->entry_size, mm->entry_version);
            uint32_t entries_len = mm->size - sizeof(struct multiboot_tag_mmap);
            uint8_t *ptr = (uint8_t*)mm->entries;
            for (uint32_t off = 0; off + mm->entry_size <= entries_len; off += mm->entry_size) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry*)(ptr + off);
                kprintf("  base=0x%016llx len=0x%016llx type=%s\n", (unsigned long long)e->addr, (unsigned long long)e->len, memory_type_to_string(e->type));
                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    total_usable += e->len;
                }
            }
            kprintf("Total usable memory: %llu bytes (%.2f MB)\n", (unsigned long long)total_usable, (double)total_usable / (1024.0 * 1024.0));
            return;
        }
    }

    kprintf("No memory map tag found\n");
}

void kernel_main()
{
    framebuffer_early_init();
    kprintf(LOG_OK "Kernel initialized successfully!\n");
    // print memory map
    print_memory_map();
    //print acpi address
    kprintf("ACPI RSDP pointer: %p\n", TitanBootInfo.acpi_ptr);
    kprintf("Kernel size: %zu bytes (%.2f MB)\n", TitanBootInfo.kernel_size, (double)TitanBootInfo.kernel_size / (1024.0 * 1024.0));
}
void kernel_run()
{
    // Kernel main loop
    while (1) {
        __asm__ volatile("hlt");
    }
}