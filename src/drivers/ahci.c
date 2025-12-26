#include <drivers/ahci.h>
#include <kernel/kprintf.h>
#include <mem/pmm.h>
#include <common/boot.h>
#include <lib/string.h>
#include <dev/dev.h>
#include <stddef.h>

/* AHCI structures (minimal subset) */
struct hba_mem {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  reserved[0xA0-0x28];
    uint8_t  vendor[0x100-0xA0];
    struct hba_port { /* 0x100 per port */
        uint32_t clb;
        uint32_t clbu;
        uint32_t fb;
        uint32_t fbu;
        uint32_t is;
        uint32_t ie;
        uint32_t cmd;
        uint32_t reserved0;
        uint32_t tfd;
        uint32_t sig;
        uint32_t ssts;
        uint32_t sctl;
        uint32_t serr;
        uint32_t sact;
        uint32_t ci;
        uint32_t sata_ctl;
        uint32_t sata_sntf;
        uint32_t sata_devslp;
        uint8_t  reserved1[0x70-0x48];
        uint32_t vendor[0x80/4 - 0x1C];
    } ports[1];
};

/* Command header */
struct hba_cmd_header {
    uint8_t  cfl:5;   /* command fis length in DWs */
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  reserved0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved1[4];
};

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc; /* byte count, interrupt on completion */
};

/* Command table (minimal) */
struct hba_cmd_tbl {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct hba_prdt_entry prdt[1];
};

/* H2D FIS */
struct fis_h2d {
    uint8_t type; // 0x27
    uint8_t pmport:4;
    uint8_t res1:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
};

static inline void delay(volatile int d) { while (d--) __asm__ volatile ("nop"); }

/* Per-controller and per-port persistent state to avoid leaking frames and to
 * ensure the port is started before submitting commands. */
struct ahci_port_state {
    uint64_t clb_ph;
    uint64_t fb_ph;
    uint64_t ct_ph;
    uint64_t buf_ph;
    int initialized;
};

struct ahci_controller {
    uintptr_t abar;
    struct ahci_port_state ports[32];
};

#define MAX_AHCI_CONTROLLERS 4
static struct ahci_controller ahci_controllers[MAX_AHCI_CONTROLLERS];

static struct ahci_controller* ahci_get_controller(uintptr_t abar)
{
    for (int i = 0; i < MAX_AHCI_CONTROLLERS; ++i) {
        if (ahci_controllers[i].abar == abar) return &ahci_controllers[i];
        if (ahci_controllers[i].abar == 0) {
            ahci_controllers[i].abar = abar;
            return &ahci_controllers[i];
        }
    }
    return NULL;
}

static void dump_port_status(volatile struct hba_port *port, int portno)
{
    kprintf("ahci: port %d status CMD=0x%08x SSTS=0x%08x TFD=0x%08x IS=0x%08x SERR=0x%08x\n",
            portno, port->cmd, port->ssts, port->tfd, port->is, port->serr);
}

static int start_port(volatile struct hba_port *port)
{
    /* Be tolerant: try to stop any previously running engine first */
    if (port->cmd & (1u << 15)) {
        port->cmd &= ~(1u << 0); /* clear ST */
        int t = 100000;
        while (port->cmd & (1u << 15)) { if (--t == 0) break; }
    }

    /* Enable FIS receive and start */
    port->cmd |= (1u << 4); /* FRE */
    port->cmd |= (1u << 0); /* ST  */

    /* Wait for CR to be set (engine running) */
    int t = 1000000;
    while (!(port->cmd & (1u << 15))) { if (--t == 0) {
        kprintf("ahci: start_port timeout (CMD=0x%08x SST=0x%08x)\n", port->cmd, port->ssts);
        return -1;
    } }
    return 0;
}

static void stop_port(volatile struct hba_port *port)
{
    /* Clear ST and FRE and wait for CR to clear */
    port->cmd &= ~(1u << 0); /* clear ST */
    port->cmd &= ~(1u << 4); /* clear FRE */
    int t = 1000000;
    while (port->cmd & (1u << 15)) { if (--t == 0) break; }
}

/* Perform a COMRESET on the port and wait for a device to appear. Returns 0 on
 * success (device present), -1 on timeout/failure. */
