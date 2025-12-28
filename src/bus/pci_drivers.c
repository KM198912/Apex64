#include <bus/pci.h>
#include <kernel/kprintf.h>

#include <drivers/ahci.h>
#include <drivers/ide.h>
#include <common/boot.h>
#include <lib/sys/io.h>
#include <drivers/idt.h>
#include <drivers/rtl8139.h>
/* AHCI registers (offsets) */
#define AHCI_CAP   0x00
#define AHCI_GHC   0x04
#define AHCI_IS    0x08
#define AHCI_PI    0x0C
#define AHCI_VS    0x10

/* AHCI probe: map ABAR (virtual) and print basic registers */
static int ahci_probe(struct pci_device* dev)
{
    if (dev->class_code != 0x01 || dev->subclass != 0x06) return -1;
    return ahci_attach(dev);
}

static int ide_probe(struct pci_device* dev)
{
    if (dev->class_code != 0x01 || dev->subclass != 0x01) return -1;
    return ide_attach(dev);
}

typedef struct {
    volatile uint32_t reserved1[1];
    volatile uint32_t eeprom_offset;   // 0x04
    volatile uint32_t eeprom_cmd;      // 0x08
    volatile uint32_t eeprom_data;     // 0x0C
    volatile uint32_t reserved2[52];
    volatile uint32_t mac_addr_low;    // 0x100
    volatile uint32_t mac_addr_high;   // 0x104
} ath_regs_t;

static volatile ath_regs_t *ath = NULL;
#define AR_EEPROM_OFFSET 0x04
#define AR_EEPROM_CMD    0x08
#define AR_EEPROM_DATA   0x0C

static uint16_t ath_eeprom_read(uint16_t off)
{
    /* Access the MMIO registers via the struct fields, not by
     * indexing the struct as an array. */
    ath->eeprom_offset = off;
    ath->eeprom_cmd = 1;            // start read
    while (ath->eeprom_cmd & 1)     // wait
        ;
    return (uint16_t)(ath->eeprom_data & 0xffff);
}

/* Diagnostics: walk upstream bridges (PCI-to-PCI) that lead to target_bus and print
   their IO/MEM/PREF windows so we can verify whether `bar_phys` is routed. */
static void dump_bridge_windows_for_bus(uint8_t target_bus, uint64_t bar_phys)
{
    uint8_t cur_bus = target_bus;
    kprintf(LOG_INFO "pci: walking upstream bridges for bus %u (bar_phys=0x%llx)\n", cur_bus, (unsigned long long)bar_phys);
    while (cur_bus != 0) {
        bool found = false;
        for (int i = 0; i < pci_get_device_count(); ++i) {
            struct pci_device *bd = pci_get_device(i);
            if (!bd) continue;
            if (bd->class_code != 0x06 || bd->subclass != 0x04) continue; /* not a bridge */
            uint8_t sec = pci_read_config_byte(bd->bus, bd->device, bd->function, 0x19);
            if (sec != cur_bus) continue;
            found = true;
            uint8_t prim = pci_read_config_byte(bd->bus, bd->device, bd->function, 0x18);
            uint8_t sub  = pci_read_config_byte(bd->bus, bd->device, bd->function, 0x1A);
            uint16_t io_base  = pci_read_config_word(bd->bus, bd->device, bd->function, 0x1C);
            uint16_t io_limit = pci_read_config_word(bd->bus, bd->device, bd->function, 0x1D);
            uint16_t mem_base = pci_read_config_word(bd->bus, bd->device, bd->function, 0x20);
            uint16_t mem_limit = pci_read_config_word(bd->bus, bd->device, bd->function, 0x22);
            uint32_t pref_base_lo = pci_read_config_dword(bd->bus, bd->device, bd->function, 0x24);
            uint32_t pref_limit_lo = pci_read_config_dword(bd->bus, bd->device, bd->function, 0x28);

            kprintf(LOG_INFO " pci-bridge %02x:%02x.%x prim=%u sec=%u sub=%u\n",
                    bd->bus, bd->device, bd->function, prim, sec, sub);
            kprintf(LOG_INFO "  IO base=0x%04x limit=0x%04x\n", io_base, io_limit);

            if (mem_base || mem_limit) {
                uint64_t mb = ((uint64_t)(mem_base & 0xFFF0)) << 16;
                uint64_t ml = (((uint64_t)(mem_limit & 0xFFF0)) << 16) | 0xFFFFF;
                kprintf(LOG_INFO "  MEM base=0x%llx limit=0x%llx\n", (unsigned long long)mb, (unsigned long long)ml);
                if (bar_phys >= mb && bar_phys <= ml) kprintf(LOG_OK "   -> BAR phys 0x%llx is inside bridge MEM window\n", (unsigned long long)bar_phys);
            } else {
                kprintf(LOG_INFO "  MEM base/limit not programmed (raw base=0x%04x limit=0x%04x)\n", mem_base, mem_limit);
            }

            if (pref_base_lo || pref_limit_lo) {
                kprintf(LOG_INFO "  PREF base_lo=0x%08x limit_lo=0x%08x\n", pref_base_lo, pref_limit_lo);
                /* Upper 32-bit parts available at 0x28/0x2C on some bridges; skip for now */
            }

            cur_bus = prim; /* continue upward */
            break;
        }
        if (!found) {
            kprintf(LOG_INFO " pci: no upstream bridge found for bus %u (stopping)\n", cur_bus);
            break;
        }
    }
}

