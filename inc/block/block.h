#pragma once
#include <stdint.h>
#include <stddef.h>

/* Simple block device registry used by higher-level filesystems */
int block_register_disk(const char *name, uintptr_t abar, int port);
int block_register_partition(const char *disk_name, int idx, uint64_t start, uint64_t count);

/* Read sectors from a block device by name (count in sectors) */
int block_read(const char *name, uint64_t lba, uint16_t count, void *out_buf, size_t out_len);

/* Find partition LBA start by name, return 0 on success and writes start/count */
int block_get_partition(const char *name, uint64_t *out_start, uint64_t *out_count);
