#include <common/boot.h>
#include <drivers/fb.h>
#include <kernel/kprintf.h>
#include <common/multiboot2.h>

const char* memory_type_to_string(uint32_t type) {
    switch (type) {
        case MULTIBOOT_MEMORY_AVAILABLE: return "Available";
        case MULTIBOOT_MEMORY_RESERVED: return "Reserved";
        case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
        case MULTIBOOT_MEMORY_NVS: return "NVS";
        case MULTIBOOT_MEMORY_BADRAM: return "Bad RAM";
        default: return "Unknown";
    }
}

static void print_memory_map(void) {
    uint64_t mb = TitanBootInfo.mb2_addr;
    if (!mb) {
        kprintf("No multiboot info available\n");
        return;
    }
    uint64_t total_usable = 0;
    struct multiboot_tag *tag = (struct multiboot_tag*)PHYS_TO_VIRT(mb + 8);

    /* compute tag sizes using padded tag sizes and include the end tag */
    uint32_t computed = 0;
    int tag_count = 0;
    for (struct multiboot_tag *t = tag; ; t = (struct multiboot_tag*)((uint8_t*)t + ((t->size + 7) & ~7))) {
        uint32_t padded = (t->size + 7) & ~7U;
        computed += padded;
        tag_count++;
        if (t->type == MULTIBOOT_TAG_TYPE_END) break;
    }
    /* optional warning for mismatches */
    if (computed != ((*(uint32_t*)PHYS_TO_VIRT(mb)) - 8)) {
        kprintf("WARNING: tag sizes mismatch by %d bytes\n", (int)((*(uint32_t*)PHYS_TO_VIRT(mb)) - 8 - computed));
    }

    for (; tag->type != MULTIBOOT_TAG_TYPE_END; tag = (struct multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mm = (struct multiboot_tag_mmap*)tag;
            kprintf("Memory map: entry_size=%u entry_version=%u\n", mm->entry_size, mm->entry_version);
            uint32_t entries_len = mm->size - sizeof(struct multiboot_tag_mmap);
            uint8_t *ptr = (uint8_t*)mm->entries;
            for (uint32_t off = 0; off + mm->entry_size <= entries_len; off += mm->entry_size) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry*)(ptr + off);
                kprintf("  base=0x%016llx len=0x%016llx type=%s\n", (unsigned long long)e->addr, (unsigned long long)e->len, memory_type_to_string(e->type));
                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    total_usable += e->len;
                }
            }
                    kprintf("Total usable memory: %llu bytes (%.2f MB)\n", (unsigned long long)total_usable, (double)total_usable / (1024.0 * 1024.0));
            return;
        }
    }

    kprintf("No memory map tag found\n");
}

#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mem/slab.h>
#include <lib/alloc.h>
#include <drivers/cmdline.h>
#include <block/block.h>
#include <fs/vfs.h>
#include <fs/ustar.h>
#include <fs/ext2.h>
#include <dev/dev.h>
#include <bus/pci.h>
#include <lib/string.h>

