#include <block/block.h>
#include <lib/string.h>
#include <kernel/kprintf.h>
#include <drivers/ahci.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/alloc.h>

#define MAX_BLOCKS 8

struct block_dev {
    char name[16];
    uintptr_t abar;
    int port;
    uint64_t start_lba;
    uint64_t count;
    int is_partition;
};

static struct block_dev blocks[MAX_BLOCKS];

int block_register_disk(const char *name, uintptr_t abar, int port)
{
    for (int i = 0; i < MAX_BLOCKS; ++i) {
        if (blocks[i].name[0] == '\0') {
            size_t j = 0; for (; j+1 < sizeof(blocks[i].name) && name[j]; ++j) blocks[i].name[j] = name[j];
            blocks[i].name[j] = '\0';
            blocks[i].abar = abar;
            blocks[i].port = port;
            blocks[i].start_lba = 0;
            blocks[i].count = 0;
            blocks[i].is_partition = 0;
            klog(1, "block: registered disk %s (abar=%p port=%d)\n", blocks[i].name, (void*)abar, port);
            return 0;
        }
    }
    return -1;
}

int block_register_partition(const char *disk_name, int idx, uint64_t start, uint64_t count)
{
    /* find disk to base name on */
    char name[16];
    size_t j = 0; for (; j+1 < sizeof(name) && disk_name[j]; ++j) name[j] = disk_name[j];
    name[j] = '\0';
    /* append idx */
    size_t len = strlen(name);
    name[len] = '0' + (char)idx; name[len+1] = '\0';

    for (int i = 0; i < MAX_BLOCKS; ++i) {
        if (blocks[i].name[0] == '\0') {
            size_t k = 0; for (; k+1 < sizeof(blocks[i].name) && name[k]; ++k) blocks[i].name[k] = name[k];
            blocks[i].name[k] = '\0';
            /* copy from parent disk */
            for (int p = 0; p < MAX_BLOCKS; ++p) {
                if (strcmp(blocks[p].name, disk_name) == 0) {
                    blocks[i].abar = blocks[p].abar;
                    blocks[i].port = blocks[p].port;
                }
            }
            blocks[i].start_lba = start;
            blocks[i].count = count;
            blocks[i].is_partition = 1;
            klog(1, "block: registered partition %s start=%llu count=%llu\n", blocks[i].name, (unsigned long long)start, (unsigned long long)count);
            return 0;
        }
    }
    return -1;
}

static struct block_dev *find_block(const char *name)
{
    for (int i = 0; i < MAX_BLOCKS; ++i) if (strcmp(blocks[i].name, name) == 0) return &blocks[i];
    return NULL;
}

int block_read(const char *name, uint64_t lba, uint16_t count, void *out_buf, size_t out_len)
{
    struct block_dev *b = find_block(name);
    if (!b) { kprintf("block: read missing device %s\n", name); return -1; }
    uint64_t final = lba;
    if (b->is_partition) final = lba + b->start_lba;
    int r = ahci_read(b->abar, b->port, final, count, out_buf, out_len);
    if (r != 0) kprintf("block: read failed %s lba=%llu count=%u -> final_lba=%llu (err=%d)\n", name, (unsigned long long)lba, (unsigned)count, (unsigned long long)final, r);
    return r;
}
int block_get_partition(const char *name, uint64_t *out_start, uint64_t *out_count)
{
    struct block_dev *b = find_block(name);
    if (!b) return -1;
    if (!b->is_partition) return -1;
    if (out_start) *out_start = b->start_lba;
    if (out_count) *out_count = b->count;
    return 0;
}
