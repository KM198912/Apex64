#include <kernel/kprintf.h>
#include <drivers/fb.h>
#include <lib/debug.h>
int kprintf(const char *fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    e9_puts(buffer);
    if (global_fb && global_fb->ft_ctx) {
        flanterm_write(global_fb->ft_ctx, buffer, len);
    }

    return len;
}

void _putchar(char c) {
    if (global_fb && global_fb->ft_ctx) {
        flanterm_write(global_fb->ft_ctx, &c, 1);
    }
}