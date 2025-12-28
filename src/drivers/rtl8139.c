#include <bus/pci.h>
#include <kernel/kprintf.h>
#include <lib/sys/io.h>
#include <drivers/idt.h>
#include <drivers/pit.h>
#include <mem/pmm.h>
#include <common/boot.h>
#include <drivers/rtl8139.h>

#include <stdbool.h>
#include <lib/string.h>

/* Local helper: find a capability in PCI config space */
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

/* Driver state globals */
static uint16_t rtl_io_base = 0;
static uint64_t rtl_mmio_base = 0;
static bool rtl_is_io = false;

static uint64_t rtl_rx_phys = 0;
static void *rtl_rx_virt = NULL;
static size_t rtl_rx_alloc_size = 0;
static size_t rtl_rx_ring_size = 0;
static uint64_t rtl_tx_phys[4] = {0};
static void *rtl_tx_virt[4] = {0};
static int rtl_tx_idx = 0;

/* Forward decls */
static void rtl_handle_rx_io(void);
static void rtl_handle_rx_mmio(void);
static void rtl8139_isr(context_t *ctx);
static int probe_rtl8139(struct pci_device* dev);

static void rtl_handle_rx_io(void) {
    if (!rtl_rx_virt) return;
    
    /* CBR (0x36) is where the NIC is currently writing (its write pointer)
     * CAPR (0x38) is what we tell the NIC we've read up to (our read pointer - 16) */
    uint16_t cbr = inw(rtl_io_base + 0x36);
    uint16_t capr_reg = inw(rtl_io_base + 0x38);
    
    /* Our actual read position in the ring - compute from CAPR
     * Special case: 0xFFF0 means we start at position 0 */
    uint32_t read_pos;
    if (capr_reg == 0xFFF0) {
        read_pos = 0;
    } else {
        read_pos = (capr_reg + 16) % 8192;
    }
    
    kprintf(LOG_INFO "rtl8139: RX IRQ CBR=%u CAPR_reg=%u read_pos=%u\n", 
            cbr, capr_reg, read_pos);
    
    /* Sanity check */
    uint32_t rbstart_reg = inl(rtl_io_base + 0x30);
    if (rbstart_reg != (uint32_t)rtl_rx_phys) {
        kprintf(LOG_ERROR "rtl8139: RBSTART mismatch! reg=0x%08x expected=0x%llx\n", 
                rbstart_reg, (unsigned long long)rtl_rx_phys);
    }
    
    int processed = 0;
    uint8_t *buf = (uint8_t*)rtl_rx_virt;
    
    /* If we're at read_pos=0 and CBR is non-zero, we may have missed the initial packets
     * Let's check if there's actually data at CBR - 4 (the last packet header) */
    if (read_pos == 0 && cbr > 0) {
        uint16_t check_status = *(uint16_t*)(buf + 0);
        if (!(check_status & 0x0001)) {
            /* No valid packet at position 0 - device started writing past 0
             * This can happen if device started writing before we initialized CAPR
             * Jump to CBR and mark everything before as consumed */
            kprintf(LOG_INFO "rtl8139: no data at pos 0, jumping to CBR=%u\n", cbr);
            read_pos = cbr;
            uint16_t new_capr = (read_pos >= 16) ? (read_pos - 16) : (8192 + read_pos - 16);
            outw(rtl_io_base + 0x38, new_capr);
            kprintf(LOG_INFO "rtl8139: advanced CAPR to %u to skip missing data\n", new_capr);
            return;
        }
    }
    
    /* Process packets between our read position and where NIC has written */
    while (read_pos != cbr && processed < 32) {
        /* Read packet header: status (2 bytes) + length (2 bytes) */
        uint16_t status = *(uint16_t*)(buf + read_pos);
        uint16_t len = *(uint16_t*)(buf + read_pos + 2);
        
        kprintf(LOG_INFO "rtl8139: checking pos=%u status=0x%04x len=%u\n",
                read_pos, status, len);
        
        /* Validate packet */
        if (!(status & 0x0001)) {
            kprintf(LOG_ERROR "rtl8139: packet at pos=%u missing ROK bit (status=0x%04x)\n",
                    read_pos, status);
            break;
        }
        
        if (len < 14 || len > 1518) {
            kprintf(LOG_ERROR "rtl8139: invalid len=%u at pos=%u\n", len, read_pos);
            /* Dump some data */
            for (int i = 0; i < 64; ++i) {
                if ((i & 0x0F) == 0) kprintf(LOG_INFO "%04x: ", (read_pos + i) % 8192);
                kprintf("%02x ", buf[(read_pos + i) % 8192]);
                if ((i & 0x0F) == 0x0F) kprintf("\n");
            }
            kprintf("\n");
            break;
        }
        
        processed++;
        
        kprintf(LOG_INFO "rtl8139: RX pkt#%d len=%u status=0x%04x\n",
                processed, len, status);
        
        /* Print first 14 bytes (Ethernet header) */
        uint32_t data_off = (read_pos + 4) % 8192;
        kprintf(LOG_INFO "  data:");
        for (int i = 0; i < 14 && i < len; ++i) {
            kprintf(" %02x", buf[(data_off + i) % 8192]);
        }
        kprintf("\n");
        
        /* Advance: header (4) + data (len), align to 4 bytes */
        uint32_t pkt_total = 4 + len;
        pkt_total = (pkt_total + 3) & ~3U;  /* align to 4 */
        
        read_pos = (read_pos + pkt_total) % 8192;
        
        kprintf(LOG_INFO "rtl8139: advanced to read_pos=%u\n", read_pos);
    }
    
    /* Update CAPR to tell NIC what we've consumed
     * CAPR must be set to (read_pos - 16) with wraparound */
    uint16_t new_capr;
    if (read_pos >= 16) {
        new_capr = read_pos - 16;
    } else {
        new_capr = 8192 + read_pos - 16;
    }
    
    outw(rtl_io_base + 0x38, new_capr);
    
    kprintf(LOG_INFO "rtl8139: RX done, processed=%d final_read_pos=%u wrote_CAPR=%u\n", 
            processed, read_pos, new_capr);
}

