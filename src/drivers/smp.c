#include <common/boot.h>
#include <drivers/acpi.h>
#include <drivers/smp.h>
#include <kernel/kprintf.h>
#include <drivers/gdt.h>
#include <drivers/idt.h>
#include <lib/sys/io.h>
#include <lib/debug.h>

/* enable_sse() is defined in entry.c; declare here for per-AP setup */
extern void enable_sse(void);

/* AP entrypoint called from trampoline. It will look up this CPU's mp_info by
 * LAPIC ID and invoke the configured `goto_address` with the provided argument.
 */
void ap_entry(void)
{
    uint32_t lapic = apic_get_id();

    /* Signal that we reached AP entry (increment counter) */
    __atomic_add_fetch((uint32_t*)&smp_started_count, 1, __ATOMIC_SEQ_CST);

    /* Early serial out to QEMU debug port 0xE9 so we can see progress even in VBE mode */
    outb(QEMU_DEBUG_PORT, (uint8_t)('A' + (lapic % 10)));

    kprintf(LOG_INFO "AP: entry (LAPIC ID=%u) smp_started_count=%u\n", lapic, smp_started_count);

    /* Find matching MP info */
    for (uint32_t i = 0; i < TitanBootInfo.smp_info.cpu_count; ++i) {
        titan_mp_info_t *info = &TitanBootInfo.mp_info[i];
        if (info->lapic_id != lapic) continue;

        kprintf(LOG_INFO "AP: found MP info at index %u\n", i);

        /* Perform minimal per-CPU setup: enable SSE, initialize GDT/IDT for this CPU */
        enable_sse();

        /* If processor_id is valid, initialize GDT/TSS for this CPU index */
        if (info->processor_id < MAX_CPUS) {
            gdt_init(info->processor_id);
            kprintf(LOG_OK "AP %u: GDT initialized for processor %u\n", info->processor_id, info->processor_id);
        }

        if (info->goto_address) {
            kprintf(LOG_INFO "AP %u: calling goto_address %p (arg=%p)\n",
                    info->processor_id, info->goto_address, (void*)(uintptr_t)info->extra_argument);
            info->goto_address((void*)(uintptr_t)info->extra_argument);
            /* If the handler returns we intentionally halt the AP */
            kprintf(LOG_ERROR "AP %u: goto_address returned; halting\n", info->processor_id);
        } else {
            kprintf(LOG_ERROR "AP %u: No goto_address set; halting\n", info->processor_id);
        }
        interrupts_reload();
        kprintf(LOG_OK "AP %u: IDT reloaded.\n", info->processor_id);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    kprintf(LOG_ERROR "AP: could not find MP info for LAPIC ID %u; halting\n", lapic);
    for (;;) __asm__ volatile ("cli; hlt");
}
