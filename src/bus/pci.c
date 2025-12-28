#include <bus/pci.h>
#include <lib/sys/io.h>
#include <kernel/kprintf.h>
#include <lib/string.h>
#include <common/boot.h>
#include <mem/vmm.h>

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count = 0;

/* Driver registry */
#define MAX_PCI_DRIVERS 32
struct pci_driver_entry { uint8_t class, subclass; pci_probe_fn probe; };
static struct pci_driver_entry pci_drivers[MAX_PCI_DRIVERS];
static int pci_driver_count = 0;

int pci_register_class_driver(uint8_t class, uint8_t subclass, pci_probe_fn probe)
{
    if (pci_driver_count >= MAX_PCI_DRIVERS) return -1;
    pci_drivers[pci_driver_count].class = class;
    pci_drivers[pci_driver_count].subclass = subclass;
    pci_drivers[pci_driver_count].probe = probe;
    pci_driver_count++;
    return 0;
}

/* Device-specific drivers (vendor/device matching) */
#define MAX_PCI_DEVICE_DRIVERS 32
struct pci_device_driver_entry { uint16_t vendor, device; pci_probe_fn probe; };
static struct pci_device_driver_entry pci_device_drivers[MAX_PCI_DEVICE_DRIVERS];
static int pci_device_driver_count = 0;

int pci_register_device_driver(uint16_t vendor, uint16_t device, pci_probe_fn probe)
{
    if (pci_device_driver_count >= MAX_PCI_DEVICE_DRIVERS) return -1;
    pci_device_drivers[pci_device_driver_count].vendor = vendor;
    pci_device_drivers[pci_device_driver_count].device = device;
    pci_device_drivers[pci_device_driver_count].probe  = probe;
    pci_device_driver_count++;
    return 0;
}

void pci_probe_devices(void)
{
    for (int d = 0; d < device_count; ++d) {
        struct pci_device *dev = &devices[d];

        /* Check device-specific drivers first (they take precedence). If a device driver
           returns 0 (success), we skip class-based probing for that device. If it
           returns non-zero, the device is considered unhandled and class drivers may run. */
        bool handled = false;
        for (int i = 0; i < pci_device_driver_count; ++i) {
            if (pci_device_drivers[i].vendor == dev->vendor_id &&
                (pci_device_drivers[i].device == PCI_DEVICE_ANY || pci_device_drivers[i].device == dev->device_id)) {
                if (pci_device_drivers[i].probe) {
                    int r = pci_device_drivers[i].probe(dev);
                    kprintf("pci: device-probe result=%d for %04x:%04x at %02x:%02x.%x\n",
                            r, dev->vendor_id, dev->device_id, dev->bus, dev->device, dev->function);
                    if (r == 0) { handled = true; break; }
                }
            }
        }
        if (handled) continue;

        /* Fallback to class-based drivers */
        for (int i = 0; i < pci_driver_count; ++i) {
            if (pci_drivers[i].class == dev->class_code &&
                (pci_drivers[i].subclass == 0xFF || pci_drivers[i].subclass == dev->subclass)) {
                if (pci_drivers[i].probe) {
                    int r = pci_drivers[i].probe(dev);
                    kprintf("pci: class-probe result=%d for %02x:%02x.%x\n", r, dev->bus, dev->device, dev->function);
                }
            }
        }
    }
}

/* Simple MMIO allocator and mapper: maps BAR physical pages into a kernel VA range
   starting just after the kernel image. This ensures PTEs exist even if HHDM
   mapping does not cover the device's BAR window. */
static uint64_t mmio_alloc_ptr = 0;
static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + (a-1)) & ~(a-1); }
static uint64_t map_mmio(uint64_t phys, uint64_t size)
{
    if (size == 0) return 0;
    uint64_t phys_base = phys & ~0xFFFULL;
    uint64_t offset = phys & 0xFFFULL;
    uint64_t map_size = align_up(size + offset, 0x1000);

    if (!mmio_alloc_ptr) {
        extern char _kernel_end;
        uint64_t base = align_up((uint64_t)&_kernel_end, 0x1000);
        /* leave a small gap after kernel for safety */
        mmio_alloc_ptr = base + 0x200000; /* +2MiB */
    }

    uint64_t virt_base = align_up(mmio_alloc_ptr, 0x1000);
    mmio_alloc_ptr = virt_base + map_size;

    for (uint64_t off = 0; off < map_size; off += 0x1000) {
        uint64_t v = virt_base + off;
        uint64_t p = phys_base + off;
        int r = vmm_map_page(v, p, VMM_PTE_W);
        if (r < 0) {
            kprintf(LOG_ERROR "pci: vmm_map_page failed virt=0x%llx phys=0x%llx\n", (unsigned long long)v, (unsigned long long)p);
            return 0;
        }
    }

    return virt_base + offset;
}

