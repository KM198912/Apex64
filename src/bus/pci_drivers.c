#include <bus/pci.h>
#include <kernel/kprintf.h>

#include <drivers/ahci.h>
#include <drivers/ide.h>

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

/* Register AHCI driver (call from kernel init) */
void pci_register_builtin_drivers(void)
{
    pci_register_class_driver(0x01, 0x06, ahci_probe);
    pci_register_class_driver(0x01, 0x01, ide_probe);
}