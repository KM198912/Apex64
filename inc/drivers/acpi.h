#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/idt.h>
typedef struct {
	char sign[8];
	uint8_t checksum;
	char oem[6];
	uint8_t rev;
	uint32_t rsdt_addr;
	uint32_t len;
	uint64_t xsdt_addr;
	uint8_t ext_checksum;
	uint8_t resv[8];
} xsdp_t;
typedef struct {
	char sign[4];
	uint32_t len;
	uint8_t rev;
	uint8_t checksum;
	char oem[6];
	char oem_table[8];
	uint32_t oem_rev;
	uint32_t creator_id;
	uint32_t creator_rev;
} sdt_header_t;
typedef struct {
	sdt_header_t sdt_header;

	uint32_t apic_addr;
	uint32_t flags;

	char entry_table[];
} madt_t;

typedef struct {
	uint16_t resv;

	uint8_t ioapic_id;
	uint8_t resv1;
	uint32_t ioapic_addr;
	uint32_t gsi_base;
} madt_ioapic_t;

typedef struct {
	uint16_t resv;

	uint8_t bus_src;
	uint8_t irq_src;
	uint32_t gsi;
	uint16_t flags;
} madt_ioapic_iso_t;

typedef struct {
	uint16_t resv;

	uint16_t resv1;
	uint64_t lapic_addr;
} madt_lapic_override_t;

extern uint64_t madt_apic_addr;

extern madt_ioapic_t *madt_ioapic_vec[32];
extern madt_ioapic_iso_t *madt_iso_vec[32];

extern int madt_ioapic_count;
extern int madt_iso_count;

/* Count of started CPUs (BSP starts at 1) */
extern volatile uint32_t smp_started_count; 

void madt_init();
#define APIC_MSR 0x1B
#define APIC_REG_ID 0x20
#define APIC_REG_EOI 0xB0
#define APIC_REG_SPURIOUS_INT 0xF0
#define APIC_REG_ICR_LO 0x300
#define APIC_REG_ICR_HI 0x310
#define APIC_REG_LVT_TIMER 0x320
#define APIC_REG_INIT_CNT 0x380
#define APIC_REG_CURR_CNT 0x390
#define APIC_REG_DIV_CFG 0x3E0

#define APIC_IPI_SINGLE 0
#define APIC_IPI_EVERY 0x80000
#define APIC_IPI_OTHERS 0xC0000

#define APIC_FLAG_X2APIC (1 << 21)
typedef struct timer {
	void(*oneshot)(struct timer *timer, uint64_t ms, uint8_t vector);
} timer_t;

timer_t *timer_create(void *fn_oneshot);
typedef struct {
	uint32_t id;
	int interrupt_status;
	timer_t *local_timer;
	context_t *trap_frame;

//	thread_queue_t thread_queue;
//	thread_t *current_thread;

#if defined(__x86_64__)
	uint32_t apic_timer_ticks;
#endif
} cpu_t;

void apic_init();
void apic_eoi();
void apic_ipi(uint32_t id, uint32_t data, uint32_t type);
uint32_t apic_get_id();
void apic_timer_init();
//void apic_timer_oneshot(timer_t *timer, uint64_t ms, uint8_t vec);

void apic_write(uint32_t reg, uint64_t value);
uint64_t apic_read(uint32_t reg);

// I/O APIC

#define IOAPIC_REDIR_TABLE(n) (0x10 + 2 * n)

void ioapic_init();
void ioapic_map_irq(uint32_t apic_id, uint8_t irq, uint8_t vec, bool mask);

void ioapic_write(uint32_t base, uint8_t reg, uint32_t data);
uint32_t ioapic_read(uint32_t base, uint8_t reg);

/* Simple AP bringup (minimal) */
void smp_start_aps(void);
void madt_populate_smp_info(void);
void smp_build_mp_info(void);
void acpi_init(void);
void madt_init(void);
uint64_t find_smp_cores(void);
