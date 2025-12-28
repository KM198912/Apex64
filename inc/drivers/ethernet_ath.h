#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/*
* Atheros AR8151 v1.0 Gigabit Ethernet
* Apex64 Driver
*/
#define ATH_ETHERNET_VENDOR_ID 0x1969
#define ATH_ETHERNET_DEVICE_ID 0x1090

/* PCI/Class IDs */
#define ATH_ETHERNET_PCI_CLASS    0x0200 /* Network controller / Ethernet */
#define ATH_ETHERNET_BAR0         0

/* AR8151 register offsets (BAR0) */
#define AR8151_REG_MAC_CR        0x0000 /* MAC control */
#define AR8151_REG_MAC_CFG       0x0010 /* MAC configuration */
#define AR8151_REG_RX_CFG        0x0020 /* RX configuration */
#define AR8151_REG_TX_CFG        0x0030 /* TX configuration */
#define AR8151_REG_INT_STATUS    0x00D0 /* Interrupt status */
#define AR8151_REG_INT_MASK      0x00D4 /* Interrupt mask */
#define AR8151_REG_MII_CTRL      0x00E0 /* MII/MDIO control */
#define AR8151_REG_MII_DATA      0x00E4 /* MII/MDIO data */
#define AR8151_REG_SW_RESET      0x00F0 /* Software reset */

/* MAC control bits */
#define AR8151_MAC_CR_RX_EN      (1u << 0)
#define AR8151_MAC_CR_TX_EN      (1u << 1)
#define AR8151_MAC_CR_PROMISC    (1u << 2)
#define AR8151_MAC_CR_SOFT_RESET (1u << 31)

/* Interrupts */
#define AR8151_INT_LINK_CHANGE   (1u << 2)
#define AR8151_INT_RX_DONE       (1u << 4)
#define AR8151_INT_TX_DONE       (1u << 5)

/* MII/MDIO control bits */
#define AR8151_MII_BUSY          (1u << 0)
#define AR8151_MII_READ          (1u << 1)
#define AR8151_MII_WRITE         (0u << 1)

/* PHY registers (standard MII) */
#define MII_REG_BMCR             0x00 /* Basic control */
#define MII_REG_BMSR             0x01 /* Basic status */
#define MII_REG_PHYID1           0x02 /* PHY identifier 1 */
#define MII_REG_PHYID2           0x03 /* PHY identifier 2 */
#define MII_REG_ANAR             0x04 /* Auto-neg advertisement */
#define MII_REG_ANLPAR           0x05 /* Auto-neg link partner ability */

/* Basic control bits (BMCR) */
#define MII_BMCR_RESET           (1u << 15)
#define MII_BMCR_AN_ENABLE       (1u << 12)
#define MII_BMCR_RESTART_AN      (1u << 9)
#define MII_BMCR_SPEED_SEL       (1u << 13) /* example: speed select bit */

/* Basic status bits (BMSR) */
#define MII_BMSR_LINK_STATUS     (1u << 2)
#define MII_BMSR_AN_COMPLETE     (1u << 5)

/* Descriptor and driver structures */

/* Generic DMA descriptor used for TX and RX rings
 * Layout is simplified: buffer address (low/high), length and command/status
 */
struct ath_desc {
	uint32_t addr_low;
	uint32_t addr_high;
	uint32_t length;
	uint32_t cmd; /* owner, flags, status */
} __attribute__((packed));

/* Ring buffer for descriptors */
struct ath_ring {
	struct ath_desc *descs;   /* pointer to descriptor array (DMA-able) */
	void **buffers;           /* pointers to CPU buffers (e.g. sk_buffs) */
	size_t size;              /* number of descriptors */
	uint32_t head;            /* producer index */
	uint32_t tail;            /* consumer index */
};

/* Private device structure */
struct ath_private {
	volatile void *mmio;      /* mapped BAR0 */
	int irq;
	int phy_addr;             /* PHY MDIO address */
	bool link_up;
	uint8_t mac_addr[6];

	struct ath_ring rx;       /* RX descriptor ring */
	struct ath_ring tx;       /* TX descriptor ring */
};

/* Default ring sizes */
#define ATH_RX_RING_SIZE 256
#define ATH_TX_RING_SIZE 256

/* Minimal probe helpers (used by probe registration) */
int ath_eeprom_read(uint16_t off);
bool ath_eeprom_write(uint16_t off, uint16_t value);
void ath_register(void);