static void rtl_handle_rx_mmio(void) {
    if (!rtl_rx_virt || !rtl_mmio_base) return;
    
    volatile uint8_t *mmio8 = (volatile uint8_t*)(uintptr_t)rtl_mmio_base;
    volatile uint32_t *mmio32 = (volatile uint32_t*)(uintptr_t)rtl_mmio_base;
    
    uint16_t cbr = *(volatile uint16_t*)(mmio8 + 0x36);
    uint16_t capr_reg = *(volatile uint16_t*)(mmio8 + 0x38);
    
    uint32_t read_pos;
    if (capr_reg == 0xFFF0) {
        read_pos = 0;
    } else {
        read_pos = (capr_reg + 16) % 8192;
    }
    
    kprintf(LOG_INFO "rtl8139: MMIO RX IRQ CBR=%u CAPR_reg=%u read_pos=%u\n",
            cbr, capr_reg, read_pos);
    
    uint32_t rbstart_reg = mmio32[0x30/4];
    if (rbstart_reg != (uint32_t)rtl_rx_phys) {
        kprintf(LOG_ERROR "rtl8139: MMIO RBSTART mismatch! reg=0x%08x expected=0x%llx\n",
                rbstart_reg, (unsigned long long)rtl_rx_phys);
    }
    
    int processed = 0;
    uint8_t *buf = (uint8_t*)rtl_rx_virt;
    
    /* If we're at read_pos=0 and CBR is non-zero, check for missed initial packets */
    if (read_pos == 0 && cbr > 0) {
        uint16_t check_status = *(uint16_t*)(buf + 0);
        if (!(check_status & 0x0001)) {
            kprintf(LOG_INFO "rtl8139: MMIO no data at pos 0, jumping to CBR=%u\n", cbr);
            read_pos = cbr;
            uint16_t new_capr = (read_pos >= 16) ? (read_pos - 16) : (8192 + read_pos - 16);
            *(volatile uint16_t*)(mmio8 + 0x38) = new_capr;
            kprintf(LOG_INFO "rtl8139: MMIO advanced CAPR to %u to skip missing data\n", new_capr);
            return;
        }
    }
    
    while (read_pos != cbr && processed < 32) {
        uint16_t status = *(uint16_t*)(buf + read_pos);
        uint16_t len = *(uint16_t*)(buf + read_pos + 2);
        
        kprintf(LOG_INFO "rtl8139: MMIO checking pos=%u status=0x%04x len=%u\n",
                read_pos, status, len);
        
        if (!(status & 0x0001)) {
            kprintf(LOG_ERROR "rtl8139: MMIO packet at pos=%u missing ROK bit (status=0x%04x)\n",
                    read_pos, status);
            break;
        }
        
        if (len < 14 || len > 1518) {
            kprintf(LOG_ERROR "rtl8139: MMIO invalid len=%u at pos=%u\n", len, read_pos);
            for (int i = 0; i < 64; ++i) {
                if ((i & 0x0F) == 0) kprintf(LOG_INFO "%04x: ", (read_pos + i) % 8192);
                kprintf("%02x ", buf[(read_pos + i) % 8192]);
                if ((i & 0x0F) == 0x0F) kprintf("\n");
            }
            kprintf("\n");
            break;
        }
        
        processed++;
        
        kprintf(LOG_INFO "rtl8139: MMIO RX pkt#%d len=%u status=0x%04x\n",
                processed, len, status);
        
        uint32_t data_off = (read_pos + 4) % 8192;
        kprintf(LOG_INFO "  data:");
        for (int i = 0; i < 14 && i < len; ++i) {
            kprintf(" %02x", buf[(data_off + i) % 8192]);
        }
        kprintf("\n");
        
        uint32_t pkt_total = 4 + len;
        pkt_total = (pkt_total + 3) & ~3U;
        
        read_pos = (read_pos + pkt_total) % 8192;
        
        kprintf(LOG_INFO "rtl8139: MMIO advanced to read_pos=%u\n", read_pos);
    }
    
    uint16_t new_capr;
    if (read_pos >= 16) {
        new_capr = read_pos - 16;
    } else {
        new_capr = 8192 + read_pos - 16;
    }
    
    *(volatile uint16_t*)(mmio8 + 0x38) = new_capr;
    
    kprintf(LOG_INFO "rtl8139: MMIO RX done, processed=%d final_read_pos=%u wrote_CAPR=%u\n",
            processed, read_pos, new_capr);
}