static int port_reset_and_wait(volatile struct hba_port *port, int portno)
{
    stop_port(port);

    /* Write COMRESET (DET = 1) into PxSCTL */
    uint32_t sctl = port->sctl & ~0xF;
    sctl |= 1; /* DET = 1 */
    port->sctl = sctl;

    /* small delay to let link reset */
    delay(10000);

    int t = 500000;
    while (t--) {
        uint32_t ssts = port->ssts;
        uint8_t det = ssts & 0x0F;
        uint8_t ipm = (ssts >> 8) & 0x0F;
        if (det == 3 && ipm == 1) break;
        delay(100);
    }

    /* Clear errors and status */
    port->serr = (uint32_t)-1;
    port->is = (uint32_t)-1;
    delay(1000);

    /* Start port engine */
    if (start_port(port) != 0) {
        dump_port_status(port, portno);
        return -1;
    }

    /* Check if device present */
    uint32_t ssts = port->ssts;
    uint8_t det = ssts & 0x0F;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    if (det == 3 && ipm == 1) return 0;

    dump_port_status(port, portno);
    return -1;
}

static int find_cmdslot(volatile struct hba_port *port)
{
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; ++i) {
        if ((slots & (1u << i)) == 0) return i;
    }
    return -1;
}

int ahci_read(uintptr_t abar, int port, uint64_t lba, uint16_t count, void* out_buf, size_t out_len)
{
    if (!out_buf) return -1;
    if (count == 0 || count > 8) return -1;
    if (out_len < (size_t)count * 512) return -1;

    volatile struct hba_mem *hba = (volatile struct hba_mem*)abar;
    volatile struct hba_port *p  = &hba->ports[port];

    struct ahci_controller *ctrl = ahci_get_controller(abar);
    if (!ctrl) return -1;

    struct ahci_port_state *st = &ctrl->ports[port];
    if (!st->initialized) return -1;

    /* Ensure the port is running */
    if (start_port(p) != 0) {
        kprintf("ahci: failed to start port %d\n", port);
        return -1;
    }

    /* Wait until the device is not busy / not requesting data */
    /* (PxTFD: BSY=bit7, DRQ=bit3) */
    {
        int spin = 1000000;
        while (spin--) {
            uint32_t tfd = p->tfd;
            if ((tfd & (1u << 7)) == 0 && (tfd & (1u << 3)) == 0) break;
            delay(1);
        }
        /* If still busy, bail */
        if ((p->tfd & (1u << 7)) || (p->tfd & (1u << 3))) {
            kprintf("ahci: port %d still busy (tfd=%x)\n", port, p->tfd);
            return -1;
        }
    }

    /* Clear pending interrupt + error bits */
    p->is   = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;

    /* Find a free command slot */
    int slot = find_cmdslot(p);
    if (slot < 0 || slot >= 32) {
        kprintf("ahci: no free cmd slot on port %d\n", port);
        return -1;
    }

    /* Command list (32 headers) */
    struct hba_cmd_header *cmdheader =
        (struct hba_cmd_header*)PHYS_TO_VIRT(st->clb_ph);

    /* Use the selected slot (DO NOT wipe all 32 entries every time) */
    memset(&cmdheader[slot], 0, sizeof(cmdheader[slot]));
    cmdheader[slot].cfl   = sizeof(struct fis_h2d) / 4; /* dwords */
    cmdheader[slot].w     = 0;                          /* read */
    cmdheader[slot].prdtl = 1;

    /* One 4KiB command table per slot */
    uint64_t ct_ph = st->ct_ph + ((uint64_t)slot * 4096ull);
    struct hba_cmd_tbl *cmdtbl = (struct hba_cmd_tbl*)PHYS_TO_VIRT(ct_ph);
    memset(cmdtbl, 0, 4096);

    cmdheader[slot].ctba  = (uint32_t)ct_ph;
    cmdheader[slot].ctbau = (uint32_t)(ct_ph >> 32);

    /* PRDT entry (single contiguous buffer) */
    cmdtbl->prdt[0].dba  = (uint32_t)st->buf_ph;
    cmdtbl->prdt[0].dbau = (uint32_t)(st->buf_ph >> 32);
    cmdtbl->prdt[0].dbc  = ((uint32_t)count * 512u) - 1u; /* bytes - 1 */
    cmdtbl->prdt[0].dbc |= (1u << 31);                    /* IOC */

    /* Build CFIS (H2D) */
    struct fis_h2d *cfis = (struct fis_h2d*)cmdtbl->cfis;
    memset(cfis, 0, sizeof(*cfis));
    cfis->type    = 0x27;     /* FIS_TYPE_REG_H2D */
    cfis->c       = 1;        /* command */
    cfis->command = 0x25;     /* READ DMA EXT */
    cfis->device  = 1 << 6;   /* LBA mode */

    cfis->lba0 = (uint8_t)(lba & 0xFF);
    cfis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    cfis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    cfis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    cfis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    cfis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

    cfis->countl = (uint8_t)(count & 0xFF);
    cfis->counth = (uint8_t)((count >> 8) & 0xFF);

    /* Make sure HBA sees the prepared structures before setting CI */
    asm volatile("" ::: "memory");

    /* Issue command (OR-in, do not clobber) */
    p->ci |= (1u << slot);

    /* Poll for completion or task-file error */
    int t = 2000000;
    while (t--) {
        if ((p->ci & (1u << slot)) == 0) break;

        /* Task File Error Status in PxIS is bit 30 (TFES) */
        if (p->is & (1u << 30)) {
            kprintf("ahci: TFES on port %d (is=%x tfd=%x)\n", port, p->is, p->tfd);
            return -1;
        }

        delay(1);
    }

    if (p->ci & (1u << slot)) {
        kprintf("ahci: read timeout on port %d (ci=%x is=%x tfd=%x)\n",
                port, p->ci, p->is, p->tfd);
        return -1;
    }

    /* Copy out */
    void *buf_v = PHYS_TO_VIRT(st->buf_ph);
    memcpy(out_buf, buf_v, (size_t)count * 512);

    return 0;
}

