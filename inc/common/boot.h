#pragma once
#include <stdint.h>
#include <stddef.h>  /* for uintptr_t */
#ifdef TITAN_NO_PTR
    #define TITAN_PTR(TYPE) uint64_t
#else
    #define TITAN_PTR(TYPE) TYPE
#endif
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

//hack like CPP, when compiled with no pointer support, we might need them anyway in some rare cases
#ifdef TITAN_NO_PTR
    #define REINTERPRET_AS(TYPE, VALUE) ((TYPE)(uintptr_t)(VALUE))
#else
    #define REINTERPRET_AS(TYPE, VALUE) ((TYPE)(VALUE))
#endif

#define CAST_AS(TYPE, VALUE) ((TYPE)(uintptr_t)(VALUE))

typedef struct boot {
    uint64_t mb2_addr;
    uint64_t hhdm_base;
    TitanFramebuffer framebuffer;
    size_t kernel_size;
    void* acpi_ptr;
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