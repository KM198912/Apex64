#include <lib/debug.h>
#include <lib/sys/io.h>

void e9_puts(const char *str) {
    while (*str) {
        outb(QEMU_DEBUG_PORT, (uint8_t)(*str));
        str++;
    }
}