int pci_map_device_bars(struct pci_device* dev)
{
    for (int b = 0; b < 6; ++b) {
        dev->bar_virt[b] = 0;
        if (dev->bar_size[b] == 0) continue;
        if (dev->bar_is_io[b]) continue; /* IO not mapped here */
        uint64_t phys = dev->bar[b];
        uint64_t size = dev->bar_size[b];

        /* Try HHDM first if within low 4GiB and HHDM likely covers it */
        if (phys < 0x100000000ULL) {
            dev->bar_virt[b] = (uint64_t)PHYS_TO_VIRT(phys);
            /* Validate translation exists */
            if (vmm_translate(dev->bar_virt[b]) == 0) {
                dev->bar_virt[b] = 0;
            }
        }

        /* If HHDM not usable, map pages explicitly into kernel VA space */
        if (!dev->bar_virt[b]) {
            dev->bar_virt[b] = map_mmio(phys, size);
            if (dev->bar_virt[b]) kprintf(LOG_INFO "pci: mapped BAR%d phys=0x%llx -> virt=0x%llx size=0x%llx\n",
                                          b, (unsigned long long)phys, (unsigned long long)dev->bar_virt[b], (unsigned long long)size);
        }
    }
    return 0;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t addr = (uint32_t)((1u << 31) |
                                ((uint32_t)bus << 16) |
                                ((uint32_t)device << 11) |
                                ((uint32_t)function << 8) |
                                (offset & 0xFC));
    outl(0xCF8, addr);
    uint32_t val = inl(0xCFC);
    return val;
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value)
{
    uint32_t addr = (uint32_t)((1u << 31) |
                                ((uint32_t)bus << 16) |
                                ((uint32_t)device << 11) |
                                ((uint32_t)function << 8) |
                                (offset & 0xFC));
    outl(0xCF8, addr);
    outl(0xCFC, value);
}

uint16_t pci_read_vendor(uint8_t bus, uint8_t device, uint8_t function)
{
    uint32_t v = pci_config_read32(bus, device, function, 0x00);
    return (uint16_t)(v & 0xFFFF);
}

uint16_t pci_read_device(uint8_t bus, uint8_t device, uint8_t function)
{
    uint32_t v = pci_config_read32(bus, device, function, 0x00);
    return (uint16_t)((v >> 16) & 0xFFFF);
}

/* Convenience wrappers */
uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return pci_config_read32(bus, device, function, offset);
}

uint16_t pci_read_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    /* read aligned dword and extract */
    uint32_t v = pci_config_read32(bus, device, function, offset & ~3);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read_config_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, device, function, offset & ~3);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value)
{
    uint8_t aligned = offset & ~3;
    uint32_t orig = pci_config_read32(bus, device, function, aligned);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t newv = (orig & ~mask) | ((uint32_t)value << shift);
    pci_config_write32(bus, device, function, aligned, newv);
}

void pci_write_config_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    uint8_t aligned = offset & ~3;
    uint32_t orig = pci_config_read32(bus, device, function, aligned);
    uint32_t shift = (offset & 3) * 8;
    uint32_t mask = 0xFFu << shift;
    uint32_t newv = (orig & ~mask) | ((uint32_t)value << shift);
    pci_config_write32(bus, device, function, aligned, newv);
}

/* helpers: read 8/16 */
static inline uint8_t pci_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, device, function, offset & ~3);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}
static inline uint16_t pci_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, device, function, offset & ~3);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

/* BAR probing: returns original value in bar, size written back via out_size. is_io set for IO BARs. */
static uint64_t pci_probe_bar(uint8_t bus, uint8_t device, uint8_t function, int bar_idx, uint8_t *is_io, uint64_t *out_size)
{
    uint8_t offset = 0x10 + bar_idx * 4;
    uint32_t orig = pci_config_read32(bus, device, function, offset);
    if (orig == 0) {
        *is_io = 0;
        *out_size = 0;
        return 0;
    }
    if (orig & 1) {
        /* IO BAR */
        *is_io = 1;
        uint32_t saved = orig;
        pci_config_write32(bus, device, function, offset, 0xFFFFFFFF);
        uint32_t sz = pci_config_read32(bus, device, function, offset) & 0xFFFFFFFC;
        pci_config_write32(bus, device, function, offset, saved);
        uint64_t size = (~(uint64_t)sz) + 1ULL;
        *out_size = size;
        return (uint64_t)(saved & 0xFFFFFFFC);
    } else {
        /* Memory BAR */
        *is_io = 0;
        uint32_t type = (orig >> 1) & 0x3; /* 00=32, 10=64 */
        if (type == 0x2) {
            /* 64-bit BAR: combine with next BAR */
            uint32_t orig_hi = pci_config_read32(bus, device, function, offset + 4);
            uint64_t full_orig = ((uint64_t)orig_hi << 32) | (orig & 0xFFFFFFF0);
            /* write all ones to low and high */
            pci_config_write32(bus, device, function, offset, 0xFFFFFFFF);
            pci_config_write32(bus, device, function, offset + 4, 0xFFFFFFFF);
            uint32_t sz_lo = pci_config_read32(bus, device, function, offset) & 0xFFFFFFF0;
            uint32_t sz_hi = pci_config_read32(bus, device, function, offset + 4);
            uint64_t mask = ((uint64_t)sz_hi << 32) | sz_lo;
            pci_config_write32(bus, device, function, offset, (uint32_t)(orig & 0xFFFFFFFF));
            pci_config_write32(bus, device, function, offset + 4, orig_hi);
            uint64_t size = (~mask) + 1ULL;
            *out_size = size;
            return full_orig;
        } else {
            uint32_t saved = orig;
            pci_config_write32(bus, device, function, offset, 0xFFFFFFFF);
            uint32_t sz = pci_config_read32(bus, device, function, offset) & 0xFFFFFFF0;
            pci_config_write32(bus, device, function, offset, saved);
            uint64_t size = (~(uint64_t)sz) + 1ULL;
            *out_size = size;
            return (uint64_t)(saved & 0xFFFFFFF0);
        }
    }
}

