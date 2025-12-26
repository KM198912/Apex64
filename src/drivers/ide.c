#include <drivers/ide.h>
#include <kernel/kprintf.h>
#include <lib/sys/io.h>
#include <lib/string.h>

static inline void delay_local(volatile int d) { 
    while (d--) __asm__ volatile ("nop"); 
}

/* IDE register offsets from base */
#define IDE_REG_DATA       0
#define IDE_REG_ERROR      1
#define IDE_REG_FEATURES   1
#define IDE_REG_SECCOUNT   2
#define IDE_REG_LBA_LOW    3
#define IDE_REG_LBA_MID    4
#define IDE_REG_LBA_HIGH   5
#define IDE_REG_DEVICE     6
#define IDE_REG_STATUS     7
#define IDE_REG_COMMAND    7

/* IDE status register bits */
#define IDE_STATUS_ERR     0x01
#define IDE_STATUS_DRQ     0x08
#define IDE_STATUS_SRV     0x10
#define IDE_STATUS_DF      0x20
#define IDE_STATUS_RDY     0x40
#define IDE_STATUS_BSY     0x80

/* IDE commands */
#define IDE_CMD_IDENTIFY        0xEC
#define IDE_CMD_IDENTIFY_PACKET 0xA1

/* Device control register bits */
#define IDE_CTRL_NIEN   0x02
#define IDE_CTRL_SRST   0x04

/* Global controller state */
static struct {
    uint16_t base_io[2];
    uint16_t ctrl_port[2];
    bool initialized;
} ide_controller;

static inline void ide_delay_400ns(uint16_t ctrl)
{
    for (int i = 0; i < 4; ++i) 
        (void)inb(ctrl);
}

static inline uint8_t ide_read_status(uint16_t base)
{
    return inb(base + IDE_REG_STATUS);
}

static inline uint8_t ide_read_altstatus(uint16_t ctrl)
{
    return inb(ctrl);
}

static int ide_wait_not_busy(uint16_t base, uint16_t ctrl, int timeout_ms)
{
    (void)base; /* base unused in this helper */
    for (int i = 0; i < timeout_ms; ++i) {
        uint8_t status = ide_read_altstatus(ctrl);
        if (status == 0xFF) return -1;
        if (!(status & IDE_STATUS_BSY)) return 0;
        delay_local(1000);
    }
    return -1;
}

static int ide_wait_drq(uint16_t base, uint16_t ctrl, int timeout_ms)
{
    (void)ctrl; /* ctrl unused in this helper */
    for (int i = 0; i < timeout_ms; ++i) {
        uint8_t status = ide_read_status(base);
        if (status == 0xFF) return -1;
        if (status & IDE_STATUS_ERR) return -1;
        if (status & IDE_STATUS_DRQ) return 0;
        delay_local(1000);
    }
    return -1;
}

static void ide_soft_reset(uint16_t base, uint16_t ctrl) __attribute__((unused));
static void ide_soft_reset(uint16_t base, uint16_t ctrl)
{
    outb(ctrl, IDE_CTRL_SRST | IDE_CTRL_NIEN);
    delay_local(5000);
    outb(ctrl, IDE_CTRL_NIEN);
    delay_local(2000);
    ide_wait_not_busy(base, ctrl, 2000);
}

