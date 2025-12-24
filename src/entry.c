#include <stdint.h>
#include <stddef.h>
#include <common/multiboot2.h>
#include <common/boot.h>
boot_t TitanBootInfo;
//get _kernel_phys_start = .; and _kernel_phys_end = .; from linker script
extern char _kernel_phys_start[];
extern char _kernel_load_end[];
extern char _kernel_bss_end[];
size_t kernel_size = 0;
void enable_sse() {
    __asm__ __volatile__ (
        "mov %cr0, %rax\n"
        "and $~(1 << 2), %rax\n"  // Clear EM (bit 2)
        "or $(1 << 1), %rax\n"    // Set MP (bit 1)
        "mov %rax, %cr0\n"
        "mov %cr4, %rax\n"
        "or $(3 << 9), %rax\n"    // Set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
        "mov %rax, %cr4\n"
    );
    __asm__ volatile ("fninit");
}

void _start(uint64_t mb_addr, uint64_t hhdm_base) {
    TitanBootInfo.mb2_addr  = mb_addr;
    TitanBootInfo.hhdm_base = hhdm_base;
    enable_sse();
    uint32_t total_size = *(uint32_t*)PHYS_TO_VIRT(mb_addr);

    struct multiboot_tag_framebuffer* fb_tag = NULL;

    uintptr_t base = (uintptr_t)PHYS_TO_VIRT(mb_addr);
    uintptr_t end  = base + total_size;

    for (struct multiboot_tag* tag = (struct multiboot_tag*)(base + 8);
         (uintptr_t)tag + sizeof(*tag) <= end && tag->type != MULTIBOOT_TAG_TYPE_END;
         tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7))) {

        if ((uintptr_t)tag + tag->size > end) break;

        if (tag->type == MULTIBOOT_TAG_TYPE_FRAMEBUFFER) {
            fb_tag = (struct multiboot_tag_framebuffer*)tag;
            break;
        }
    }

    if (fb_tag) {
        uint64_t fb_addr = fb_tag->common.framebuffer_addr;
        uint64_t fb_size = (uint64_t)fb_tag->common.framebuffer_height *
                           (uint64_t)fb_tag->common.framebuffer_pitch;

        TitanBootInfo.framebuffer.addr   = fb_addr;
        TitanBootInfo.framebuffer.size   = fb_size;
        TitanBootInfo.framebuffer.width  = fb_tag->common.framebuffer_width;
        TitanBootInfo.framebuffer.height = fb_tag->common.framebuffer_height;
        TitanBootInfo.framebuffer.pitch  = fb_tag->common.framebuffer_pitch;
        TitanBootInfo.framebuffer.bpp    = fb_tag->common.framebuffer_bpp;

        TitanBootInfo.framebuffer.green_mask  = fb_tag->framebuffer_green_mask_size;
        TitanBootInfo.framebuffer.red_mask    = fb_tag->framebuffer_red_mask_size;
        TitanBootInfo.framebuffer.blue_mask   = fb_tag->framebuffer_blue_mask_size;

        TitanBootInfo.framebuffer.green_shift = fb_tag->framebuffer_green_field_position;
        TitanBootInfo.framebuffer.red_shift   = fb_tag->framebuffer_red_field_position;
        TitanBootInfo.framebuffer.blue_shift  = fb_tag->framebuffer_blue_field_position;

        volatile uint32_t* fb = (volatile uint32_t*)PHYS_TO_VIRT(fb_addr);
        uint32_t stride = TitanBootInfo.framebuffer.pitch / 4;
        //find the ACPI_PTR
        for (struct multiboot_tag* tag = (struct multiboot_tag*)(base + 8);
             (uintptr_t)tag + sizeof(*tag) <= end && tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7))) {
            if ((uintptr_t)tag + tag->size > end) break;
            if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD || tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW) {
                void* rsdp = (void*)((uintptr_t)tag + sizeof(struct multiboot_tag));
                TitanBootInfo.acpi_ptr = rsdp;              // already in your address space

            }
        }
        TitanBootInfo.kernel_size =  (size_t)((uintptr_t)_kernel_load_end - (uintptr_t)_kernel_phys_start);
    }
    kernel_main();
    kernel_run();
}