static bool scanned_buses[256];

static void scan_bus(uint8_t bus)
{
    if (bus >= 256 || scanned_buses[bus]) return;
    scanned_buses[bus] = true;

    for (uint8_t dev = 0; dev < 32; ++dev) {
        uint16_t vendor = pci_read_vendor(bus, dev, 0);
        if (vendor == 0xFFFF) continue;
        /* determine multifunction */
        uint8_t header_type = pci_read8(bus, dev, 0, 0x0E);
        int max_func = (header_type & 0x80) ? 8 : 1;
        for (int fn = 0; fn < max_func; ++fn) {
            vendor = pci_read_vendor(bus, dev, fn);
            if (vendor == 0xFFFF) continue;
            uint16_t device_id = pci_read_device(bus, dev, fn);
            uint32_t cls = pci_config_read32(bus, dev, fn, 0x08);
            uint8_t class_code = (cls >> 24) & 0xFF;
            uint8_t subclass = (cls >> 16) & 0xFF;
            uint8_t prog_if = (cls >> 8) & 0xFF;

            struct pci_device *pd = &devices[device_count];
            pd->bus = bus; pd->device = dev; pd->function = fn;
            pd->vendor_id = vendor; pd->device_id = device_id;
            pd->class_code = class_code; pd->subclass = subclass; pd->prog_if = prog_if;
            pd->header_type = header_type;
            for (int b = 0; b < 6; ++b) {
                uint8_t is_io = 0; uint64_t sz = 0;
                uint64_t addr = pci_probe_bar(bus, dev, fn, b, &is_io, &sz);
                pd->bar[b] = addr;
                pd->bar_size[b] = sz;
                pd->bar_is_io[b] = is_io;
                pd->bar_virt[b] = 0;
            }

            ++device_count;
            if (device_count >= PCI_MAX_DEVICES) return;

            /* If this device is a PCI-to-PCI bridge, scan its secondary bus */
            if (class_code == 0x06 && subclass == 0x04) {
                uint8_t sec = pci_read_config_byte(bus, dev, fn, 0x19);
                if (sec != 0 && sec != bus && !scanned_buses[sec]) {
                    scan_bus(sec);
                    if (device_count >= PCI_MAX_DEVICES) return;
                }
            }
        }
    }
}

void pci_init(void)
{
    device_count = 0;
    memset(scanned_buses, 0, sizeof(scanned_buses));
    scan_bus(0);
}

int pci_get_device_count(void) { return device_count; }
struct pci_device* pci_get_device(int idx) { if (idx < 0 || idx >= device_count) return NULL; return &devices[idx]; }

static const char *pci_class_name(uint8_t class, uint8_t subclass, uint8_t prog_if)
{
    switch (class) {
        case 0x00: return "Unclassified";
        case 0x01:
            if (subclass == 0x06) return (prog_if == 0x01) ? "AHCI (SATA)" : "Mass storage controller";
            if (subclass == 0x08) return "NVM Express";
            if (subclass == 0x01) return "IDE controller";
            return "Mass storage controller";
        case 0x02: return "Network controller";
        case 0x03:
            if (subclass == 0x00) return "VGA-compatible controller";
            return "Display controller";
        case 0x04: return "Multimedia controller";
        case 0x06: return "Bridge device";
        case 0x0C:
            if (subclass == 0x03) return "USB controller";
            return "Serial bus controller";
        default: return "Unknown";
    }
}

void pci_print_devices(void)
{
    kprintf("PCI devices: %d\n", device_count);
    for (int i = 0; i < device_count; ++i) {
        struct pci_device *d = &devices[i];
        const char *type = pci_class_name(d->class_code, d->subclass, d->prog_if);
        kprintf("[%02d] %02x:%02x.%x %s vendor=0x%04x device=0x%04x\n",
                i, d->bus, d->device, d->function, type, d->vendor_id, d->device_id);
                //print class, subclass, prog_if
        kprintf("      class=0x%02x subclass=0x%02x prog_if=0x%02x\n",
                d->class_code, d->subclass, d->prog_if);
    
    }
}

