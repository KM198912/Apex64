; boot.asm — Multiboot2 → long mode with Limine-like mappings done in the stub
;
; Contract at kernel_main:
;  - Kernel executes at KERNEL_VIRT_BASE (higher half)
;  - Low identity map exists for 0..4GiB (2MiB pages)
;  - HHDM map exists: virt = HHDM_BASE + phys for phys 0..4GiB (2MiB pages)
;  - Framebuffer MMIO typically <4GiB is already mapped via identity+HHDM
;  - Todo: 16-bit AP Trampoline for SMP Bringup
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
    dd 0
    dd 0
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

; Per-AP stacks (each AP gets its own stack to avoid clobbering BSP stack)
%define AP_STACK_SIZE 0x8000           ; 32 KiB per AP (0x8000 = 32768 bytes)
%define MAX_APS 1024                ; Max supported APs, threadripper might want this ..... (hobby kernel on a monster? get real)

align 4096
ap_stacks:
    resb AP_STACK_SIZE * MAX_APS

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
    dq 0x0000000000000000           ; 0x00: Null descriptor
.code: equ $ - gdt64
    dq 0x00209A0000000000           ; 0x08: 64-bit code (L=1, P=1, S=1, type=code)
.data: equ $ - gdt64
    dq 0x0000920000000000           ; 0x10: 64-bit data (P=1, S=1, type=data)
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
    ; Set valid data segment for SS
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Map stack to HHDM
    mov rsp, stack_top
    mov rax, HHDM_BASE
    add rsp, rax

    ; Fix mb2_info pointer
    xor rdi, rdi
    mov edi, ebx
    add rdi, rax          ; rdi = mb2_info + HHDM_BASE
    mov rsi, rax          ; rsi = HHDM_BASE

    extern _start
    mov rax, _start
    call rax

.hang:
    cli
    hlt
    jmp .hang


; ----------------------------
; AP startup trampoline (16-bit -> protected -> long mode)
; To use: copy the bytes between `ap_trampoline_start` and `ap_trampoline_end`
; to a low physical address (e.g., 0x7000) and issue INIT/SIPI to AP with that vector.
SECTION .boot
align 16
global ap_trampoline_start, ap_trampoline_end, ap_trampoline_jmp_slot, ap_trampoline_jmp_instr, ap_trampoline_pm
ap_trampoline_start:
    ; 16-bit real mode entry
    BITS 16
    cli

    ; Real-mode marker: write 'R' to physical 0x8000 so BSP can observe progress
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x4000
    mov di, 0x8000
    mov BYTE [di], 'R'
    ; Serial port 0xE9 debug 'R'
    mov dx, 0x00E9
    mov al, 'R'
    out dx, al

    ; Load a minimal GDT from the trampoline itself
    ; The GDT is at a fixed offset from the start of the trampoline
    ; ap_trampoline_start + 0x20 = ap_trampoline_gdt
    ; ap_trampoline_start + 0x38 = ap_trampoline_gdt_ptr
    BITS 32
    
    mov eax, 0x7038
    lgdt [eax]
    BITS 16
    mov BYTE [di], 'H'
    ; Serial port 0xE9 debug 'H'
    mov dx, 0x00E9
    mov al, 'H'
    out dx, al

    ; Marker before CR0 reads
    mov dx, 0x00E9
    mov al, 'G'
    out dx, al

    ; Enter protected mode
    mov eax, cr0

    ; Marker after read CR0
    mov dx, 0x00E9
    mov al, 'J'
    out dx, al

    or  eax, 1

    ; Marker after set bit
    mov dx, 0x00E9
    mov al, 'K'
    out dx, al

mov cr0, eax
mov BYTE [di], 'I'
; Serial port 0xE9 debug 'I'
mov dx, 0x00E9
mov al, 'I'
out dx, al

; Use an indirect far-jump via a memory pointer to avoid operand-size complexities
ap_trampoline_jmp_slot:
    ; placeholder for patched far-jump (8 bytes): 66 EA dd dd dd dd dw dw
    times 8 db 0x90

    ; Compute absolute physical address of jump slot and use register-indirect far JMP
    ; Note: expression below is evaluated at assemble time and yields a constant (no external relocation)
    ; Short-jump into the patched instruction area that will contain an EA immediate
    jmp short ap_trampoline_jmp_instr

    ; Debug: if the short jump fails and execution continues here, report and halt
    mov dx, 0x00E9
    mov al, 'F'
    out dx, al
    hlt

