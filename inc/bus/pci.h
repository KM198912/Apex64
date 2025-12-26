#pragma once
#include <stdint.h>

#define PCI_MAX_DEVICES 256

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint64_t bar[6];
    uint64_t bar_size[6];
    uint8_t bar_is_io[6];
    /* Virtual address for MMIO BARs (0 if not mapped) */
    uint64_t bar_virt[6];
};

/* Driver registry: register probe by class/subclass (probes receive a pci_device*) */
typedef int (*pci_probe_fn)(struct pci_device* dev);
int pci_register_class_driver(uint8_t class, uint8_t subclass, pci_probe_fn probe);

/* Probe devices (call registered drivers) */
void pci_probe_devices(void);

/* Register built-in drivers (call from kernel init) */
void pci_register_builtin_drivers(void);

/* Map device BARs into kernel address space (returns 0 on success) */
int pci_map_device_bars(struct pci_device* dev);

/* Initialize PCI subsystem and enumerate devices on bus 0.. */
void pci_init(void);

/* Return number of discovered devices */
int pci_get_device_count(void);

/* Get pointer to device at index (0..pci_get_device_count()-1) */
struct pci_device* pci_get_device(int idx);

/* Read config space (32-bit) */
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

/* Convenience helpers: read/write smaller sizes or named widths */
uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_read_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t  pci_read_config_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void     pci_write_config_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);

/* higher-level helpers */
uint16_t pci_read_vendor(uint8_t bus, uint8_t device, uint8_t function);
uint16_t pci_read_device(uint8_t bus, uint8_t device, uint8_t function);

/* Print discovered devices */
void pci_print_devices(void);