void kernel_main()
{
    framebuffer_early_init();
    kprintf(LOG_OK "Kernel initialized successfully!\n");
    // print memory map
    print_memory_map();
    //print acpi address
    kprintf("ACPI RSDP pointer: %p\n", TitanBootInfo.acpi_ptr);
    kprintf("Kernel size: %zu bytes (%.2f MB)\n", (size_t)TitanBootInfo.kernel_size, (double)TitanBootInfo.kernel_size / (1024.0 * 1024.0));

    kprintf("Initializing PMM...\n");
    pmm_init(TitanBootInfo.mb2_addr);
    kprintf("PMM free frames: %zu\n", pmm_free_count());

    /* self-test: allocate 3 frames, print them, free them */
    uint64_t a = pmm_alloc_frame();
    uint64_t b = pmm_alloc_frame();
    uint64_t c = pmm_alloc_frame();
    kprintf("pmm_test: allocs: a=0x%016llx b=0x%016llx c=0x%016llx\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    if (a) pmm_free_frame(a);
    if (b) pmm_free_frame(b);
    if (c) pmm_free_frame(c);
    kprintf("pmm_test: free frames after test: %zu\n", pmm_free_count());

    /* VMM init and simple map test */
    kprintf("Initializing VMM...\n");
    vmm_init();
    uint64_t test_virt = 0xFFFFFFFF80000000ULL + (128 * 1024 * 1024) + 0x1000; /* KERNEL_VIRT_BASE + KERNEL_MAP_SIZE + 4K */
    uint64_t test_phys = pmm_alloc_frame();
    if (!test_phys) kprintf("vmm_test: no free frames\n");
    else {
        if (vmm_map_page(test_virt, test_phys, VMM_PTE_W) == 0) {
            volatile uint64_t *ptr = (volatile uint64_t*)test_virt;
            *ptr = 0xDEADBEEFCAFEBABEULL;
            uint64_t v = *ptr;
            kprintf("vmm_test: wrote/read 0x%016llx at virt 0x%016llx -> phys 0x%016llx\n", (unsigned long long)v, (unsigned long long)test_virt, (unsigned long long)test_phys);
            vmm_unmap_page(test_virt);
            pmm_free_frame(test_phys);
        } else kprintf("vmm_test: map failed\n");
    }

    /* Slab allocator test */
    slab_init();
    kprintf("slab: test allocating various sizes...\n");
    void *a1 = slab_alloc(20); /* 32 */
    void *a2 = slab_alloc(100); /* 128 */
    void *a3 = slab_alloc(2000); /* 2048 */
    if (a1) { memset(a1, 0xAA, 20); kprintf("slab: alloc 20 -> %p\n", a1); }
    if (a2) { memset(a2, 0xBB, 100); kprintf("slab: alloc 100 -> %p\n", a2); }
    if (a3) { memset(a3, 0xCC, 2000); kprintf("slab: alloc 2000 -> %p\n", a3); }
    slab_free(a1);
    slab_free(a2);
    slab_free(a3);
    /* allocate/free a bit more to fill magazines */
    for (int i = 0; i < SLAB_MAGAZINE_SIZE; ++i) {
        void *x = slab_alloc(20);
        slab_free(x);
    }

    kprintf("slab: free objects (32) = %zu\n", slab_free_objects(32));
    kprintf("slab: free objects (128) = %zu\n", slab_free_objects(128));
    kprintf("slab: free objects (2048) = %zu\n", slab_free_objects(2048));

    /* kmalloc tests */
    kprintf("kmalloc: testing small allocation (100)\n");
    void *ks = kmalloc(100);
    if (ks) { memset(ks, 0x11, 100); kprintf(" kmalloc small -> %p\n", ks); kfree(ks); }
    else kprintf("kmalloc small failed\n");

    kprintf("kmalloc: testing large allocation (7000)\n");
    void *kl = kmalloc(7000);
    if (kl) { memset(kl, 0x22, 7000); kprintf(" kmalloc large -> %p\n", kl); kfree(kl); }
    else kprintf("kmalloc large failed\n");

    /* PCI enumeration */
    kprintf("Initializing PCI...\n");
    pci_init();
    pci_print_devices();

    /* Map MMIO BARs (via HHDM for now) and probe drivers */
    for (int i = 0; i < pci_get_device_count(); ++i) {
        struct pci_device *d = pci_get_device(i);
        pci_map_device_bars(d);
    }
    /* register builtin drivers and probe devices */
    pci_register_builtin_drivers();
    pci_probe_devices();
    //list modules
    size_t module_count = TitanBootInfo.module_count;
    kprintf("Modules loaded: %zu\n", module_count);
    for (size_t i = 0; i < module_count; ++i) {
        void *mod_ptr = TitanBootInfo.modules[i];
        size_t mod_size = TitanBootInfo.module_sizes[i];
        char* mod_path = TitanBootInfo.module_path[i];

        if (!mod_ptr) {
            kprintf(" Module %d: (null module pointer)\n", i);
            continue;
        }

        typedef struct ustar_header {
            char name[100];
            char mode[8];
            char uid[8];
            char gid[8];
            char size[12];
            char mtime[12];
            char checksum[8];
            char typeflag;
            char linkname[100];
            char magic[6];
            char version[2];
            char uname[32];
            char gname[32];
            char devmajor[8];
            char devminor[8];
            char prefix[155];
            char padding[12];
        } ustar_header_t;
        /* ensure module is large enough to contain a ustar header */
        static const char USTAR_MAGIC[6] = { 'u','s','t','a','r','\0' };
        static const char USTAR_VER[2]   = { '0','0' };
        /* Only access header bytes if module is at least 512 bytes */
        if (mod_size >= 512) {


            const ustar_header_t *h = (const ustar_header_t *)mod_ptr;

            if (memcmp(h->magic, USTAR_MAGIC, 6) == 0 &&
                memcmp(h->version, USTAR_VER, 2) == 0) {
                kprintf(" Module %zu: USTAR archive at %p, size=%zu bytes\n", i, mod_ptr, mod_size);
                //list the files in the archive
                size_t offset = 0;
                while (offset + sizeof(ustar_header_t) <= mod_size) {
                    const ustar_header_t *fh = (const ustar_header_t *)((uint8_t*)mod_ptr + offset);
                    if (fh->name[0] == '\0') break; // end of archive
                    //get file size
                    size_t fsize = 0;
                    for (int j = 0; j < 11; ++j) {
                        char c = fh->size[j];
                        if (c < '0' || c > '7') break;
                        fsize = (fsize << 3) | (c - '0');
                    }
                    kprintf("   File: %s, size=%zu bytes\n", fh->name, fsize);
                    //advance to next header (file data is aligned to 512 bytes)
                    size_t total_size = sizeof(ustar_header_t) + ((fsize + 511) & ~511);
                    offset += total_size;
                    if (total_size == 0) break; // prevent infinite loop
                }
                continue;
            }
        }

        kprintf(" Module %zu: %s at %p, size=%zu bytes\n", i, mod_path ? mod_path : "(no path)", mod_ptr, mod_size);
    }
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
                    }                } else {
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