ap_trampoline_jmp_instr:
    ; placeholder for EA immediate instruction: 66 EA dd dd dd dd dw dw
    times 8 db 0x90

; Fallback marker: if we see 'Z' here it means the far-jump failed and execution continued
mov dx, 0x00E9
mov al, 'Z'
out dx, al

; Minimal GDT for AP trampoline (must be in the trampoline section)
align 8
ap_trampoline_gdt:
    dq 0x0000000000000000           ; Null descriptor
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code (P=1, S=1, type=code, D/B=1)
    dq 0x00A09A0000000000           ; 0x10: 64-bit long-mode code (L=1)
    dq 0x00CF92000000FFFF           ; 0x18: 32-bit data (P=1, S=1, type=data)
ap_trampoline_gdt_ptr:
    dw 0x1F  ; GDT limit (4 descriptors * 8 - 1 = 31 = 0x1F)
    dd 0x7020  ; GDT base (0x7000 + 0x20, where ap_trampoline_gdt is located)
; Pad to offset 0xA0 for 32-bit code
times (0xA0 - ($ - ap_trampoline_start)) db 0x90
; 32-bit protected mode entry point
BITS 32
ap_trampoline_pm:
    ; Set up data segments and stack in protected mode to avoid GP faults
    mov ax, 0x18            ; data selector (ap_trampoline_gdt index 3)
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x4000

    ; Protected-mode marker: write 'P' to physical 0x8001
    mov edi, 0x8001
    mov BYTE [edi], 'P'
    ; Serial port 0xE9 debug 'P'
    mov dx, 0x00E9
    mov al, 'P'
    out dx, al

    ; Load PML4 physical address into CR3
    mov eax, pml4
    mov cr3, eax

    ; Enable PAE (required for long mode)
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; Enable Long Mode (LME)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable paging (PG)
    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    ; Far jump to 64-bit entry (GDT code selector = 0x10)
    ; Use push/retf in 32-bit mode so the IP is a 32-bit immediate and avoids
    ; relocations that need 16-bit truncation. Use selector 0x10 (index 2) which is
    ; the 64-bit code descriptor in the trampoline GDT.
    push dword ap_trampoline_64_entry - ap_trampoline_start + 0x7000
    push word 0x10
    retf

; 64-bit long mode entry
BITS 64
ap_trampoline_64_entry:
    ; Setup data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor rax, rax
    mov fs, ax
    mov gs, ax

    ; mark long-mode arrival early
    mov rdi, 0x8002
    mov BYTE [rdi], 'L'
    ; Serial port 0xE9 debug 'L'
    mov dx, 0x00E9
    mov al, 'L'
    out dx, al

    ; Setup stack: choose per-CPU stack based on local APIC ID
    ; Read IA32_APIC_BASE MSR (0x1B) to find LAPIC physical base
    mov ecx, 0x1B
    rdmsr
        mov BYTE [rdi], 'M'
    ; Serial port 0xE9 debug 'L'
    mov dx, 0x00E9
    mov al, 'M'
    out dx, al
    ; rdx:rax -> physical apic base
    mov rbx, rdx
    shl rbx, 32
    or  rbx, rax
    and rbx, 0xFFFFF000
    mov rax, HHDM_BASE
    add rbx, rax
    ; read APIC ID from LAPIC ID register (offset 0x20, ID in bits 24..31)
    mov eax, dword [rbx + 0x20]
    shr eax, 24
    and eax, 0xFF
    ; EAX now contains the APIC ID (writing to EAX zero-extends RAX)
    ; multiply by AP_STACK_SIZE (32768 = 0x8000) using shift (<< 15) on RAX
    sal rax, 15
    mov ebx, stack_top
    add ebx, ap_stacks - stack_top
    mov rcx, HHDM_BASE
    add rbx, rcx
    add rax, rbx
    ; point to top of stack for that AP
    add rax, AP_STACK_SIZE
    mov rsp, rax

    ; Call kernel-provided `ap_entry` in the future (hook point).
    ; Invoke `ap_entry` (if present) so the AP can perform kernel init. If it returns,
    ; halt the AP.
    extern ap_entry
    call ap_entry
    
    ; If ap_entry returns, halt the AP
    cli
    hlt
    jmp $
ap_trampoline_end:

global ap_trampoline_size
ap_trampoline_size: dq ap_trampoline_end - ap_trampoline_start

; Symbols provided by linker (physical addresses for MB2 address tag)
extern _kernel_load_end
extern _kernel_bss_end
