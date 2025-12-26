#include <stdint.h>
#include <stddef.h>
#include <common/multiboot2.h>
#include <common/boot.h>
boot_t TitanBootInfo;

extern char _kernel_phys_start[];
extern char _kernel_load_end[];
extern char _kernel_bss_end[];
size_t kernel_size = 0;

/*
* TitanBoot64 does what no other bootloader dares to do: it enables SSE support
* right in the entry point, before even calling the kernel main function. This ensures
* that the kernel can immediately leverage the power of SSE instructions for
* optimized computations, floating-point operations, and data processing from the very start.
* By enabling SSE early, TitanBoot64 sets the stage for a high-performance computing environment,
* allowing the kernel to take full advantage of modern CPU capabilities without delay.
* Additionally, this allows for floating point operations in kprintf, which can be very useful for debugging.
* and bypasses ubsan issues, like __fixdfdi and others, which would require SSE to work properly.
* For other Developers, if you intend on implementing SMP, make sure to include this either as "extern void enable_sse();" or make it public via a header,
* then call it for each AP you start, before any floating point or SSE instruction is executed, apart from the Boot Processor (BP).
*/
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

    // Find framebuffer
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

        TitanBootInfo.framebuffer.addr   = (void*)(uintptr_t)fb_addr;
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
    }

    // Find ACPI pointer
    for (struct multiboot_tag* tag = (struct multiboot_tag*)(base + 8);
         (uintptr_t)tag + sizeof(*tag) <= end && tag->type != MULTIBOOT_TAG_TYPE_END;
         tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7))) {
        
        if ((uintptr_t)tag + tag->size > end) break;
        
        if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD || tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW) {
            void* rsdp = (void*)((uintptr_t)tag + sizeof(struct multiboot_tag));
            TitanBootInfo.acpi_ptr = rsdp;
            break;
        }
    }

    // Count modules and populate module array
    size_t module_index = 0;
    for (struct multiboot_tag* tag = (struct multiboot_tag*)(base + 8);
         (uintptr_t)tag + sizeof(*tag) <= end && tag->type != MULTIBOOT_TAG_TYPE_END;
         tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7))) {

        if ((uintptr_t)tag + tag->size > end) break;

        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;

            if (module_index < MAX_BOOT_MODULES) {
                    /* Convert physical module start to kernel virtual address (HHDM) */
                    TitanBootInfo.modules[module_index] = PHYS_TO_VIRT((uintptr_t)mod->mod_start);
                    TitanBootInfo.module_sizes[module_index] = (size_t)(mod->mod_end - mod->mod_start);
                    TitanBootInfo.module_path[module_index] = (char*)((uintptr_t)mod + sizeof(struct multiboot_tag_module));
                    module_index++;
                }
        }
    }

    TitanBootInfo.module_count = module_index;

    // Find and copy cmdline
    for (struct multiboot_tag* tag = (struct multiboot_tag*)(base + 8);
         (uintptr_t)tag + sizeof(*tag) <= end && tag->type != MULTIBOOT_TAG_TYPE_END;
         tag = (struct multiboot_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7))) {
        
        if ((uintptr_t)tag + tag->size > end) break;
        
        if (tag->type == MULTIBOOT_TAG_TYPE_CMDLINE) {
            const char* cmdline_src = (const char*)((uintptr_t)tag + 8);
            
            // Copy directly to TitanBootInfo.cmdline array
            size_t len = 0;
            while (cmdline_src[len] && len < sizeof(TitanBootInfo.cmdline) - 1) {
                TitanBootInfo.cmdline[len] = cmdline_src[len];
                len++;
            }
            TitanBootInfo.cmdline[len] = '\0';
            break;
        }
    }

    TitanBootInfo.kernel_size = (size_t)((uintptr_t)_kernel_load_end - (uintptr_t)_kernel_phys_start);
    
    kernel_main();
    kernel_run();
}