#include <drivers/acpi.h>
#include <kernel/kprintf.h>
#include <common/boot.h>
#include <stdbool.h>
#include <kernel/assert.h>
#include <drivers/gdt.h>
#include <lib/string.h>
#include <lib/alloc.h>
#include <drivers/pit.h>
#include <drivers/idt.h>
void *sdt_address = NULL;
bool use_xsdt = false;
uint64_t cpu_read_msr(uint32_t msr) {
	uint32_t low = 0, high = 0;
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

void cpu_write_msr(uint32_t msr, uint64_t value) {
	__asm__ volatile ("wrmsr" : : "a"(value), "d"((uint32_t)(value >> 32)), "c"(msr));
}
timer_t *timer_create(void *fn_oneshot) {
	timer_t *timer = (timer_t*)kmalloc(sizeof(timer_t));
	timer->oneshot = fn_oneshot;
	return timer;
}

void *acpi_find_table(const char *sign) {
	if (!sdt_address) {
		kprintf(LOG_ERROR "ACPI: sdt_address is NULL in acpi_find_table\n");
		return NULL;
	}
	
	sdt_header_t *sdt_header = (sdt_header_t*)sdt_address;
	/* Defensive: ensure entry_size is sane and header length is valid to avoid division by zero */
	size_t entry_size = use_xsdt ? 8 : 4;
	if (entry_size == 0) {
		kprintf(LOG_ERROR "ACPI: invalid entry_size=0 in acpi_find_table\n");
		return NULL;
	}
	uint64_t hdr_len = sdt_header->len;
	if (hdr_len < sizeof(sdt_header_t)) {
		kprintf(LOG_ERROR "ACPI: header length too small (%llu)\n", (unsigned long long)hdr_len);
		return NULL;
	}
	size_t entry_count = (size_t)((hdr_len - sizeof(sdt_header_t)) / entry_size);
	uint64_t *table_start = (uint64_t*)(sdt_header + 1);
	for (size_t i = 0; i < entry_count; i++) {
		sdt_header_t *header;
		uint64_t address = use_xsdt ? *(table_start + i) : *((uint32_t*)table_start + i);
		header = (sdt_header_t*)HIGHER_HALF(address);
		if (!memcmp(header->sign, sign, 4))
			return (void*)header;
	}
	return NULL;
}

void acpi_init() {
	xsdp_t *rsdp = (xsdp_t*)TitanBootInfo.acpi_ptr;
    ASSERT(rsdp != NULL, "ACPI RSDP pointer is NULL");
	use_xsdt = rsdp->rev == 2;
	sdt_address = use_xsdt ? (void*)rsdp->xsdt_addr : (void*)rsdp->rsdt_addr;
	sdt_address = HIGHER_HALF(sdt_address);
    kprintf(LOG_INFO "ACPI initialized using %s, found at %p\n", use_xsdt ? "XSDT" : "RSDT", sdt_address);
    uint64_t* madt = acpi_find_table("APIC");
    if (madt) {
        kprintf(LOG_OK "ACPI MADT found at %p\n", madt);
    } else {
        kprintf(LOG_ERROR "ACPI MADT not found!\n");
    }
    uint64_t* fadt = acpi_find_table("FACP");
    if (fadt) {
        kprintf(LOG_OK "ACPI FADT found at %p\n", fadt);
    } else {
        kprintf(LOG_ERROR "ACPI FADT not found!\n");    
    }
}

madt_ioapic_t *madt_ioapic_vec[32] = { 0 };
madt_ioapic_iso_t *madt_iso_vec[32] = { 0 };

int madt_ioapic_count = 0;
int madt_iso_count = 0;

uint64_t madt_apic_addr = 0;

void madt_init() {
	madt_t *madt = (madt_t*)acpi_find_table("APIC");
	ASSERT(madt != NULL, "MADT table not found during MADT init");

	madt_apic_addr = madt->apic_addr;

	uint64_t offset = 0;
	uint64_t table_size = madt->sdt_header.len - sizeof(madt_t);
	while (offset < table_size) {
		uint8_t entry_type = madt->entry_table[offset];
		uint8_t entry_len = madt->entry_table[offset + 1];
		void *entry_data = (void*)(madt->entry_table + offset);
		switch (entry_type) {
		case 1: {
			madt_ioapic_t *ioapic = (madt_ioapic_t*)entry_data;
			madt_ioapic_vec[madt_ioapic_count++] = ioapic;
			break;
		}
		case 2: {
			madt_ioapic_iso_t *iso = (madt_ioapic_iso_t*)entry_data;
			madt_iso_vec[madt_iso_count++] = iso;
			kprintf(LOG_INFO "Found Interrupt Source Override for IRQ #%d.\n", iso->irq_src);
			break;
		}
		case 5: {
			madt_lapic_override_t *lapic_override = (madt_lapic_override_t*)entry_data;
			madt_apic_addr = lapic_override->lapic_addr;
			break;
		}
		}
		offset += entry_len;
	}
	kprintf(LOG_INFO "MADT Found %d I/O APICs.\n", madt_ioapic_count);
}

uint64_t find_smp_cores() {
    madt_t *madt = (madt_t*)acpi_find_table("APIC");
    ASSERT(madt != NULL, "MADT table not found during SMP core count");
    uint64_t offset = 0;
    uint64_t table_size = madt->sdt_header.len - sizeof(madt_t);
    uint64_t cpu_count = 0;
    while (offset < table_size) {
        uint8_t entry_type = madt->entry_table[offset];
        uint8_t entry_len = madt->entry_table[offset + 1];
        void *entry_data = (void*)(madt->entry_table + offset);
        if (entry_type == 0) { // Processor Local APIC
            cpu_count++;
        }
        offset += entry_len;
    }
    return cpu_count;
}

uint32_t lapic_get_id(void)
{
    volatile uint32_t *lapic = (volatile uint32_t *)PHYS_TO_VIRT((uintptr_t)madt_apic_addr);

    /* LAPIC ID register (0x20): bits 24..31 contain xAPIC ID */
    return lapic[0x20 / 4] >> 24;
}


void madt_populate_smp_info(void)
{
    madt_t *madt = (madt_t*)acpi_find_table("APIC");
    ASSERT(madt != NULL, "MADT table not found during SMP core population");
    kprintf(LOG_INFO "Populating SMP info from MADT at %p\n", madt);

    /* Clear existing state (optional but recommended) */
    memset(TitanBootInfo.smp_cpus, 0, sizeof(TitanBootInfo.smp_cpus));
    TitanBootInfo.smp_info.cpu_count = 0;

    /* MADT header is: SDT header + lapic_addr (u32) + flags (u32) = 44 bytes typical.
       Your madt_t must have entry_table[] immediately after those fields. */
    uint64_t table_bytes = (uint64_t)madt->sdt_header.len;
    uint64_t entries_off = sizeof(madt_t);
    if (entries_off > table_bytes) {
        ASSERT(false, "MADT size is smaller than madt_t header");
    }

    uint64_t offset = 0;
    uint64_t entries_size = table_bytes - entries_off;
    uint32_t cpu_index = 0;

    while (offset + 2 <= entries_size) {
        uint8_t *base = (uint8_t*)madt->entry_table + offset;
        uint8_t entry_type = base[0];
        uint8_t entry_len  = base[1];

        if (entry_len < 2) {
            kprintf(LOG_ERROR "MADT: invalid entry_len=%u at offset=%llu (abort)\n",
                    entry_len, (unsigned long long)offset);
            break;
        }
        if (offset + entry_len > entries_size) {
            kprintf(LOG_ERROR "MADT: entry overruns table (type=%u len=%u off=%llu size=%llu) (abort)\n",
                    entry_type, entry_len,
                    (unsigned long long)offset, (unsigned long long)entries_size);
            break;
        }

        /* Type 0: Processor Local APIC (ACPI 1.0+) */
        if (entry_type == 0) {
            if (entry_len < 8) {
                kprintf(LOG_INFO "MADT: type 0 entry too short (len=%u), skipping\n", entry_len);
                offset += entry_len;
                continue;
            }

            uint8_t acpi_processor_id = base[2];
            uint8_t apic_id           = base[3];
            uint32_t flags            = *(uint32_t*)(base + 4);

            bool enabled        = (flags & 0x1u) != 0;
            bool online_capable = (flags & 0x2u) != 0;

            kprintf(LOG_INFO
                    "Found Processor Local APIC: ACPI proc ID %u, APIC ID %u, flags 0x%08x%s%s\n",
                    acpi_processor_id, apic_id, flags,
                    enabled ? " (enabled)" : " (disabled)",
                    online_capable ? " (online-capable)" : "");

            if (!enabled) {
                offset += entry_len;
                continue;
            }

            if (cpu_index < MAX_CPUS) {
                TitanCpu *cpu = &TitanBootInfo.smp_cpus[cpu_index];
                cpu->apic_id = apic_id;
                cpu->processor_id = acpi_processor_id;
                cpu->is_bsp = false; /* filled after we know BSP LAPIC ID */

                kprintf(LOG_INFO "Registered CPU %u (APIC ID: %u, ACPI ID: %u)\n",
                        cpu_index, apic_id, acpi_processor_id);

                cpu_index++;
            } else {
                kprintf(LOG_INFO "Warning: Max CPUs (%d) reached; skipping CPU with APIC ID %u\n",
                        MAX_CPUS, apic_id);
            }
        }

        /* You can add Type 9 (x2APIC) handling later if needed. */

        offset += entry_len;
    }

    TitanBootInfo.smp_info.cpu_count = cpu_index;

    /* Determine BSP by reading current LAPIC ID and matching */
    uint32_t bsp_lapic_id = lapic_get_id();
    for (uint32_t i = 0; i < cpu_index; i++) {
        TitanBootInfo.smp_cpus[i].is_bsp = (TitanBootInfo.smp_cpus[i].apic_id == bsp_lapic_id);
        if (TitanBootInfo.smp_cpus[i].is_bsp) {
            kprintf(LOG_INFO "CPU %u (APIC ID %u) is BSP (matched current LAPIC ID)\n",
                    i, TitanBootInfo.smp_cpus[i].apic_id);
        }
    }

    kprintf(LOG_OK "SMP info populated: %u CPUs registered (BSP LAPIC ID=%u)\n",
            cpu_index, bsp_lapic_id);
}

/*
 * Simple AP bring-up routine (minimal): copy the AP trampoline into low
 * physical memory and send INIT/SIPI sequence to each non-BSP CPU.
 */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_jmp_slot[];
extern uint8_t ap_trampoline_jmp_instr[];
extern uint8_t ap_trampoline_pm[];
extern uint64_t ap_trampoline_size;

extern bool x2apic_enabled;

void smp_start_aps(void)
{
    uint64_t tramp_size = (uint64_t)ap_trampoline_size;
    if (tramp_size == 0) {
        kprintf(LOG_ERROR "SMP: trampoline size is zero, aborting AP startup\n");
        return;
    }

    /* Destination physical address for trampoline (must be below 1MB and page-aligned) */
    const uint64_t dest_phys = 0x7000;
    void *dest = PHYS_TO_VIRT(dest_phys);

    kprintf(LOG_INFO "SMP: copying AP trampoline (%llu bytes) to phys 0x%llx\n",
            (unsigned long long)tramp_size, (unsigned long long)dest_phys);
    memcpy(dest, (void*)ap_trampoline_start, (size_t)tramp_size);

    /* Patch indirect far-jump pointer inside trampoline to point to physical AP PM entry */
    {
        /* Patch the short-jump target instruction area with EA immediate: 66 EA <dword target> <word selector> */
        uintptr_t rel = (uintptr_t)ap_trampoline_jmp_instr - (uintptr_t)ap_trampoline_start;
        uint8_t *instr = (uint8_t*)dest + rel;
        uint32_t target = (uint32_t)(dest_phys + ((uintptr_t)ap_trampoline_pm - (uintptr_t)ap_trampoline_start));
        kprintf(LOG_INFO "SMP: patching trampoline EA instr at offset 0x%zx -> phys 0x%08x\n", rel, target);
        instr[0] = 0x66; /* operand-size override */
        instr[1] = 0xEA; /* opcode: JMP FAR ptr16:32 (EA) */
        *(uint32_t*)(instr + 2) = target; /* 32-bit offset */
        *(uint16_t*)(instr + 6) = 0x0008; /* selector */
        /* Dump the patched instr bytes for debugging */
        kprintf(LOG_INFO "SMP: trampoline instr bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                instr[0], instr[1], instr[2], instr[3], instr[4], instr[5], instr[6], instr[7]);
    }

    /* Dump first 3 dwords of trampoline to verify copy */
    uint32_t t0 = *(volatile uint32_t*)(PHYS_TO_VIRT(dest_phys) + 0);
    uint32_t t1 = *(volatile uint32_t*)(PHYS_TO_VIRT(dest_phys) + 4);
    uint32_t t2 = *(volatile uint32_t*)(PHYS_TO_VIRT(dest_phys) + 8);
    kprintf(LOG_INFO "SMP: trampoline[0..8]=0x%08x 0x%08x 0x%08x\n", t0, t1, t2);

    uint8_t vector = (uint8_t)((dest_phys >> 12) & 0xff);

    /* Sequence: INIT (assert) -> wait 10ms -> SIPI (vector) -> wait 200us -> SIPI again */

    /* clear markers so we can detect fresh AP progress */
    volatile uint8_t *m0 = (volatile uint8_t*)PHYS_TO_VIRT(0x8000);
    volatile uint8_t *m1 = (volatile uint8_t*)PHYS_TO_VIRT(0x8001);
    volatile uint8_t *m2 = (volatile uint8_t*)PHYS_TO_VIRT(0x8002);
    *m0 = 0; *m1 = 0; *m2 = 0;

    /* Note: x2APIC can be used for INIT/SIPI, so we don't need to disable it.
       The apic_ipi() function handles both x2APIC and xAPIC modes correctly. */

    for (uint32_t i = 0; i < TitanBootInfo.smp_info.cpu_count; ++i) {
        TitanCpu *c = &TitanBootInfo.smp_cpus[i];
        if (c->is_bsp) continue;
        uint32_t apic = c->apic_id;
        kprintf(LOG_INFO "SMP: starting AP %u (APIC ID %u)\n", i, apic);

        /* Send INIT assert + deassert sequence */
        uint32_t init_lo = (5 << 8) | (1 << 14) | (1 << 15);
        apic_ipi(apic, init_lo, APIC_IPI_SINGLE);
        pit_wait(10);
        uint32_t init_deassert = (5 << 8);
        apic_ipi(apic, init_deassert, APIC_IPI_SINGLE);
        pit_wait(10);

        /* SIPI: delivery mode 6 (Startup), vector in low 8 bits */
        uint32_t sipi_lo = (6 << 8) | vector;
        kprintf(LOG_INFO "SMP: sending first SIPI to APIC %u (ICR data=0x%08x)\n", apic, sipi_lo);
        apic_ipi(apic, sipi_lo, APIC_IPI_SINGLE);

        /* Read back ICR immediately to check delivery status */
        uint32_t rlo1 = (uint32_t)apic_read(APIC_REG_ICR_LO);
        uint32_t rhi1 = (uint32_t)apic_read(APIC_REG_ICR_HI);
        kprintf(LOG_INFO "SMP: APIC ICR after first SIPI: HI=0x%08x LO=0x%08x\n", rhi1, rlo1);

        pit_wait(5); /* give AP a bit more time to start */

        kprintf(LOG_INFO "SMP: sending second SIPI to APIC %u (ICR data=0x%08x)\n", apic, sipi_lo);
        apic_ipi(apic, sipi_lo, APIC_IPI_SINGLE);

       uint32_t rlo2 = (uint32_t)apic_read(APIC_REG_ICR_LO);
        uint32_t rhi2 = (uint32_t)apic_read(APIC_REG_ICR_HI);
       // kprintf(LOG_INFO "SMP: APIC ICR after second SIPI: HI=0x%08x LO=0x%08x\n", rhi2, rlo2);

        /* Wait briefly for AP to come up and increment smp_started_count */
        uint32_t before = smp_started_count;
        bool started = false;
        for (int wait = 0; wait < 400; ++wait) { /* ~400 ms */
            pit_wait(1);
            if (smp_started_count > before) { started = true; break; }
        }
        if (started) {
            kprintf(LOG_INFO "SMP: APIC %u reported started (smp_started_count=%u)\n", apic, smp_started_count);
        } else {
            kprintf(LOG_ERROR "SMP: APIC %u did not start within timeout, trying broadcast SIPI\n", apic);
            /* Try a broadcast SIPI as fallback (all excluding self) */
            uint32_t broadcast_sipi = (6 << 8) | vector | APIC_IPI_OTHERS;
            kprintf(LOG_INFO "SMP: sending broadcast SIPI (ICR=0x%08x)\n", broadcast_sipi);
            apic_ipi(0, broadcast_sipi, APIC_IPI_SINGLE);
            /* wait briefly */
            uint32_t before2 = smp_started_count;
            for (int wait2 = 0; wait2 < 200; ++wait2) { pit_wait(1); if (smp_started_count > before2) { started = true; break; } }
            if (started) kprintf(LOG_INFO "SMP: broadcast SIPI caused AP start (smp_started_count=%u)\n", smp_started_count);
            else kprintf(LOG_ERROR "SMP: broadcast SIPI failed\n");
        }
    }
}

volatile uint32_t smp_started_count = 1;  // BSP already running

extern void enable_sse(void);
static void smp_cpu_entry(void *arg) {
    titan_mp_info_t *info = (titan_mp_info_t*)arg;
    enable_sse();
    kprintf(LOG_OK "SMP CPU started: Processor ID %u, LAPIC ID %u\n",
            info->processor_id, info->lapic_id);
            gdt_init(info->processor_id);
            kprintf(LOG_OK "SMP CPU %u GDT initialized.\n", info->processor_id);
            interrupts_reload();
    // Per-CPU setup you need:
    // - enable SSE for AP
    // - set up GDT/IDT (or load the same)
    // - init CPU-local structures
    // - start LAPIC timer, etc.

//    __atomic_add_fetch((uint32_t*)&smp_started_count, 1, __ATOMIC_SEQ_CST);

    for (;;) __asm__ volatile("hlt");
}

void smp_build_mp_info(void) {
    uint32_t n = TitanBootInfo.smp_info.cpu_count;

    for (uint32_t i = 0; i < n; i++) {
        TitanCpu *c = &TitanBootInfo.smp_cpus[i];

        TitanBootInfo.mp_info[i].processor_id   = c->processor_id;
        TitanBootInfo.mp_info[i].lapic_id       = c->apic_id;

        if (c->is_bsp) {
            TitanBootInfo.mp_info[i].goto_address   = 0;
            TitanBootInfo.mp_info[i].extra_argument = 0;
        } else {
            TitanBootInfo.mp_info[i].goto_address   = smp_cpu_entry;
            TitanBootInfo.mp_info[i].extra_argument = (uint64_t)&TitanBootInfo.mp_info[i];
        }
    }
    kprintf(LOG_OK "SMP MP info built and SMP entry points set up for %u CPUs.\n", n);
}

uint64_t apic_addr = 0;
bool x2apic_enabled = false;

void apic_init() {
	apic_addr = HIGHER_HALF(madt_apic_addr);
//	mmu_map(kernel_pagemap, apic_addr, madt_apic_addr, MAP_READ | MAP_WRITE);

	uint64_t apic_flags = cpu_read_msr(APIC_MSR);
	apic_flags |= 0x800; // Enable apic

	uint32_t a = 1, b = 0, c = 0, d = 0;
	__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a));
	if (c & APIC_FLAG_X2APIC) {
		apic_flags |= 0x400; // Enable x2apic
		x2apic_enabled = true;
		kprintf(LOG_INFO "APIC: Using X2APIC.\n");
	}

	cpu_write_msr(APIC_MSR, apic_flags);

	uint64_t spurious_int = apic_read(APIC_REG_SPURIOUS_INT);
	spurious_int |= 0x100; // Enable it
	apic_write(APIC_REG_SPURIOUS_INT, spurious_int);
	kprintf(LOG_OK "APIC Initialised.\n");
}
cpu_t *smp_this_cpu() {
    uint32_t lapic_id = apic_get_id();
    /* First try the canonical smp_cpus array */
    for (uint32_t i = 0; i < TitanBootInfo.smp_info.cpu_count; i++) {
        if (TitanBootInfo.smp_cpus[i].apic_id == lapic_id) {
            return &TitanBootInfo.smp_cpus[i];
        }
    }

    /* Fallback: try to match MP info table (sometimes mp_info is populated differently)
       and return the corresponding smp_cpus slot if available. */
    for (uint32_t i = 0; i < TitanBootInfo.smp_info.cpu_count; i++) {
        if (TitanBootInfo.mp_info[i].lapic_id == lapic_id) {
            kprintf(LOG_WARN "smp_this_cpu: matched via mp_info index %u (lapic=%u)\n", i, lapic_id);
            return &TitanBootInfo.smp_cpus[i];
        }
    }

    kprintf(LOG_WARN "smp_this_cpu: could not find CPU structure for LAPIC %u\n", lapic_id);
    return NULL;
}
void apic_eoi() {
	apic_write(APIC_REG_EOI, 0);
}