void hexdump8(const void *buf, size_t len)
{
    const uint8_t *b = (const uint8_t*)buf;
    for (size_t i = 0; i < len; i += 16) {
        kprintf("%08x: ", (unsigned int)i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) {
                kprintf("%02x ", b[i + j]);
            } else {
                kprintf("   ");
            }
        }
        kprintf(" ");
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) {
                uint8_t c = b[i + j];
                if (c >= 32 && c <= 126) {
                    kprintf("%c", c);
                } else {
                    kprintf(".");
                }
            }
        }
        kprintf("\n");
    }
}

/* Internal identify implementation that uses per-port persistent buffers */
static int ahci_identify_port_internal(volatile struct hba_mem *abar, int portno, void *out_buf, size_t out_len)
{
    volatile struct hba_port *port = &abar->ports[portno];
    uint32_t ssts = port->ssts;
    uint8_t det = ssts & 0x0F;
    if (det != 3) return -1; /* no device */

    struct ahci_controller *ctrl = ahci_get_controller((uintptr_t)abar);
    if (!ctrl) return -1;
    struct ahci_port_state *st = &ctrl->ports[portno];

    if (!st->initialized) {
        /* allocate persistent CLB and FIS receive buffers */
        st->clb_ph = pmm_alloc_frame(); if (!st->clb_ph) return -1;
        void *clb = PHYS_TO_VIRT(st->clb_ph); memset(clb, 0, 4096);
        port->clb = (uint32_t)st->clb_ph; port->clbu = (uint32_t)(st->clb_ph >> 32);

        st->fb_ph = pmm_alloc_frame(); if (!st->fb_ph) return -1;
        void *fb = PHYS_TO_VIRT(st->fb_ph); memset(fb, 0, 4096);
        port->fb = (uint32_t)st->fb_ph; port->fbu = (uint32_t)(st->fb_ph >> 32);

        st->ct_ph = pmm_alloc_frame(); if (!st->ct_ph) return -1;
        void *ct = PHYS_TO_VIRT(st->ct_ph); memset(ct, 0, 4096);

        st->buf_ph = pmm_alloc_frame(); if (!st->buf_ph) return -1;
        void *buf = PHYS_TO_VIRT(st->buf_ph); memset(buf, 0, 4096);

        st->initialized = 1;
    }

    /* (Re)start the port engine to ensure it will process commands */
    if (start_port(port) != 0) {
        kprintf("ahci: failed to start port %d\n", portno);
        return -1;
    }

    /* prepare command list entry 0 */
    struct hba_cmd_header *cmdheader = (struct hba_cmd_header*)PHYS_TO_VIRT(st->clb_ph); /* slot 0 */
    memset((void*)cmdheader, 0, sizeof(struct hba_cmd_header) * 32);
    cmdheader[0].cfl = sizeof(struct fis_h2d) / 4; /* CFIS length in dwords */
    cmdheader[0].w = 0; /* read from device */
    cmdheader[0].prdtl = 1;

    /* setup command table */
    struct hba_cmd_tbl *cmdtbl = (struct hba_cmd_tbl*)PHYS_TO_VIRT(st->ct_ph);
    memset((void*)cmdtbl, 0, 4096);
    cmdheader[0].ctba = (uint32_t)st->ct_ph;
    cmdheader[0].ctbau = (uint32_t)(st->ct_ph >> 32);

    /* setup PRDT entry */
    cmdtbl->prdt[0].dba = (uint32_t)st->buf_ph;
    cmdtbl->prdt[0].dbau = (uint32_t)(st->buf_ph >> 32);
    cmdtbl->prdt[0].dbc = 512 - 1; /* bytes to transfer - 1 */
    cmdtbl->prdt[0].dbc |= (1u<<31); /* IOC */

    /* setup CFIS (H2D) */
    struct fis_h2d *cfis = (struct fis_h2d*)cmdtbl->cfis;
    memset(cfis, 0, sizeof(*cfis));
    cfis->type = 0x27;
    cfis->c = 1; /* command */

    uint32_t sig = port->sig;
    if (sig == 0xEB140101) {
        /* SATAPI: use IDENTIFY PACKET DEVICE (0xA1) and mark ATAPI in header */
        cmdheader[0].a = 1;
        cfis->command = 0xA1; /* IDENTIFY PACKET DEVICE */
        /* ATAPI may use acmd area for packet commands; zero it for now */
        memset(cmdtbl->acmd, 0, sizeof(cmdtbl->acmd));
    } else {
        cfis->command = 0xEC; /* IDENTIFY DEVICE */
    }
    cfis->device = 0;

    int done = 0;
    int attempts = 2;
    void *buf_v = PHYS_TO_VIRT(st->buf_ph);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        /* clear pending interrupt */
        port->is = (uint32_t)-1;

        /* find free slot */
        int slot = find_cmdslot(port);
        if (slot < 0) {
            kprintf("ahci: no free cmd slot on port %d\n", portno);
            return -1;
        }

        /* issue command */
        port->ci = (1u << slot);

        /* wait for completion */
        int t = 1000000;
        while (port->ci & (1u << slot)) {
            if (port->is & (1u<<30)) { /* error */ break; }
            if (--t == 0) { kprintf("ahci: identify timeout on port %d (attempt %d)\n", portno, attempt); break; }
        }

        /* check for completion/error */
        if (port->is & (1u<<30)) {
            kprintf("ahci: identify error on port %d attempt %d\n", portno, attempt);
            dump_port_status(port, portno);
            /* try resetting the port once */
            if (attempt + 1 < attempts) {
                kprintf("ahci: attempting port_reset on port %d\n", portno);
                if (port_reset_and_wait(port, portno) == 0) continue;
            }
            return -1;
        }

        /* copy out data if requested */
        if (out_buf && out_len >= 512) memcpy(out_buf, buf_v, 512);

        done = 1;
        break;
    }

    if (!done) return -1;

    /* parse identify data */
    uint16_t *id = (uint16_t*)buf_v;
    char model[41];
    for (int i = 0; i < 20; ++i) {
        uint16_t w = id[27 + i];
        model[i*2] = (char)(w >> 8);
        model[i*2 + 1] = (char)(w & 0xFF);
    }
    model[40] = '\0';
    /* trim trailing spaces */
    for (int i = 39; i >= 0 && model[i] == ' '; --i) model[i] = '\0';

    uint32_t word60 = id[60] | (id[61] << 16);
    uint64_t sectors = (uint64_t)word60;

    /* If IDENTIFY returned empty info, try ATAPI INQUIRY as a fallback (covers SATAPI) */
    if ((model[0] == '\0' || sectors == 0)) {
        kprintf("ahci: identify returned empty on port %d sig=0x%08x, trying ATAPI INQUIRY\n", portno, sig);
        /* issue ATAPI PACKET INQUIRY (SCSI CDB 0x12) */
        memset((void*)cmdtbl, 0, 4096);
        cmdheader[0].ctba = (uint32_t)st->ct_ph;
        cmdheader[0].ctbau = (uint32_t)(st->ct_ph >> 32);
        cmdheader[0].a = 1; /* ATAPI */
        cmdheader[0].cfl = sizeof(struct fis_h2d) / 4;
        cmdheader[0].w = 0; /* data-in */
        cmdheader[0].prdtl = 1;

        struct fis_h2d *cfis2 = (struct fis_h2d*)cmdtbl->cfis;
        memset(cfis2, 0, sizeof(*cfis2));
        cfis2->type = 0x27;
        cfis2->c = 1;
        cfis2->command = 0xA0; /* PACKET */
        cfis2->device = 0;
        
        /* CRITICAL FIX: Set feature and byte count registers for ATAPI PACKET command */
        cfis2->featurel = 0x01; /* PIO data-in transfer */
        cfis2->featureh = 0x00;
        cfis2->lba1 = 36;       /* Byte count limit low (cylinder low) */
        cfis2->lba2 = 0;        /* Byte count limit high (cylinder high) */
        cfis2->lba0 = 0;
        cfis2->lba3 = 0;
        cfis2->lba4 = 0;
        cfis2->lba5 = 0;
        cfis2->countl = 0;      /* Tag (optional) */
        cfis2->counth = 0;

        /* SCSI INQUIRY 6-byte CDB in acmd area */
        uint8_t *acmd = cmdtbl->acmd;
        memset(acmd, 0, sizeof(cmdtbl->acmd));
        acmd[0] = 0x12; /* INQUIRY */
        acmd[4] = 36;   /* allocation length */

        cmdtbl->prdt[0].dba = (uint32_t)st->buf_ph;
        cmdtbl->prdt[0].dbau = (uint32_t)(st->buf_ph >> 32);
        cmdtbl->prdt[0].dbc = 36 - 1;
        cmdtbl->prdt[0].dbc |= (1u<<31);

        port->is = (uint32_t)-1;
        int slot2 = find_cmdslot(port);
        if (slot2 >= 0) {
            int found = 0;
            for (int attempt2 = 0; attempt2 < 2; ++attempt2) {
                port->ci = (1u << slot2);
                int t = 1000000;
                while (port->ci & (1u << slot2)) {
                    if (port->is & (1u<<30)) break;
                    if (--t == 0) { kprintf("ahci: ATAPI inquiry timeout on port %d\n", portno); break; }
                }

                if (port->is & (1u<<30)) {
                    kprintf("ahci: ATAPI inquiry error on port %d attempt %d\n", portno, attempt2);
                    dump_port_status(port, portno);
                    if (attempt2 + 1 < 2) {
                        kprintf("ahci: ATAPI inquiry retry: resetting port %d\n", portno);
                        if (port_reset_and_wait(port, portno) == 0) continue;
                    }
                } else {
                    void *inq = PHYS_TO_VIRT(st->buf_ph);
                    char vendor[9]; char product[17];
                    memset(vendor, 0, sizeof(vendor)); memset(product, 0, sizeof(product));
                    /* Vendor ID at offset 8, product at 16 (standard SCSI INQUIRY) */
                    memcpy(vendor, (uint8_t*)inq + 8, 8);
                    memcpy(product, (uint8_t*)inq + 16, 16);
                    for (int i = 7; i >= 0 && vendor[i] == ' '; --i) vendor[i] = '\0';
                    for (int i = 15; i >= 0 && product[i] == ' '; --i) product[i] = '\0';
                    kprintf("  ATAPI INQUIRY: vendor='%s' product='%s'\n", vendor, product);
                    /* prefer product as model */
                    snprintf(model, sizeof(model), "%s %s", vendor, product);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                kprintf("ahci: ATAPI inquiry fully failed on port %d, marking as ATAPI device\n", portno);
                snprintf(model, sizeof(model), "ATAPI device");
            }
        }
    }

    kprintf("  IDENTIFY: model='%s' sectors=%llu\n", model, (unsigned long long)sectors);
    uint8_t sec[512];
    if (ahci_read((uintptr_t)abar, portno, 0, 1, sec, sizeof(sec)) == 0) {
        /* suppressed noisy hex dump */
        uint8_t mbr[512];
        if (ahci_read((uintptr_t)abar, portno, 0, 1, mbr, sizeof(mbr)) != 0) {
            kprintf("read MBR failed\n");
            return -1;
        }
        kprintf("MBR sig: %02x %02x\n", (unsigned)mbr[510], (unsigned)mbr[511]);

        /* Register a block device name for this disk (sda, sdb, ...) */
        static int disk_count = 0;
        char disk_name[8];
        disk_name[0] = 's'; disk_name[1] = 'd'; disk_name[2] = 'a' + (char)disk_count; disk_name[3] = '\0';
        disk_count++;
        /* register disk */
        extern int block_register_disk(const char*, uintptr_t, int);
        block_register_disk(disk_name, (uintptr_t)abar, portno);

        // List and register all 4 primary partitions
        for (int i = 0; i < 4; ++i) {
            uint8_t *ent_ptr = mbr + 0x1BE + i * 16;
            uint8_t type = ent_ptr[4];
            if (type == 0)
                continue; // skip empty partition
            uint32_t lba_start = (uint32_t)ent_ptr[8]  | ((uint32_t)ent_ptr[9] << 8) |
                                ((uint32_t)ent_ptr[10] << 16) | ((uint32_t)ent_ptr[11] << 24);
            uint32_t lba_cnt   = (uint32_t)ent_ptr[12] | ((uint32_t)ent_ptr[13] << 8) |
                                ((uint32_t)ent_ptr[14] << 16) | ((uint32_t)ent_ptr[15] << 24);
            kprintf("Partition %d: type=%02x start=%u count=%u\n", i, (unsigned)type, (unsigned)lba_start, (unsigned)lba_cnt);
            extern int block_register_partition(const char*, int, uint64_t, uint64_t);
            block_register_partition(disk_name, i+1, (uint64_t)lba_start, (uint64_t)lba_cnt);
            /* register device node, e.g. /dev/sda1 */
            char devnode[16];
            size_t dn = 0; for (; dn + 1 < sizeof(devnode) && disk_name[dn]; ++dn) devnode[dn] = disk_name[dn];
            devnode[dn] = '\0';
            char full[20]; size_t fp = 0; full[fp++] = '/'; full[fp++] = 'd'; full[fp++] = 'e'; full[fp++] = 'v'; full[fp++] = '/';
            for (size_t j = 0; j + 1 < sizeof(full) && devnode[j]; ++j) full[fp++] = devnode[j];
            /* append partition number (1-based) */
            full[fp++] = '0' + (char)(i + 1);
            full[fp] = '\0';
            extern int dev_register(const char*, int, void*, size_t);
            dev_register(full, DEV_TYPE_BLOCK, NULL, 0);
        }
    
    } else {
        kprintf("  ahci: read test sector 0 FAILED on port %d\n", portno);
    }
    return 0;
}

/* Public wrapper matching header */
int ahci_identify_port(uintptr_t abar, int port, void* out_buf, size_t out_len)
{
    volatile struct hba_mem *abarv = (volatile struct hba_mem*)(uintptr_t)abar;
    return ahci_identify_port_internal(abarv, port, out_buf, out_len);
}

int ahci_attach(struct pci_device *dev)
{
    if (!dev) return -1;
    /* find ABAR */
    uint64_t abar = 0;
    for (int b = 0; b < 6; ++b) {
        if (dev->bar_size[b] == 0) continue;
        if (dev->bar_is_io[b]) continue;
        if (dev->bar_virt[b]) { abar = dev->bar_virt[b]; break; }
    }
    if (!abar) return -1;

    volatile struct hba_mem *abarv = (volatile struct hba_mem*)(uintptr_t)abar;
    uint32_t pi = abarv->pi;

    kprintf("ahci: attach dev %02x:%02x.%x PI=0x%08x\n", dev->bus, dev->device, dev->function, pi);

    for (int p = 0; p < 32; ++p) {
        if (pi & (1u<<p)) {
            ahci_identify_port((uintptr_t)abarv, p, NULL, 0);
        }
    }

    return 0;
}