static int probe_168c_002e(struct pci_device* dev)
{
    if (dev->vendor_id != 0x168c || dev->device_id != 0x002e) return -1;
    kprintf(LOG_INFO "pci: matched specific device 168c:002e at %02x:%02x.%x\n",
            dev->bus, dev->device, dev->function);

    /* Print BAR / PCI state */
    uint64_t phys_bar0 = dev->bar[0];
    uint64_t bar0_size = dev->bar_size[0];
    int is_io = dev->bar_is_io[0];
    kprintf(LOG_INFO "pci: BAR0 phys=0x%llx size=0x%llx is_io=%d virt=0x%llx\n",
            (unsigned long long)phys_bar0, (unsigned long long)bar0_size, is_io, (unsigned long long)dev->bar_virt[0]);

    dump_bridge_windows_for_bus(dev->bus, phys_bar0);

    /* Enable Memory Space + Bus Mastering and show resulting command register */
    uint16_t cmd = pci_read_config_word(dev->bus, dev->device, dev->function, 0x04);
    kprintf(LOG_INFO "pci: before enable cmd=0x%04x\n", cmd);
    if (!(cmd & (1<<1)) || !(cmd & (1<<2))) {
        cmd |= (1<<1);
        cmd |= (1<<2);
        pci_write_config_word(dev->bus, dev->device, dev->function, 0x04, cmd);
    }
    cmd = pci_read_config_word(dev->bus, dev->device, dev->function, 0x04);
    kprintf(LOG_INFO "pci: after enable cmd=0x%04x\n", cmd);

    /* Capability scan: check power state and PCIe capabilities */
    uint16_t status = pci_read_config_word(dev->bus, dev->device, dev->function, 0x06);
    kprintf(LOG_INFO "pci: status=0x%04x\n", status);
    uint8_t pm_cap_ptr = 0;
    if (status & (1<<4)) { /* Capabilities list present */
        uint8_t cap = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x34);
        kprintf(LOG_INFO "pci: capabilities list starts at 0x%02x\n", cap);
        int iter = 0;
        while (cap && iter++ < 32) {
            uint8_t cid = pci_read_config_byte(dev->bus, dev->device, dev->function, cap);
            uint8_t next = pci_read_config_byte(dev->bus, dev->device, dev->function, cap + 1);
            kprintf(LOG_INFO " pci cap @0x%02x id=0x%02x next=0x%02x\n", cap, cid, next);
            if (cid == 0x01) {
                pm_cap_ptr = cap;
                uint16_t pmcsr = pci_read_config_word(dev->bus, dev->device, dev->function, cap + 4);
                kprintf(LOG_INFO " pci: PMCSR @0x%02x = 0x%04x (power state = D%u)\n", cap + 4, pmcsr, pmcsr & 3);
                /* If not in D0, try to set D0 */
                if ((pmcsr & 3) != 0) {
                    kprintf(LOG_INFO " pci: attempting to set power state to D0\n");
                    uint16_t new_pmcsr = (pmcsr & ~3) | 0;
                    pci_write_config_word(dev->bus, dev->device, dev->function, cap + 4, new_pmcsr);
                    uint16_t pmcsr2 = pci_read_config_word(dev->bus, dev->device, dev->function, cap + 4);
                    kprintf(LOG_INFO " pci: PMCSR after write = 0x%04x (power state = D%u)\n", pmcsr2, pmcsr2 & 3);
                    /* small delay to allow device to wake */
                    pit_wait(50);
                }
            }
            cap = next;
        }
    } else {
        kprintf(LOG_INFO "pci: no capability list reported in status\n");
    }

    /* Ensure MMIO mapping is present; attempt mapping if missing */
    if (is_io) {
        kprintf(LOG_ERROR "pci: device BAR0 is IO, expected MMIO; aborting device-specific probe.\n");
        return -1;
    }

    if (!dev->bar_virt[0]) {
        pci_map_device_bars(dev);
        kprintf(LOG_INFO "pci: attempted to map BARs; dev->bar_virt[0]=0x%llx\n", (unsigned long long)dev->bar_virt[0]);
        if (!dev->bar_virt[0]) {
            /* Fallback: if phys_bar0 is within the low 4GiB HHDM, try PHYS_TO_VIRT */
            if (phys_bar0 && phys_bar0 < 0x100000000ULL) {
                dev->bar_virt[0] = (uint64_t)PHYS_TO_VIRT(phys_bar0);
                kprintf(LOG_INFO "pci: fallback PHYS_TO_VIRT -> 0x%llx\n", (unsigned long long)dev->bar_virt[0]);
            }
        }
    }

    if (!dev->bar_virt[0]) {
        kprintf(LOG_ERROR "pci: cannot obtain virtual mapping for BAR0; aborting probe.\n");
        return -1;
    }
    volatile uint32_t *mmio = (volatile uint32_t*)(uintptr_t)dev->bar_virt[0];

    /* Dump first few dwords to help diagnose read issues */
    size_t dump_words = 16;
    if (bar0_size) {
        size_t max_words = bar0_size / 4;
        if (max_words < dump_words) dump_words = max_words;
    }
    kprintf(LOG_INFO "pci: dumping first %zu dwords of MMIO @ virt=0x%llx\n", dump_words, (unsigned long long)dev->bar_virt[0]);
    for (size_t i = 0; i < dump_words; ++i) {
        uint32_t v = mmio[i];
        kprintf("  [%02zu*4]=0x%08x\n", i, v);
    }

    /* Read SREV (AR_SREV at offset 0x40) */
    uint32_t srev = mmio[0x40 / 4];
    kprintf("ath9k: SREV = 0x%08x\n", srev);

    /* If SREV looks like a sentinel (e.g., 0xDEADBEEF or 0xFFFFFFFF), warn */
    if (srev == 0xDEADBEEF || srev == 0xFFFFFFFF) {
        kprintf(LOG_ERROR "ath9k: SREV appears invalid (0x%08x) - device may need reset or clocks\n", srev);
    }

    return -1;
}

//function to read e1000 device eeprom and get mac address


static int find_pci_cap(struct pci_device* dev, uint8_t capid)
{
    uint16_t status = pci_read_config_word(dev->bus, dev->device, dev->function, 0x06);
    if (!(status & (1<<4))) return 0; /* no capabilities list */
    uint8_t cap = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x34);
    int iter = 0;
    while (cap && iter++ < 32) {
        uint8_t cid = pci_read_config_byte(dev->bus, dev->device, dev->function, cap);
        if (cid == capid) return cap;
        cap = pci_read_config_byte(dev->bus, dev->device, dev->function, cap + 1);
    }
    return 0;
}



/* Register AHCI driver (call from kernel init) */
void pci_register_builtin_drivers(void)
{

    pci_register_class_driver(0x01, 0x06, ahci_probe);
    pci_register_class_driver(0x01, 0x01, ide_probe);
}

void pci_register_extra_drivers(void)
{
    /* Device-specific registration (takes precedence when probe returns 0) */
    pci_register_device_driver(0x168c, 0x002e, probe_168c_002e);
    rtl8139_register();
    /* Register Atheros AR8151 minimal driver */
    ath_register();
}