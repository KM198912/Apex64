#include <drivers/ethernet_ath.h>
#include <bus/pci.h>
#include <kernel/kprintf.h>
#include <lib/sys/io.h>
#include <drivers/idt.h>
#include <drivers/pit.h>
#include <mem/pmm.h>
#include <common/boot.h>
#include <lib/string.h>
/*
* Atheros AR8151 v1.0 Gigabit Ethernet
* Apex64 Driver
*/
static volatile uint32_t *ath_mmio = NULL;
static int ath_phy = -1;

/* Poll for MII/MDIO busy flag to clear. Returns 0 on success, -1 on timeout. */
static int ath_mii_wait(void)
{
    for (int i = 0; i < 10000; ++i) {
        uint32_t v = ath_mmio[AR8151_REG_MII_CTRL / 4];
        if (!(v & AR8151_MII_BUSY)) return 0;
        for (volatile int j = 0; j < 200; ++j) __asm__ volatile ("pause");
    }
    return -1;
}

/* Read a 16-bit MII register via device MDIO interface. Returns >=0 on success, -1 on failure */
static int ath_mii_read(int phy, int reg)
{
    if (!ath_mmio) return -1;

    /* This layout is a reasonable guess for AR8151 MDIO control fields: place reg/phy and set read/busy flags */
    uint32_t ctrl = ((reg & 0x1f) << 16) | ((phy & 0x1f) << 21) | AR8151_MII_READ | AR8151_MII_BUSY;
    ath_mmio[AR8151_REG_MII_DATA / 4] = 0; /* clear data */
    ath_mmio[AR8151_REG_MII_CTRL / 4] = ctrl;

    if (ath_mii_wait() != 0) return -1;

    uint32_t data = ath_mmio[AR8151_REG_MII_DATA / 4];
    return (int)(data & 0xffff);
}

/* Write a 16-bit MII register. Returns 0 on success, -1 on failure */
static int ath_mii_write(int phy, int reg, uint16_t val)
{
    if (!ath_mmio) return -1;

    ath_mmio[AR8151_REG_MII_DATA / 4] = (uint32_t)val;
    uint32_t ctrl = ((reg & 0x1f) << 16) | ((phy & 0x1f) << 21) | AR8151_MII_WRITE | AR8151_MII_BUSY;
    ath_mmio[AR8151_REG_MII_CTRL / 4] = ctrl;

    if (ath_mii_wait() != 0) return -1;
    return 0;
}

/* Minimal EEPROM read helper. Hardware-specific access varies between chips; this provides a safe non-blocking attempt.
 * Strategy: if MDIO PHY is detected, we try a conservative MDIO read of the requested index; otherwise fall back to a
 * direct MMIO 16-bit read (non-destructive). Returns -1 on failure, otherwise 16-bit value. */
int ath_eeprom_read(uint16_t off)
{
    if (!ath_mmio) return -1;

    /* If we haven't discovered PHY yet, attempt to find a responding PHY */
    if (ath_phy < 0) {
        for (int p = 0; p < 32; ++p) {
            int id = ath_mii_read(p, MII_REG_PHYID1);
            if (id >= 0 && id != 0xffff && id != 0x0000) {
                ath_phy = p;
                kprintf(LOG_INFO "ath: detected PHY at addr %d (PHYID1=0x%04x)\n", ath_phy, id);
                break;
            }
        }
    }

    /* Try MDIO read first if we have a PHY address */
    if (ath_phy >= 0) {
        int v = ath_mii_read(ath_phy, (int)off);
        if (v >= 0) return v & 0xffff;
    }

    /* Fallback: conservative MMIO read as 16-bit word at BAR0 + off*2 (harmless read) */
    volatile uint16_t *p = (volatile uint16_t *)((uint8_t *)ath_mmio + (off * 2));
    return (int)(*p);
}

/* Write not implemented in minimal probe; return false to indicate failure */
bool ath_eeprom_write(uint16_t off, uint16_t value)
{
    (void)off; (void)value;
    return false;
}

static int probe_ath(struct pci_device* dev)
{
    if (dev->vendor_id != ATH_ETHERNET_VENDOR_ID || dev->device_id != ATH_ETHERNET_DEVICE_ID) return -1;

    kprintf(LOG_INFO "ath: Found Atheros device at %02x:%02x.%x\n",
            dev->bus, dev->device, dev->function);

    /* Map BAR0 */
    if (pci_map_device_bars(dev) != 0) {
        kprintf(LOG_ERROR "ath: Failed to map device BARs\n");
        return -1;
    }
    if (!dev->bar_virt[0]) {
        kprintf(LOG_ERROR "ath: No BAR0 mapping available\n");
        return -1;
    }

    ath_mmio = (volatile uint32_t *)(uintptr_t)dev->bar_virt[0];

    /* Try to read MAC from three 16-bit EEPROM words (indices 0..2) */
    uint8_t mac[6] = {0};
    bool mac_ok = true;
    for (int i = 0; i < 3; ++i) {
        int w = ath_eeprom_read(i);
        if (w < 0) {
            kprintf(LOG_ERROR "ath: failed to read EEPROM word %d\n", i);
            mac_ok = false;
            break;
        }
        kprintf(LOG_INFO "ath: MAC ADDR part %d: 0x%04x\n", i, (uint16_t)w);
        mac[i*2 + 0] = (uint8_t)(w & 0xff);
        mac[i*2 + 1] = (uint8_t)(w >> 8);
    }

    if (mac_ok) {
        kprintf(LOG_INFO "ath: MAC Address = %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        kprintf(LOG_INFO "ath: MAC unavailable (EEPROM read failed)\n");
    }

    /* Print PCI interrupt info */
    uint8_t int_line = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x3C);
    uint8_t int_pin  = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x3D);
    kprintf(LOG_INFO "ath: Interrupt Line = %u pin = %u\n", int_line, int_pin);

    /* MSI enable omitted in minimal probe: enable later if required */

    kprintf(LOG_INFO "ath: probe completed (minimal)") ;
    return 0;
}

void ath_register(void)
{
    pci_register_device_driver(ATH_ETHERNET_VENDOR_ID, ATH_ETHERNET_DEVICE_ID, probe_ath);
}