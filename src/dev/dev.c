#include <dev/dev.h>
#include <lib/alloc.h>
#include <lib/string.h>
#include <kernel/kprintf.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_DEVICES 32
static struct dev_entry devs[MAX_DEVICES];

int dev_register(const char *name, int type, void *data, size_t size)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (devs[i].name == NULL) {
            char *n = strdup(name);
            if (!n) return -1;
            devs[i].name = n;
            devs[i].type = type;
            devs[i].data = data;
            devs[i].size = size;
            klog(1, "dev: registered %s type=%d size=%zu\n", n, type, size);
            return 0;
        }
    }
    return -1;
}

struct dev_entry *dev_get(const char *name)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (devs[i].name && strcmp(devs[i].name, name) == 0) return &devs[i];
    }
    return NULL;
}
