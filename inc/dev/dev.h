#pragma once
#include <stddef.h>

#define DEV_TYPE_BLOCK 1
#define DEV_TYPE_CHAR  2
#define DEV_TYPE_SPECIAL 3

struct dev_entry {
    char *name; /* e.g. "/dev/sda1" */
    int type;
    void *data; /* optional pointer to device-specific data */
    size_t size; /* optional size (for initrd etc) */
};

/* Register a device. Returns 0 on success, -1 on error. The name will be copied. */
int dev_register(const char *name, int type, void *data, size_t size);

/* Lookup a device by name. Returns pointer or NULL. */
struct dev_entry *dev_get(const char *name);
