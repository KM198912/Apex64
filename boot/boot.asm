; boot.asm — Multiboot2 → long mode with Limine-like mappings done in the stub
;
; Contract at kernel_main:
;  - Kernel executes at KERNEL_VIRT_BASE (higher half)
;  - Low identity map exists for 0..4GiB (2MiB pages)
;  - HHDM map exists: virt = HHDM_BASE + phys for phys 0..4GiB (2MiB pages)
;  - Framebuffer MMIO typically <4GiB is already mapped via identity+HHDM

BITS 32

%define MB2_MAGIC 0xE85250D6
%define MB2_ARCH  0

%define KERNEL_VIRT_BASE  0xFFFFFFFF80000000
%define HHDM_BASE         0xFFFF800000000000
%define KERNEL_PHYS_BASE  0x00200000

%define KERNEL_MAP_SIZE   (128 * 1024 * 1024)   ; 128MiB mapped for kernel window

%define PTE_P   0x001
%define PTE_W   0x002
%define PDE_PS  0x080

SECTION .multiboot
align 8
mb2_hdr_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd mb2_hdr_end - mb2_hdr_start
    dd -(MB2_MAGIC + MB2_ARCH + (mb2_hdr_end - mb2_hdr_start))

    ; Address tag (type 2): request load at KERNEL_PHYS_BASE
    align 8
    dw 2
    dw 0
    dd 24
    dd mb2_hdr_start         ; header_addr (physical where header ends up)
    dd KERNEL_PHYS_BASE      ; load_addr
    dd _kernel_load_end      ; load_end_addr (physical) - provided by linker
    dd _kernel_bss_end       ; bss_end_addr  (physical) - provided by linker
    ; Entry address tag (type 3) — REQUIRED if you use the address tag in GRUB
    align 8
    dw 3                ; type = entry address
    dw 0                ; flags
    dd 12               ; size
    dd _TitanBoot       ; entry_addr (32-bit physical address)

    ; Pad to 8-byte boundary for the next tag
    dd 0

    ; Optional framebuffer request tag (type 5)
    align 8
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32

    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
mb2_hdr_end:

; ----------------------------
; Low bootstrap BSS (must stay low)
; ----------------------------
SECTION .boot.bss nobits
align 16
stack_bottom:
    resb 16384
stack_top:

align 4096
global pml4, pdpt_low, pdpt_kern
global pd_low_0, pd_low_1, pd_low_2, pd_low_3
global pd_kern_0

pml4:       resb 4096
pdpt_low:   resb 4096
pd_low_0:   resb 4096
pd_low_1:   resb 4096
pd_low_2:   resb 4096
pd_low_3:   resb 4096

pdpt_kern:  resb 4096
pd_kern_0:  resb 4096

; ----------------------------
; Low bootstrap data/code (must stay low)
; ----------------------------
SECTION .boot

SECTION .boot.data
align 16
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)  ; code seg
.data: equ $ - gdt64
    dq (1<<44) | (1<<47)                      ; data seg
.ptr:
    dw $ - gdt64 - 1
    dq gdt64

SECTION .boot
global _TitanBoot
_TitanBoot:
    cli

    ; Save Multiboot2 info pointer from GRUB
    mov esi, ebx

    ; Stack is in low .boot.bss
    mov esp, stack_top
    cld

    call check_cpuid
    call check_long_mode

    call setup_page_tables

    ; Restore mb2 info pointer
    mov ebx, esi

    ; Enable PAE
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; Load CR3 with low address of PML4
    mov eax, pml4
    mov cr3, eax

    ; Enable long mode
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable paging
    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    lgdt [gdt64.ptr]
    jmp gdt64.code:long_mode_start

; ----------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no
    ret
.no:
    mov al, 'C'
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no
    ret
.no:
    mov al, 'L'
    jmp error