static void rtl8139_isr(context_t *ctx) {
    uint16_t status = 0;
    
    /* Read interrupt status */
    if (rtl_is_io && rtl_io_base) {
        status = inw(rtl_io_base + 0x3E);
        if (status == 0) {
            interrupts_eoi();
            return;
        }
        kprintf(LOG_INFO "rtl8139: IRQ (vec=%u) status=0x%04x\n", ctx->int_no, status);
        
        /* Acknowledge interrupts immediately to prevent re-entry */
        outw(rtl_io_base + 0x3E, status);
        
    } else if (!rtl_is_io && rtl_mmio_base) {
        volatile uint32_t *mmio = (volatile uint32_t*)(uintptr_t)rtl_mmio_base;
        status = (uint16_t)(mmio[0x3E / 4] & 0xFFFF);
        if (status == 0) {
            interrupts_eoi();
            return;
        }
        kprintf(LOG_INFO "rtl8139: MMIO IRQ (vec=%u) status=0x%04x\n", ctx->int_no, status);
        
        /* Acknowledge immediately */
        mmio[0x3E / 4] = status;
        
    } else {
        interrupts_eoi();
        return;
    }
    
    /* Handle Receive OK (bit 0) */
    if (status & 0x0001) {
        if (rtl_is_io) {
            rtl_handle_rx_io();
        } else {
            rtl_handle_rx_mmio();
        }
    }
    
    /* Handle Transmit OK (bit 2) */
    if (status & 0x0004) {
        kprintf(LOG_INFO "rtl8139: Transmit OK\n");
    }
    
    /* Handle RX Error (bit 3) */
    if (status & 0x0008) {
        kprintf(LOG_ERROR "rtl8139: RX Error\n");
    }
    
    /* Handle TX Error (bit 3) - same bit for TER */
    if (status & 0x0008) {
        kprintf(LOG_ERROR "rtl8139: TX Error\n");
    }
    
    /* Handle RX Overflow (bit 4) */
    if (status & 0x0010) {
        kprintf(LOG_ERROR "rtl8139: RX Buffer Overflow - resetting receiver\n");
        
        if (rtl_is_io) {
            /* Disable receiver */
            uint8_t cmd = inb(rtl_io_base + 0x37);
            outb(rtl_io_base + 0x37, cmd & ~0x04);
            
            /* Re-initialize CAPR */
            outw(rtl_io_base + 0x38, 0xFFF0);
            
            /* Re-enable receiver */
            outb(rtl_io_base + 0x37, cmd | 0x04);
            
            kprintf(LOG_INFO "rtl8139: receiver reset complete\n");
        } else {
            volatile uint8_t *mmio8 = (volatile uint8_t*)(uintptr_t)rtl_mmio_base;
            uint8_t cmd = mmio8[0x37];
            mmio8[0x37] = cmd & ~0x04;
            *(volatile uint16_t*)(mmio8 + 0x38) = 0xFFF0;
            mmio8[0x37] = cmd | 0x04;
            kprintf(LOG_INFO "rtl8139: MMIO receiver reset complete\n");
        }
    }
    
    interrupts_eoi();
}

