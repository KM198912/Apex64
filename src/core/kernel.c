#include <common/boot.h>
#include <drivers/fb.h>
#include <kernel/kprintf.h>
#include <common/multiboot2.h>
#include <drivers/gdt.h>
#include <drivers/idt.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mem/slab.h>
#include <lib/alloc.h>
#include <drivers/cmdline.h>
#include <block/block.h>
#include <fs/vfs.h>
#include <fs/ustar.h>
#include <fs/ext2.h>
#include <fs/fstab.h>
#include <dev/dev.h>
#include <bus/pci.h>
#include <lib/string.h>
#include <drivers/pit.h>

void kernel_main()
{
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

    /* Ensure we have a valid GDT/TSS and segments before any interrupts */
  
    framebuffer_early_init();
//      gdt_init(0);
  gdt_init(0);   interrupts_init(); asm volatile("sti"); pit_init();

    kprintf("Kernel initialized. RSP=%p\n", (void*)rsp);
    pmm_init(TitanBootInfo.mb2_addr);
    kprintf(LOG_OK "PMM initialized.\n");
    vmm_init();
    kprintf(LOG_OK "VMM initialized.\n");
    slab_init();
    kprintf(LOG_OK "Slab allocator initialized.\n");
    pci_init();
    kprintf(LOG_OK "PCI initialized.\n");
    pci_print_devices();
    /* Map MMIO BARs (via HHDM for now) and probe drivers */
    for (int i = 0; i < pci_get_device_count(); ++i) {
        struct pci_device *d = pci_get_device(i);
        pci_map_device_bars(d);
    }
    /* register builtin drivers and probe devices */
    pci_register_builtin_drivers();
    pci_probe_devices();
      char* root_part = cmdline_get("root");
    if (root_part) {
        kprintf("Root partition specified: %s\n", root_part);
    } else {
        kprintf("No root partition specified in command line.\n");
    }
    char* loglevel = cmdline_get("loglevel");
    if (loglevel) {
        int lv = atoi(loglevel);
        set_loglevel(lv);
        kprintf("Log level specified: %s -> %d\n", loglevel, lv);
        kfree(loglevel);
    } else {
        kprintf("No log level specified in command line.\n");
    }
  /* If initrd module present, register device and mount it at /initrd so it's always available */
    if (TitanBootInfo.module_count > 0) {
        void *mod = TitanBootInfo.modules[0];
        size_t modsz = TitanBootInfo.module_sizes[0];
        extern int dev_register(const char*, int, void*, size_t);
        /* register /dev/initrd (do this regardless of root mount success) */
        if (dev_register("/dev/initrd", DEV_TYPE_SPECIAL, mod, modsz) == 0) {
            struct dev_entry *d = dev_get("/dev/initrd");
            if (d) {
                klog(1, "dev: /dev/initrd registered (base=%p size=%zu)\n", d->data, d->size);
                /* mount at /initrd */
                struct vfs_ops *ops = ustar_get_ops();
                void *args[2]; args[0] = d->data; args[1] = (void*)d->size;
                if (vfs_mount("/initrd", ops, args) == 0) {
                    klog(1, "vfs: mounted initrd at /initrd\n");
                    /* try reading a test file from the mounted initrd */
                    char buf[256];
                    ssize_t rr = vfs_read_all("/initrd/test.txt", buf, sizeof(buf));
                    if (rr > 0) { buf[rr] = '\0'; kprintf("vfs: /initrd/test.txt contents: %s\n", buf); }
                    else kprintf("vfs: /initrd/test.txt not found\n");
                }
            }
        }
    }

    /* Try to mount root filesystem based on cmdline */
    if (root_part && root_part[0]) {
        klog(1, "Mount: requested root='%s'\n", root_part);
        if (strncmp(root_part, "/dev/", 5) == 0) {
            const char *devname = root_part + 5; /* e.g. sda1 */
            uint64_t start, cnt;
            if (block_get_partition(devname, &start, &cnt) == 0) {
                klog(1, "Mount: found partition %s start=%llu count=%llu\n", devname, (unsigned long long)start, (unsigned long long)cnt);
                /* attempt ext2 mount */
                struct vfs_ops *extops = ext2_get_ops();
                if (vfs_mount("/", extops, (void*)devname) == 0) {
                    klog(1, "Mount: ext2 mounted on / from %s\n", devname);
                    char buf[512];
                    ssize_t r = vfs_read_all("/test.txt", buf, sizeof(buf));
                    if (r > 0) { buf[r] = '\0'; kprintf("vfs: /test.txt contents: %s\n", buf); }
                    else kprintf("vfs: /test.txt not found on ext2\n");
                    /* FD API smoke test */
                    int fd = vfs_fd_open("/test.txt");
                    if (fd >= 0) {
                        char fbuf[256];
                        ssize_t rr = vfs_fd_read(fd, fbuf, 0, sizeof(fbuf)-1);
                        if (rr > 0) { fbuf[rr] = '\0'; kprintf("vfs fd read: %s\n", fbuf); }
                        else kprintf("vfs fd read failed (rr=%d)\n", (int)rr);
                        vfs_fd_close(fd);
                    } else {
                        kprintf("vfs fd open failed\n");
                    }
                    /* After mounting root, parse /etc/fstab to mount additional filesystems */
                    fstab_parse_and_mount("/etc/fstab");
                      vfs_list_dir("/mnt/data");
                } else {
                    klog(1, "Mount: ext2 mount failed on %s, falling back to initrd\n", devname);
                    /* try initrd as fallback: register device /dev/initrd then mount USTAR from it */
                    if (TitanBootInfo.module_count > 0) {
                        void *mod = TitanBootInfo.modules[0];
                        size_t modsz = TitanBootInfo.module_sizes[0];
                        /* register device */
                        extern int dev_register(const char*, int, void*, size_t);
                        dev_register("/dev/initrd", DEV_TYPE_SPECIAL, mod, modsz);
                        struct dev_entry *d = dev_get("/dev/initrd");
                        if (d) {
                            klog(1, "Mounting /dev/initrd -> base=%p size=%zu as USTAR\n", d->data, d->size);
                            struct vfs_ops *ops = ustar_get_ops();
                            void *args[2]; args[0] = d->data; args[1] = (void*)d->size;
                            if (vfs_mount("/", ops, args) == 0) {
                                char buf[256];
                                ssize_t r = vfs_read_all("/test.txt", buf, sizeof(buf));
                                if (r > 0) { buf[r] = '\0'; kprintf("vfs: /test.txt contents: %s\n", buf); }
                                else kprintf("vfs: /test.txt not found\n");
                                /* After mounting root from initrd fallback, attempt parsing /etc/fstab */
                                fstab_parse_and_mount("/etc/fstab");
                                  vfs_list_dir("/mnt/data");
                            }
                        }
                    }
                }
            } else {
                klog(1, "Mount: partition %s not found\n", devname);
            }
        } else if (strcmp(root_part, "initrd") == 0) {
            /* Mount first module as initrd ustar */
            if (TitanBootInfo.module_count > 0) {
                void *mod = TitanBootInfo.modules[0];
                size_t modsz = TitanBootInfo.module_sizes[0];
                kprintf("Mounting initrd module at %p size=%zu as USTAR\n", mod, modsz);
                struct vfs_ops *ops = ustar_get_ops();
                void *args[2]; args[0] = mod; args[1] = (void*)modsz;
                if (vfs_mount("/", ops, args) == 0) {
                    char buf[256];
                    ssize_t r = vfs_read_all("/test.txt", buf, sizeof(buf));
                    if (r > 0) {
                        buf[r] = '\0';
                        kprintf("vfs: /test.txt contents: %s\n", buf);
                    } else kprintf("vfs: /test.txt not found\n");
                    /* With / mounted from initrd, parse /etc/fstab for additional mounts */
                    fstab_parse_and_mount("/etc/fstab");
                    kprintf("Listing /mnt/data:\n");
                      vfs_list_dir("/mnt/data");
                }
            }
        }
    } else {
        kprintf("No root specified; attempting to mount initrd if available\n");
        if (TitanBootInfo.module_count > 0) {
            void *mod = TitanBootInfo.modules[0];
            size_t modsz = TitanBootInfo.module_sizes[0];
            kprintf("Mounting initrd module at %p size=%zu as USTAR\n", mod, modsz);
            struct vfs_ops *ops = ustar_get_ops();
            void *args[2]; args[0] = mod; args[1] = (void*)modsz;
            if (vfs_mount("/", ops, args) == 0) {
                char buf[256];
                ssize_t r = vfs_read_all("/test.txt", buf, sizeof(buf));
                if (r > 0) {
                    buf[r] = '\0';
                    kprintf("vfs: /test.txt contents: %s\n", buf);
                } else kprintf("vfs: /test.txt not found\n");
                /* With / mounted (initrd fallback), parse /etc/fstab */
                fstab_parse_and_mount("/etc/fstab");
                kprintf("Listing /mnt/data:\n");
                  vfs_list_dir("/mnt/data");
            }
        }
      
    }
}


void kernel_run()
{
    // Kernel main loop
    while (1) {
        __asm__ volatile("hlt");
    }
}