void apic_ipi(uint32_t id, uint32_t data, uint32_t type) {
	if (x2apic_enabled) {
		apic_write(APIC_REG_ICR_LO, (((uint64_t)id << 32) | data) | type);
		return;
	}
	apic_write(APIC_REG_ICR_HI, id << 24);
	apic_write(APIC_REG_ICR_LO, data | type);
}

uint32_t apic_get_id() {
	uint32_t id = apic_read(APIC_REG_ID);
	if (!x2apic_enabled) id >>= 24;
	return id;
}

void apic_timer_init() {
	apic_write(APIC_REG_DIV_CFG, 0x3);
	apic_write(APIC_REG_INIT_CNT, 0xffffffff);
	pit_wait(1); // Calibrate APIC timer to 1 ms.
	apic_write(APIC_REG_LVT_TIMER, 0x10000);
	uint64_t init_count = 0xffffffff - apic_read(APIC_REG_CURR_CNT);
	cpu_t *cpu = smp_this_cpu();
	if (!cpu) {
		kprintf(LOG_WARN "APIC: apic_timer_init: could not find cpu for LAPIC %u; skipping timer init\n", apic_get_id());
		return;
	}
	cpu->apic_timer_ticks = init_count;
}

void apic_timer_oneshot(timer_t *timer, uint64_t ms, uint8_t vec) {
	apic_write(APIC_REG_LVT_TIMER, 0x10000);
	apic_write(APIC_REG_INIT_CNT, 0);
	apic_write(APIC_REG_DIV_CFG, 0x3);
	cpu_t *cpu = smp_this_cpu();
	if (!cpu) {
		kprintf(LOG_WARN "APIC: apic_timer_oneshot: unknown cpu for LAPIC %u; ignoring oneshot\n", apic_get_id());
		return;
	}
	apic_write(APIC_REG_INIT_CNT, ms * cpu->apic_timer_ticks);
	apic_write(APIC_REG_LVT_TIMER, vec);
}