static int probe_rtl8139(struct pci_device* dev)
{
    if (dev->vendor_id != 0x10ec || dev->device_id != 0x8139) return -1;
    kprintf(LOG_INFO "pci: matched specific device 10ec:8139 at %02x:%02x.%x\n",
            dev->bus, dev->device, dev->function);

    uint8_t mac[6] = {0};

    /* If BAR0 is I/O, read via inb() from I/O port base */
    if (dev->bar_is_io[0]) {
        uint16_t io_base = (uint16_t)dev->bar[0];
        kprintf(LOG_INFO "rtl8139: I/O base = 0x%04x\n", io_base);
        /* Ensure I/O space is enabled */
        uint16_t cmd = pci_read_config_word(dev->bus, dev->device, dev->function, 0x04);
        if (!(cmd & 0x01)) {
            pci_write_config_word(dev->bus, dev->device, dev->function, 0x04, cmd | 0x01);
        }
        for (int i = 0; i < 6; ++i) mac[i] = inb(io_base + i);
    } else {
        /* MMIO path */
        if (!dev->bar_virt[0]) {
            pci_map_device_bars(dev);
            if (!dev->bar_virt[0] && dev->bar[0] && dev->bar[0] < 0x100000000ULL) {
                dev->bar_virt[0] = (uint64_t)PHYS_TO_VIRT(dev->bar[0]);
            }
        }
        if (!dev->bar_virt[0]) {
            kprintf(LOG_ERROR "rtl8139: no BAR mapping available (aborting probe)\n");
            return -1;
        }
        volatile uint8_t *mmio = (volatile uint8_t*)(uintptr_t)dev->bar_virt[0];
        for (int i = 0; i < 6; ++i) mac[i] = mmio[i];
    }

    kprintf(LOG_INFO "rtl8139: MAC Address = %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t int_line = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x3C);
    uint8_t int_pin  = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x3D);
    kprintf(LOG_INFO "rtl8139: Interrupt Line = %u pin = %u\n", int_line, int_pin);

    /* Try to enable MSI if the device supports it */
    int msi_off = find_pci_cap(dev, 0x05);
    if (msi_off) {
        kprintf(LOG_INFO "rtl8139: MSI capability at 0x%02x\n", msi_off);
        uint16_t msgctrl = pci_read_config_word(dev->bus, dev->device, dev->function, msi_off + 2);
        bool is64 = (msgctrl & (1<<7)) != 0;
        kprintf(LOG_INFO "rtl8139: MSI msgctrl=0x%04x (64bit=%d)\n", msgctrl, is64);
        uint8_t vec = interrupts_alloc_vec();
        uint32_t msg_addr_lo = 0xFEE00000; /* local APIC destination */
        pci_config_write32(dev->bus, dev->device, dev->function, msi_off + 4, msg_addr_lo);
        if (is64) {
            pci_config_write32(dev->bus, dev->device, dev->function, msi_off + 8, 0x0);
            pci_write_config_word(dev->bus, dev->device, dev->function, msi_off + 12, vec);
        } else {
            pci_write_config_word(dev->bus, dev->device, dev->function, msi_off + 8, vec);
        }
        pci_write_config_word(dev->bus, dev->device, dev->function, msi_off + 2, msgctrl | 0x1);
        kprintf(LOG_INFO "rtl8139: enabled MSI vector %u\n", vec);
        interrupts_set_handler(vec, rtl8139_isr);
    } else {
        kprintf(LOG_INFO "rtl8139: MSI not present, using legacy INTx\n");
        uint8_t vec = 32 + int_line;
        interrupts_set_handler(vec, rtl8139_isr);
        kprintf(LOG_INFO "rtl8139: registered IRQ handler at vector %u for IRQ %u\n", vec, int_line);
    }

    /* Initialize device */
    if (dev->bar_is_io[0]) {
        rtl_is_io = true;
        rtl_io_base = (uint16_t)dev->bar[0];
        kprintf(LOG_INFO "rtl8139: using I/O base 0x%04x for device init\n", rtl_io_base);

        /* Software reset */
        outb(rtl_io_base + 0x37, 0x10);
        uint64_t start = pit_get_ticks();
        while (inb(rtl_io_base + 0x37) & 0x10) {
            if ((pit_get_ticks() - start) > 1000) {
                kprintf(LOG_ERROR "rtl8139: reset timeout\n");
                break;
            }
        }
        kprintf(LOG_INFO "rtl8139: reset complete\n");

        /* Allocate contiguous RX buffer (8K+16+1500 for safety) */
        const size_t rx_size = 8192 + 16 + 1500;
        int pages = (rx_size + 4095) / 4096;
        uint64_t phys_base = 0;
        
        for (int attempt = 0; attempt < 200; ++attempt) {
            uint64_t frames[16];
            if (pages > 16) break;
            
            bool ok = true;
            for (int i = 0; i < pages; ++i) {
                frames[i] = pmm_alloc_frame();
                if (!frames[i]) {
                    ok = false;
                    break;
                }
            }
            
            if (!ok) {
                for (int i = 0; i < pages; ++i) {
                    if (frames[i]) pmm_free_frame(frames[i]);
                }
                continue;
            }
            
            /* Check contiguity */
            for (int i = 1; i < pages; ++i) {
                if (frames[i] != frames[i-1] + 4096) {
                    ok = false;
                    break;
                }
            }
            
            if (!ok) {
                for (int i = 0; i < pages; ++i) pmm_free_frame(frames[i]);
                continue;
            }
            
            phys_base = frames[0];
            break;
        }
        
        if (!phys_base) {
            kprintf(LOG_ERROR "rtl8139: failed to allocate contiguous RX buffer\n");
            return -1;
        }
        
        void *rxv = (void*)PHYS_TO_VIRT(phys_base);
        memset(rxv, 0, pages * 4096);
        
        rtl_rx_phys = phys_base;
        rtl_rx_virt = rxv;
        rtl_rx_alloc_size = pages * 4096;
        rtl_rx_ring_size = 8192;
        
        kprintf(LOG_INFO "rtl8139: allocated RX buffer phys=0x%llx virt=%p size=%zu\n",
                (unsigned long long)phys_base, rxv, rtl_rx_alloc_size);
        
        /* Set RBSTART - physical address of RX buffer */
        outl(rtl_io_base + 0x30, (uint32_t)phys_base);
        
        /* Initialize CAPR to 0xFFF0 before enabling receiver
         * This means "we've read up to position 0" (since 0xFFF0 + 16 = 0x10000 = 0 mod 8192) */
        outw(rtl_io_base + 0x38, 0xFFF0);
        kprintf(LOG_INFO "rtl8139: initialized CAPR to 0xFFF0 (read position will be 0)\n");
        
        /* Configure RCR: Accept Broadcast (bit 3) + Multicast (bit 2) + Physical Match (bit 1) + All Phys (bit 0) */
        outl(rtl_io_base + 0x44, 0x0000000F);
        
        /* Clear all interrupt status bits */
        outw(rtl_io_base + 0x3E, 0xFFFF);
        
        /* Enable interrupts: ROK(1) + TOK(4) + RER(8) + TER(8) + RXOVW(16) */
        outw(rtl_io_base + 0x3C, 0x001D);
        kprintf(LOG_INFO "rtl8139: interrupts enabled (IMR=0x001D)\n");
        
        /* Enable Receiver (RE, bit 3) and Transmitter (TE, bit 2) */
        outb(rtl_io_base + 0x37, 0x0C);
        kprintf(LOG_INFO "rtl8139: receiver and transmitter enabled\n");
        
        /* Read back and verify CBR starts at 0 */
        uint16_t cbr = inw(rtl_io_base + 0x36);
        uint16_t capr = inw(rtl_io_base + 0x38);
        kprintf(LOG_INFO "rtl8139: post-init CBR=%u CAPR=%u\n", cbr, capr);
        
    } else {
        rtl_is_io = false;
        rtl_mmio_base = dev->bar_virt[0];
        kprintf(LOG_INFO "rtl8139: using MMIO base 0x%llx for device init\n",
                (unsigned long long)rtl_mmio_base);

        volatile uint32_t *mmio = (volatile uint32_t*)(uintptr_t)rtl_mmio_base;
        volatile uint8_t *mmio8 = (volatile uint8_t*)(uintptr_t)rtl_mmio_base;
        
        /* Software reset */
        mmio8[0x37] = 0x10;
        uint64_t start = pit_get_ticks();
        while (mmio8[0x37] & 0x10) {
            if ((pit_get_ticks() - start) > 1000) {
                kprintf(LOG_ERROR "rtl8139: reset timeout\n");
                break;
            }
        }
        kprintf(LOG_INFO "rtl8139: reset complete\n");

        /* Allocate contiguous RX buffer */
        const size_t rx_size = 8192 + 16 + 1500;
        int pages = (rx_size + 4095) / 4096;
        uint64_t phys_base = 0;
        
        for (int attempt = 0; attempt < 200; ++attempt) {
            uint64_t frames[16];
            if (pages > 16) break;
            
            bool ok = true;
            for (int i = 0; i < pages; ++i) {
                frames[i] = pmm_alloc_frame();
                if (!frames[i]) {
                    ok = false;
                    break;
                }
            }
            
            if (!ok) {
                for (int i = 0; i < pages; ++i) {
                    if (frames[i]) pmm_free_frame(frames[i]);
                }
                continue;
            }
            
            for (int i = 1; i < pages; ++i) {
                if (frames[i] != frames[i-1] + 4096) {
                    ok = false;
                    break;
                }
            }
            
            if (!ok) {
                for (int i = 0; i < pages; ++i) pmm_free_frame(frames[i]);
                continue;
            }
            
            phys_base = frames[0];
            break;
        }
        
        if (!phys_base) {
            kprintf(LOG_ERROR "rtl8139: failed to allocate contiguous RX buffer\n");
            return -1;
        }
        
        void *rxv = (void*)PHYS_TO_VIRT(phys_base);
        memset(rxv, 0, pages * 4096);
        
        rtl_rx_phys = phys_base;
        rtl_rx_virt = rxv;
        rtl_rx_alloc_size = pages * 4096;
        rtl_rx_ring_size = 8192;
        
        kprintf(LOG_INFO "rtl8139: allocated RX buffer phys=0x%llx virt=%p size=%zu\n",
                (unsigned long long)phys_base, rxv, rtl_rx_alloc_size);
        
        /* Set RBSTART */
        mmio[0x30 / 4] = (uint32_t)phys_base;
        
        /* Initialize CAPR to 0xFFF0 */
        *(volatile uint16_t*)(mmio8 + 0x38) = 0xFFF0;
        kprintf(LOG_INFO "rtl8139: initialized CAPR to 0xFFF0 (read position will be 0)\n");
        
        /* Configure RCR */
        mmio[0x44 / 4] = 0x0000000F;
        
        /* Clear all interrupt status bits */
        mmio[0x3E / 4] = 0xFFFF;
        
        /* Enable interrupts */
        mmio[0x3C / 4] = 0x001D;
        kprintf(LOG_INFO "rtl8139: interrupts enabled (IMR=0x001D)\n");
        
        /* Enable Receiver and Transmitter */
        mmio8[0x37] = 0x0C;
        kprintf(LOG_INFO "rtl8139: receiver and transmitter enabled\n");
        
        /* Read back and verify */
        uint16_t cbr = *(volatile uint16_t*)(mmio8 + 0x36);
        uint16_t capr = *(volatile uint16_t*)(mmio8 + 0x38);
        kprintf(LOG_INFO "rtl8139: post-init CBR=%u CAPR=%u\n", cbr, capr);
    }
    
    kprintf(LOG_INFO "rtl8139: device initialization complete\n");
    return 0;
}

void rtl8139_register(void)
{
  //  pci_register_device_driver(0x10ec, 0x8139, probe_rtl8139);
}