/* Check if a device is present by examining signature and status */
static int ide_check_device_present(uint16_t base, uint16_t ctrl, int drive)
{
    /* Select drive */
    uint8_t drive_select = 0xA0 | ((drive & 1) << 4);
    outb(base + IDE_REG_DEVICE, drive_select);
    ide_delay_400ns(ctrl);
    
    /* Small delay for device selection */
    delay_local(10000);
    
    /* Check status register */
    uint8_t status = ide_read_altstatus(ctrl);
    
    /* Floating bus detection */
    if (status == 0xFF) {
        return 0; /* No device */
    }
    
    /* Status of 0 also means no device */
    if (status == 0x00) {
        return 0;
    }
    
    /* Read signature registers */
    uint8_t lba_mid = inb(base + IDE_REG_LBA_MID);
    uint8_t lba_high = inb(base + IDE_REG_LBA_HIGH);
    
    kprintf("ide: ch%d.%d probe: status=0x%02x lba_mid=0x%02x lba_high=0x%02x\n",
            (base == ide_controller.base_io[0]) ? 0 : 1, drive, 
            status, lba_mid, lba_high);
    
    /* Check for valid device signatures */
    if (lba_mid == 0x00 && lba_high == 0x00) {
        return 1; /* ATA device */
    }
    
    if (lba_mid == 0x14 && lba_high == 0xEB) {
        return 2; /* ATAPI device */
    }
    
    if (lba_mid == 0x3C && lba_high == 0xC3) {
        return 1; /* SATA device (rare in IDE mode) */
    }
    
    /* Floating bus patterns */
    if (lba_mid == 0xFF && lba_high == 0xFF) {
        return 0; /* Likely floating bus */
    }
    
    /* If status looks reasonable but signature is unknown, might still be a device */
    if ((status & IDE_STATUS_RDY) && !(status & IDE_STATUS_BSY)) {
        return 3; /* Unknown but possibly present */
    }
    
    return 0; /* Assume no device */
}

int ide_identify_drive(int channel, int drive, void *out_buf)
{
    if (channel < 0 || channel > 1 || drive < 0 || drive > 1 || !out_buf) {
        return -1;
    }
    
    if (!ide_controller.initialized) {
        kprintf("ide: controller not initialized\n");
        return -1;
    }
    
    uint16_t base = ide_controller.base_io[channel];
    uint16_t ctrl = ide_controller.ctrl_port[channel];
    
    /* First check if device is present */
    int device_type = ide_check_device_present(base, ctrl, drive);
    if (device_type == 0) {
        kprintf("ide: ch%d.%d no device detected\n", channel, drive);
        return -1;
    }
    
    kprintf("ide: ch%d.%d device type=%d detected\n", channel, drive, device_type);
    
    /* Select the drive */
    uint8_t drive_select = 0xA0 | ((drive & 1) << 4);
    outb(base + IDE_REG_DEVICE, drive_select);
    ide_delay_400ns(ctrl);
    
    /* Wait for ready */
    if (ide_wait_not_busy(base, ctrl, 1000) != 0) {
        kprintf("ide: ch%d.%d timeout on device select\n", channel, drive);
        return -1;
    }
    
    /* Clear any pending errors */
    (void)ide_read_status(base);
    
    /* Set up registers */
    outb(base + IDE_REG_FEATURES, 0);
    outb(base + IDE_REG_SECCOUNT, 0);
    outb(base + IDE_REG_LBA_LOW, 0);
    outb(base + IDE_REG_LBA_MID, 0);
    outb(base + IDE_REG_LBA_HIGH, 0);
    
    /* Choose command based on device type */
    uint8_t cmd = (device_type == 2) ? IDE_CMD_IDENTIFY_PACKET : IDE_CMD_IDENTIFY;
    outb(base + IDE_REG_COMMAND, cmd);
    ide_delay_400ns(ctrl);
    
    kprintf("ide: ch%d.%d issued command 0x%02x\n", channel, drive, cmd);
    
    /* Wait for BSY to clear */
    if (ide_wait_not_busy(base, ctrl, 3000) != 0) {
        kprintf("ide: ch%d.%d timeout waiting for BSY clear after command\n", channel, drive);
        return -1;
    }
    
    /* Read status and error */
    uint8_t status = ide_read_status(base);
    uint8_t error = inb(base + IDE_REG_ERROR);
    uint8_t lba_mid = inb(base + IDE_REG_LBA_MID);
    uint8_t lba_high = inb(base + IDE_REG_LBA_HIGH);
    
    kprintf("ide: ch%d.%d post-cmd: status=0x%02x error=0x%02x lba_mid=0x%02x lba_high=0x%02x\n",
            channel, drive, status, error, lba_mid, lba_high);
    
    /* Check for abort (wrong command for device type) */
    if (error & 0x04) { /* ABRT bit */
        kprintf("ide: ch%d.%d command aborted - trying alternate command\n", channel, drive);
        
        /* Try the other command type */
        cmd = (cmd == IDE_CMD_IDENTIFY) ? IDE_CMD_IDENTIFY_PACKET : IDE_CMD_IDENTIFY;
        outb(base + IDE_REG_COMMAND, cmd);
        ide_delay_400ns(ctrl);
        
        if (ide_wait_not_busy(base, ctrl, 3000) != 0) {
            kprintf("ide: ch%d.%d timeout on retry\n", channel, drive);
            return -1;
        }
        
        status = ide_read_status(base);
        error = inb(base + IDE_REG_ERROR);
        
        if (error & 0x04) {
            kprintf("ide: ch%d.%d both commands aborted\n", channel, drive);
            return -1;
        }
    }
    
    /* Wait for DRQ */
    if (ide_wait_drq(base, ctrl, 10000) != 0) {
        kprintf("ide: ch%d.%d timeout waiting for DRQ\n", channel, drive);
        return -1;
    }
    
    /* Read 256 words */
    uint16_t *buf = (uint16_t*)out_buf;
    for (int i = 0; i < 256; ++i) {
        buf[i] = inw(base + IDE_REG_DATA);
    }
    
    /* Validate data */
    int all_zero = 1, all_ff = 1;
    for (int i = 0; i < 256; ++i) {
        if (buf[i] != 0x0000) all_zero = 0;
        if (buf[i] != 0xFFFF) all_ff = 0;
    }
    
    if (all_zero || all_ff) {
        kprintf("ide: ch%d.%d got invalid identify data\n", channel, drive);
        return -1;
    }
    
    /* Print first few words for debugging */
    kprintf("ide: ch%d.%d IDENTIFY data [0-7]:", channel, drive);
    for (int i = 0; i < 8; ++i) {
        kprintf(" %04x", buf[i]);
    }
    kprintf("\n");
    
    return 0;
}

