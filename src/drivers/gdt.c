#include <drivers/gdt.h>
#include <kernel/kprintf.h>
#define SMP_MAX_CPU_COUNT 8
gdt_table_t gdt_table_vec[SMP_MAX_CPU_COUNT];
gdt_desc_t gdt_desc_vec[SMP_MAX_CPU_COUNT];
tss_desc_t tss_desc_vec[SMP_MAX_CPU_COUNT];

void gdt_reload_seg();


/* Per-CPU IST stacks for critical exceptions (e.g., double-fault) */
static uint8_t df_ist_stacks[SMP_MAX_CPU_COUNT][4096] __attribute__((aligned(16)));

void gdt_init(uint32_t cpu_id) {
	tss_desc_vec[cpu_id].iopb = sizeof(tss_desc_t);
	gdt_table_vec[cpu_id].entries[0] = 0x0000000000000000;
	gdt_table_vec[cpu_id].entries[1] = 0x00af9b000000ffff; // 0x08 64 Bit CS (Code).
	gdt_table_vec[cpu_id].entries[2] = 0x00af93000000ffff; // 0x10 64 Bit SS (Data).
	gdt_table_vec[cpu_id].entries[3] = 0x00aff3000000ffff; // 0x18 User mode SS (Data).
	gdt_table_vec[cpu_id].entries[4] = 0x00affb000000ffff; // 0x20 User mode CS (Code).

	uint64_t tss_base = (uint64_t)&tss_desc_vec[cpu_id];
	gdt_table_vec[cpu_id].tss_entry.len = sizeof(tss_desc_t) - 1;
	gdt_table_vec[cpu_id].tss_entry.base = (uint16_t)(tss_base & 0xffff);
	gdt_table_vec[cpu_id].tss_entry.base1 = (uint8_t)((tss_base >> 16) & 0xff);
	gdt_table_vec[cpu_id].tss_entry.flags = 0x89;
	gdt_table_vec[cpu_id].tss_entry.flags1 = 0;
	gdt_table_vec[cpu_id].tss_entry.base2 = (uint8_t)((tss_base >> 24) & 0xff);
	gdt_table_vec[cpu_id].tss_entry.base3 = (uint32_t)(tss_base >> 32);
	gdt_table_vec[cpu_id].tss_entry.resv = 0;

	/* Initialize IST0 -> double-fault stack top */
	tss_set_ist(cpu_id, 0, &df_ist_stacks[cpu_id][sizeof(df_ist_stacks[cpu_id])]);

	gdt_desc_vec[cpu_id].size = sizeof(gdt_table_t) - 1;
	gdt_desc_vec[cpu_id].addr = (uint64_t)&gdt_table_vec[cpu_id];

	__asm__ volatile ("lgdt %0" : : "m"(gdt_desc_vec[cpu_id]) : "memory");
	__asm__ volatile ("ltr %0" : : "r"(0x28) : "memory");

	gdt_reload_seg();
}

void tss_set_rsp(uint32_t cpu_id, int rsp, void *addr) {
    if (cpu_id >= SMP_MAX_CPU_COUNT || rsp < 0 || rsp > 2)
        return;
    tss_desc_vec[cpu_id].rsp[rsp] = (uint64_t)addr;
}

void tss_set_ist(uint32_t cpu_id, int idx, void *addr) {
    if (cpu_id >= SMP_MAX_CPU_COUNT || idx < 0 || idx >= 7)
        return;
    tss_desc_vec[cpu_id].ist[idx] = (uint64_t)addr;
}