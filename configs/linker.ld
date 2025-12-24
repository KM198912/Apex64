ENTRY(_TitanBoot)

KERNEL_VIRT_BASE = 0xffffffff80000000;
KERNEL_PHYS_BASE = 0x00200000;

SECTIONS
{
    /* Physical load base */
    . = KERNEL_PHYS_BASE;
    _kernel_phys_start = .;

    /* ---- Low bootstrap domain (VMA=LMA low) ---- */
    .multiboot ALIGN(8) :
    {
        KEEP(*(.multiboot))
    }

    .boot ALIGN(4K) :
    {
        KEEP(*(.boot))
        KEEP(*(.boot.*))
    }

    .boot.bss ALIGN(4K) (NOLOAD) :
    {
        KEEP(*(.boot.bss))
        KEEP(*(.boot.bss.*))
    }

    /* Mark end of the low bootstrap area in *physical* space */
    _boot_phys_end = .;

    /* ---- Higher-half kernel domain (VMA high, LMA continues low) ---- */
    . = KERNEL_VIRT_BASE + ( _boot_phys_end - KERNEL_PHYS_BASE );
    _kernel_virt_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)
    {
        *(.text*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)
    {
        *(.rodata*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)
    {
        *(.data*)
    }

    /* Physical end of file-backed image (used by MB2 address tag load_end_addr) */
    _kernel_load_end = LOADADDR(.data) + SIZEOF(.data);

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_BASE + KERNEL_PHYS_BASE)
    {
        *(COMMON)
        *(.bss*)
    }

    /* Physical end including BSS (used by MB2 address tag bss_end_addr) */
    _kernel_bss_end = LOADADDR(.bss) + SIZEOF(.bss);

    _kernel_end = .;
}