void apic_write(uint32_t reg, uint64_t value) {
	if (x2apic_enabled) {
		reg = (reg >> 4) + 0x800;
		cpu_write_msr(reg, value);
		return;
	}
	uint64_t addr = apic_addr + reg;
	*((volatile uint32_t*)addr) = (uint32_t)value;
}

uint64_t apic_read(uint32_t reg) {
	if (x2apic_enabled) {
		reg = (reg >> 4) + 0x800;
		return cpu_read_msr(reg);
	}
	uint64_t addr = apic_addr + reg;
	return *((volatile uint32_t*)addr);
}

// I/O APIC

void ioapic_init() {
	// Map every I/O APIC to memory
	for (int i = 0; i < madt_ioapic_count; i++) {
		madt_ioapic_t *ioapic = madt_ioapic_vec[i];
		uint64_t addr = ioapic->ioapic_addr;
		kprintf(LOG_INFO "I/O APIC %d found at phys 0x%08x, GSI base %d.\n", i, addr, ioapic->gsi_base);
	}
	kprintf(LOG_OK "I/O APIC Initialised.\n");
}

void ioapic_map_gsi(uint32_t apic_id, uint32_t gsi, uint8_t vec, uint32_t flags) {
	// Find I/O APIC for that GSI
	madt_ioapic_t *ioapic = NULL;
	for (int i = 0; i < madt_ioapic_count; i++) {
		ioapic = madt_ioapic_vec[i];
		if (i == madt_ioapic_count - 1) break;
		if (ioapic->gsi_base <= gsi)
			if (madt_ioapic_vec[i + 1]->gsi_base > gsi) break;
	}

	uint64_t data = (uint32_t)vec | flags;
	data |= (uint64_t)apic_id << 56;

	uint8_t reg = IOAPIC_REDIR_TABLE(gsi);

	ioapic_write(ioapic->ioapic_addr, reg, (uint32_t)data);
	ioapic_write(ioapic->ioapic_addr, reg + 1, (uint32_t)(data >> 32));
}

