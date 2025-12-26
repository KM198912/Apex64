#pragma once
#include <stdint.h>
#include <stddef.h>
#include <bus/pci.h>

/* Attach AHCI driver to a PCI device; returns 0 on success */
int ahci_attach(struct pci_device *dev);

/* Minimal structure representing an attached AHCI device/port */
struct ahci_port_info {
    struct pci_device *pdev;
    int port_num;
    uint64_t abar_phys;
    void *abar_virt;
};

/* Read sectors from a device on AHCI port. Supports up to 8 sectors (4096 bytes)
 * per call using the per-port page buffer. `count` is sector count. `out_len`
 * must be >= count*512. Returns 0 on success, -1 on error. */
int ahci_read(uintptr_t abar, int port, uint64_t lba, uint16_t count, void* out_buf, size_t out_len);

