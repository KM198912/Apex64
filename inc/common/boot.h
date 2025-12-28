#pragma once
#include <stdint.h>
#include <stddef.h>  /* for uintptr_t */
#include <stdbool.h>
#ifdef TITAN_NO_PTR
    #define TITAN_PTR(TYPE) uint64_t
#else
    #define TITAN_PTR(TYPE) TYPE
#endif
#define MAX_CPUS 1024 //match boot.asm, some amd cpus can have more than 256 cpus
typedef struct TitanCpu {
    uint32_t apic_id;
    uint32_t processor_id;
    bool is_bsp;
} TitanCpu;

typedef struct TitanSmpInfo {
    uint32_t cpu_count;
    TitanCpu cpus[MAX_CPUS];
} TitanSmpInfo;

typedef void (*titan_goto_fn)(void *arg);

typedef struct titan_mp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    titan_goto_fn goto_address;
    uint64_t extra_argument;   // or void *arg
} titan_mp_info_t;

typedef struct TitanFramebuffer {
    TITAN_PTR(void *) addr;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t green_mask;
    uint32_t red_mask;
    uint32_t blue_mask;
    uint32_t green_shift;
    uint32_t red_shift;
    uint32_t blue_shift;
    uint8_t bpp;
} TitanFramebuffer;

typedef struct TitanEdid {
    uint8_t data[128];
    // Parsed fields for convenience
    uint8_t header[8];
    uint16_t manufacturer_id;
    uint16_t product_code;
    uint32_t serial_number;
    uint8_t week_of_manufacture;
    uint8_t year_of_manufacture;
    uint8_t edid_version;
    uint8_t edid_revision;
    uint8_t video_input_type;
    uint8_t max_horizontal_image_size;
    uint8_t max_vertical_image_size;
    uint8_t display_gamma;
    uint8_t supported_features;
    // ... add more parsed fields as needed
} TitanEdid; 

//hack like CPP, when compiled with no pointer support, we might need them anyway in some rare cases
#ifdef TITAN_NO_PTR
    #define REINTERPRET_AS(TYPE, VALUE) ((TYPE)(uintptr_t)(VALUE))
#else
    #define REINTERPRET_AS(TYPE, VALUE) ((TYPE)(VALUE))
#endif

#define CAST_AS(TYPE, VALUE) ((TYPE)(uintptr_t)(VALUE))


#define MAX_BOOT_MODULES 16

typedef struct boot {
    uint64_t mb2_addr;
    uint64_t hhdm_base;
    TitanFramebuffer framebuffer;
    size_t kernel_size;
    void* acpi_ptr;
    
    /* Kernel command line (if provided via multiboot) */
    char cmdline[256];  // Changed from pointer to array
    
    size_t module_count;
    size_t module_sizes[MAX_BOOT_MODULES];  // Changed from pointer to array
    void *modules[MAX_BOOT_MODULES];        // Changed from pointer to array
    char* module_path[MAX_BOOT_MODULES];    // Changed from pointer to array
    TitanCpu smp_cpus[MAX_CPUS];
    TitanSmpInfo smp_info;
    titan_mp_info_t mp_info[MAX_CPUS];
} boot_t;

extern boot_t TitanBootInfo;

/* phys <-> hhdm */
#define PHYS_TO_VIRT(phys) ((void*)((uintptr_t)(phys) + (uintptr_t)TitanBootInfo.hhdm_base))
#define VIRT_TO_PHYS(virt) ((uint64_t)((uintptr_t)(virt) - (uintptr_t)TitanBootInfo.hhdm_base))

/* compatibility aliases if you like the names */
#define HIGHER_HALF(phys) PHYS_TO_VIRT(phys)

/* math helpers (integer-only) */
#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN(n, d) ((n) / (d))

#define ALIGN_UP(n, a)   (DIV_ROUND_UP((uintptr_t)(n), (a)) * (a))
#define ALIGN_DOWN(n, a) (DIV_ROUND_DOWN((uintptr_t)(n), (a)) * (a))

/* power-of-two only */
#define IS_ALIGNED_POW2(n, a) ((((uintptr_t)(n)) & ((a) - 1)) == 0)

#define PAGE_SIZE 0x1000
#define PAGE_ALIGN_UP(n)   ALIGN_UP((n), PAGE_SIZE)
#define PAGE_ALIGN_DOWN(n) ALIGN_DOWN((n), PAGE_SIZE)

// kernel Protoypes
void kernel_main();
void kernel_run();