; ----------------------------
; setup_page_tables (2MiB pages only):
; - PML4[0]   -> pdpt_low  (identity 0..4GiB)
; - PML4[256] -> pdpt_low  (HHDM_BASE + phys for phys 0..4GiB)
; - PML4[511] -> pdpt_kern (kernel higher-half mapping)
; ----------------------------
setup_page_tables:
    ; Clear tables: pml4 + pdpt_low + 4 PDs + pdpt_kern + pd_kern_0 = 8 pages
    mov edi, pml4
    mov ecx, (8 * 4096) / 4
    xor eax, eax
    rep stosd

    ; PML4[0] -> pdpt_low
    mov eax, pdpt_low
    or  eax, (PTE_P | PTE_W)
    mov [pml4 + 0*8], eax

    ; PML4[256] -> pdpt_low (HHDM)
    mov eax, pdpt_low
    or  eax, (PTE_P | PTE_W)
    mov [pml4 + 256*8], eax

    ; PDPT low: 4 entries for 0..4GiB
    mov eax, pd_low_0
    or  eax, (PTE_P | PTE_W)
    mov [pdpt_low + 0*8], eax

    mov eax, pd_low_1
    or  eax, (PTE_P | PTE_W)
    mov [pdpt_low + 1*8], eax

    mov eax, pd_low_2
    or  eax, (PTE_P | PTE_W)
    mov [pdpt_low + 2*8], eax

    mov eax, pd_low_3
    or  eax, (PTE_P | PTE_W)
    mov [pdpt_low + 3*8], eax

    ; Fill each PD with 2MiB pages
    call fill_pd0
    call fill_pd1
    call fill_pd2
    call fill_pd3

    ; Kernel mapping:
    ; KERNEL_VIRT_BASE (0xffffffff80000000) lies at PML4=511, PDPT=510.
    mov eax, pdpt_kern
    or  eax, (PTE_P | PTE_W)
    mov [pml4 + 511*8], eax

    ; pdpt_kern[510] -> pd_kern_0
    mov eax, pd_kern_0
    or  eax, (PTE_P | PTE_W)
    mov [pdpt_kern + 510*8], eax

    ; Fill pd_kern_0: map KERNEL_MAP_SIZE starting at KERNEL_PHYS_BASE with 2MiB pages
    mov edi, pd_kern_0
    xor ecx, ecx
    mov eax, KERNEL_PHYS_BASE
.fill_kern:
    mov edx, eax
    or  edx, (PTE_P | PTE_W | PDE_PS)
    mov [edi + ecx*8], edx
    add eax, 0x00200000
    inc ecx
    cmp ecx, (KERNEL_MAP_SIZE >> 21)
    jne .fill_kern

    ret

; --- Helpers to fill identity PDs ---
fill_pd0:
    mov edi, pd_low_0
    xor ecx, ecx
.loop:
    mov eax, ecx
    shl eax, 21
    or  eax, (PTE_P | PTE_W | PDE_PS)
    mov [edi + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

fill_pd1:
    mov edi, pd_low_1
    xor ecx, ecx
.loop:
    mov eax, ecx
    shl eax, 21
    add eax, 0x40000000
    or  eax, (PTE_P | PTE_W | PDE_PS)
    mov [edi + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

fill_pd2:
    mov edi, pd_low_2
    xor ecx, ecx
.loop:
    mov eax, ecx
    shl eax, 21
    add eax, 0x80000000
    or  eax, (PTE_P | PTE_W | PDE_PS)
    mov [edi + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

fill_pd3:
    mov edi, pd_low_3
    xor ecx, ecx
.loop:
    mov eax, ecx
    shl eax, 21
    add eax, 0xC0000000
    or  eax, (PTE_P | PTE_W | PDE_PS)
    mov [edi + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

; ----------------------------
error:
    mov dword [0xB8000], 0x4F524F45
    mov byte  [0xB8004], al
    mov byte  [0xB8005], 0x4F
    hlt

; ----------------------------
BITS 64
long_mode_start:
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top

    ; kernel_main(mb2_info_phys, HHDM_BASE)
    xor rdi, rdi
    mov edi, ebx
    mov rsi, HHDM_BASE

    extern _start
    mov rax, _start
    call rax

.hang:
    cli
    hlt
    jmp .hang

; Symbols provided by linker (physical addresses for MB2 address tag)
extern _kernel_load_end
extern _kernel_bss_end