void ioapic_map_irq(uint32_t apic_id, uint8_t irq, uint8_t vec, bool mask) {
	madt_ioapic_iso_t *iso = NULL;
	for (int i = 0; i < madt_iso_count; i++)
		if (madt_iso_vec[i]->irq_src == irq) {
			iso = madt_iso_vec[i];
			break;
		}

	if (iso == NULL) {
		ioapic_map_gsi(apic_id, irq, vec, (mask ? 1 << 16 : 0));
		return;
	}

	uint32_t flags = 0;
	if (iso->flags & (1 << 1)) flags |= 1 << 13; // Polarity
	if (iso->flags & (1 << 3)) flags |= 1 << 15; // Trigger mode
	if (mask) flags |= (1 << 16);

	ioapic_map_gsi(apic_id, iso->gsi, vec, flags);
}

void ioapic_write(uint32_t base, uint8_t reg, uint32_t data) {
	uint64_t addr = HIGHER_HALF((uint64_t)base);
	*(volatile uint32_t*)addr = reg;
	*(volatile uint32_t*)(addr + 0x10) = data;
}

uint32_t ioapic_read(uint32_t base, uint8_t reg) {
	uint64_t addr = HIGHER_HALF((uint64_t)base);
	*(volatile uint32_t*)addr = reg;
	return *(volatile uint32_t*)(addr + 0x10);
}
