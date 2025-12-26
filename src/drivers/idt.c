#include <drivers/idt.h>
#include <kernel/kprintf.h>
#include <lib/sys/io.h>
#include <common/boot.h>
const char *exception_messages[32] = {
    "Division by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Detected overflow",
    "Out-of-bounds",
    "Invalid opcode",
    "No coprocessor",
    "Double fault",
    "Coprocessor segment overrun",
    "Bad TSS",
    "Segment not present",
    "Stack fault",
    "General protection fault",
    "Page fault",
    "Unknown interrupt",
    "Coprocessor fault",
    "Alignment check",
    "Machine check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};


__attribute__((aligned(16))) static idt_entry_t idt_entries[256];
static idt_desc_t idt_register;
extern void *isr_table[256];
void *idt_handlers[256] = { 0 };

/* Snapshot records (no C stdlib or printk in handlers) */
volatile uint64_t df_record[6] = {0};
volatile uint64_t pf_record[6] = {0};
volatile uint64_t gpf_record[6] = {0};

void df_handler(context_t *ctx) {
    df_record[0] = 0x4446425553460001ULL;
    df_record[1] = ctx->int_no;
    df_record[2] = ctx->err_code;
    df_record[3] = ctx->rip;
    df_record[5] = ctx->rflags;
    kprintf("Double Fault detected! Halting system.\n");
    kprintf("  RIP: %llx RFLAGS: %llx\n", ctx->rip, ctx->rflags);
    uint64_t real_rsp = (uint64_t)ctx + sizeof(context_t);
    kprintf("  RSP: %llx\n", real_rsp);
    kprintf("int_no=%d err=%lx rip=%lx cs=%lx rflags=%lx\n", 
        ctx->int_no, ctx->err_code, ctx->rip, ctx->cs, ctx->rflags);
    for(;;) __asm__ volatile("cli; hlt");
}

void pf_handler(context_t *ctx) {
    pf_record[0] = 0x5046425553460001ULL;
    pf_record[1] = ctx->int_no;
    pf_record[2] = ctx->err_code;
    pf_record[3] = ctx->rip;
    pf_record[5] = ctx->rflags;
    for(;;) __asm__ volatile("cli; hlt");
}

void gpf_handler(context_t *ctx) {
    gpf_record[0] = 0x4746425553460001ULL;
    gpf_record[1] = ctx->int_no;
    gpf_record[2] = ctx->err_code;
    gpf_record[3] = ctx->rip;
    gpf_record[5] = ctx->rflags;
    for(;;) __asm__ volatile("cli; hlt");
}

/* IRQ capture: record vector and RIP for IRQs 0..15 (vectors 32..47) */
volatile uint64_t irq_record[16][2] = {{0}};

void irq_handler(context_t *ctx) {
    uint8_t v = (uint8_t)ctx->int_no;
    if (v >= 32 && v < 48) {
        uint8_t idx = v - 32;
        irq_record[idx][0] = v;
        irq_record[idx][1] = ctx->rip;
    }
    /* Ensure we acknowledge PIC */
    outb(0x20, 0x20);
}


void interrupts_set_entry(uint16_t vector, void *isr, uint8_t flags) {
	idt_entry_t *entry = &idt_entries[vector];
	entry->off_low = (uint16_t)((uint64_t)isr & 0xFFFF);
	entry->selector = 0x08;
	entry->ist = 0;
	entry->flags = flags;
	entry->off_mid = (uint16_t)(((uint64_t)isr >> 16) & 0xFFFF);
	entry->off_high = (uint32_t)(((uint64_t)isr >> 32) & 0xFFFFFFFF);
	entry->zero = 0;
}
void pic_remap(uint8_t offset1, uint8_t offset2) {
    uint8_t a1, a2;
    
    // Save masks
    a1 = inb(0x21);
    a2 = inb(0xA1);
    
    // Start initialization sequence (ICW1)
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    
    // ICW2: Set vector offsets
    outb(0x21, offset1);  // Master PIC offset (IRQ 0-7 → vectors 32-39)
    outb(0xA1, offset2);  // Slave PIC offset (IRQ 8-15 → vectors 40-47)
    
    // ICW3: Tell Master/Slave about each other
    outb(0x21, 0x04);  // Master: slave at IRQ2
    outb(0xA1, 0x02);  // Slave: cascade identity
    
    // ICW4: 8086 mode
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    
    // Restore masks
    outb(0x21, a1);
    outb(0xA1, a2);
}
void interrupts_init() {
    for (uint16_t vector = 0; vector < 256; vector++)
        interrupts_set_entry(vector, isr_table[vector], 0x8E);
    
    interrupts_set_handler(8, df_handler);
    interrupts_set_handler(14, pf_handler);
    interrupts_set_handler(13, gpf_handler);
    
    for (int v = 32; v <= 47; v++)
        interrupts_set_handler(v, irq_handler);
    

    //enable pit
    pic_remap(32, 40);



    

    idt_register.size = sizeof(idt_entries) - 1;
    idt_register.addr = (uint64_t)&idt_entries;
    __asm__ volatile ("lidt %0" : : "m"(idt_register) : "memory");
}
void interrupts_reload() {
	__asm__ volatile ("lidt %0" : : "m"(idt_register) : "memory");
	__asm__ volatile ("sti");
}

void interrupts_set_handler(uint8_t vector, void *handler) {
	idt_handlers[vector] = handler;
}

uint8_t interrupts_alloc_vec() {
	static uint8_t free_vector = 48;
	return free_vector++;
}

void interrupts_handle_int(context_t *ctx) {
    
    void(*handler)(context_t*) = idt_handlers[ctx->int_no];
    if (handler) {
        handler(ctx);
        return;
    }
    //handle exceptions with messages
    if (ctx->int_no < sizeof(exception_messages) / sizeof(exception_messages[0])) {
       //only print the exception number
       kprintf("%d\n", ctx->int_no);
        kprintf(LOG_ERROR "Interrupts: Exception %d: %s\n", ctx->int_no, exception_messages[ctx->int_no]);
        kprintf(LOG_ERROR "  RIP: %llx CS: %llx RFLAGS: %llx\n", ctx->rip, ctx->cs, ctx->rflags);
        kprintf(LOG_ERROR "  Error code: %llx\n", ctx->err_code);
        for(;;) {
            __asm__ volatile("hlt");
            __asm__ volatile("cli");
        }

    } else {
        kprintf(LOG_ERROR "Interrupts: Exception %d: Unknown\n", ctx->int_no);
    }

	kprintf(LOG_ERROR "Interrupts: Unhandled interrupt %d.\n", ctx->int_no);
}

void interrupts_eoi() {
	outb(0x20, 0x20);
}