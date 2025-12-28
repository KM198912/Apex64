#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t err_code, int_no;  // ← SWAPPED ORDER
    uint64_t rip, cs, rflags;
} __attribute__((packed)) context_t;
typedef struct {
	uint16_t off_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t flags;
	uint16_t off_mid;
	uint32_t off_high;
	uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
	uint16_t size;
	uint64_t addr;
} __attribute__((packed)) idt_desc_t;

void interrupts_init(void);
void interrupts_reload(void);
void interrupts_set_handler(uint8_t vector, void *handler);
void interrupts_handle_int(context_t *ctx);
void interrupts_eoi(void);
void apic_eoi(void);