int ide_attach(struct pci_device *dev)
{
    if (!dev) return -1;
    
    kprintf("ide: attaching controller %02x:%02x.%x vendor=0x%04x device=0x%04x\n",
            dev->bus, dev->device, dev->function, dev->vendor_id, dev->device_id);
    
    /* Read programming interface */
    uint8_t prog_if = pci_read_config_byte(dev->bus, dev->device, dev->function, 0x09);
    kprintf("ide: programming interface = 0x%02x\n", prog_if);
    
    bool primary_native = (prog_if & 0x01) != 0;
    bool secondary_native = (prog_if & 0x04) != 0;
    
    kprintf("ide: primary=%s secondary=%s\n",
            primary_native ? "native" : "compat",
            secondary_native ? "native" : "compat");
    
    /* Read BARs */
    uint32_t bar0 = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x10);
    uint32_t bar1 = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x14);
    uint32_t bar2 = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x18);
    uint32_t bar3 = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x1C);
    uint32_t bar4 = pci_read_config_dword(dev->bus, dev->device, dev->function, 0x20);
    
    kprintf("ide: BAR0=0x%08x BAR1=0x%08x BAR2=0x%08x BAR3=0x%08x BAR4=0x%08x\n",
            bar0, bar1, bar2, bar3, bar4);
    
    /* Enable PCI command bits */
    uint16_t command = pci_read_config_word(dev->bus, dev->device, dev->function, 0x04);
    kprintf("ide: PCI command = 0x%04x\n", command);
    
    if (!(command & 0x01)) {
        kprintf("ide: enabling I/O space access\n");
        command |= 0x01;
    }
    if (!(command & 0x04)) {
        kprintf("ide: enabling bus mastering\n");
        command |= 0x04;
    }
    pci_write_config_word(dev->bus, dev->device, dev->function, 0x04, command);
    
    /* Determine I/O ports */
    if (primary_native && (bar0 & 0x01)) {
        ide_controller.base_io[0] = bar0 & 0xFFFC;
        ide_controller.ctrl_port[0] = (bar1 & 0xFFFC) + 2;
    } else {
        ide_controller.base_io[0] = 0x1F0;
        ide_controller.ctrl_port[0] = 0x3F6;
    }
    
    if (secondary_native && (bar2 & 0x01)) {
        ide_controller.base_io[1] = bar2 & 0xFFFC;
        ide_controller.ctrl_port[1] = (bar3 & 0xFFFC) + 2;
    } else {
        ide_controller.base_io[1] = 0x170;
        ide_controller.ctrl_port[1] = 0x376;
    }
    
    kprintf("ide: primary  base=0x%04x ctrl=0x%04x\n", 
            ide_controller.base_io[0], ide_controller.ctrl_port[0]);
    kprintf("ide: secondary base=0x%04x ctrl=0x%04x\n", 
            ide_controller.base_io[1], ide_controller.ctrl_port[1]);
    
    ide_controller.initialized = true;
    
    /* Disable interrupts (polling mode) */
    outb(ide_controller.ctrl_port[0], IDE_CTRL_NIEN);
    outb(ide_controller.ctrl_port[1], IDE_CTRL_NIEN);
    
    kprintf("\n=== IDE Device Detection ===\n");
    kprintf("NOTE: If you see garbage (0xff values), the disk may be on AHCI controller\n");
    kprintf("      Check AHCI controller (00:1f.2) output for your hard drive\n\n");
    
    /* Probe all drives */
    for (int ch = 0; ch < 2; ++ch) {
        for (int dr = 0; dr < 2; ++dr) {
            kprintf("ide: === Probing ch%d.%d ===\n", ch, dr);
            
            uint8_t idbuf[512];
            memset(idbuf, 0, 512);
            
            if (ide_identify_drive(ch, dr, idbuf) == 0) {
                uint16_t *id = (uint16_t*)idbuf;
                
                /* Extract model */
                char model[41];
                for (int i = 0; i < 20; ++i) {
                    uint16_t w = id[27 + i];
                    model[i*2] = (char)(w >> 8);
                    model[i*2 + 1] = (char)(w & 0xFF);
                }
                model[40] = '\0';
                
                /* Trim spaces */
                for (int i = 39; i >= 0 && model[i] == ' '; --i) {
                    model[i] = '\0';
                }
                
                /* Get capacity */
                uint64_t sectors = 0;
                if (id[83] & 0x0400) {
                    /* 48-bit LBA */
                    sectors = ((uint64_t)id[100]) | 
                              ((uint64_t)id[101] << 16) |
                              ((uint64_t)id[102] << 32) |
                              ((uint64_t)id[103] << 48);
                } else {
                    /* 28-bit LBA */
                    sectors = ((uint64_t)id[60]) | ((uint64_t)id[61] << 16);
                }
                
                uint64_t size_mb = (sectors * 512) / (1024 * 1024);
                
                /* Determine device type from word 0 */
                uint16_t word0 = id[0];
                const char *type = "ATA";
                if (word0 & 0x8000) {
                    if ((word0 & 0xC000) == 0x8000) {
                        type = "ATAPI";
                    }
                }
                
                kprintf("  *** IDE ch%d.%d FOUND: %s ***\n", ch, dr, type);
                kprintf("      Model: '%s'\n", model);
                kprintf("      Capacity: %llu sectors (%llu MB)\n",
                        (unsigned long long)sectors,
                        (unsigned long long)size_mb);
            } else {
                kprintf("  IDE ch%d.%d: No device or identify failed\n", ch, dr);
            }
            kprintf("\n");
        }
    }
    
    return 0;
}