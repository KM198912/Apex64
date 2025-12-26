#include <kernel/kprintf.h>
#include <drivers/fb.h>
#include <lib/debug.h>
static int kernel_loglevel = 1; /* 0 = verbose, higher = quieter, 3 = nothing */

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

int klog(int level, const char *fmt, ...) {
    if (level < kernel_loglevel) return 0; /* suppressed by loglevel */
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

void set_loglevel(int l) { kernel_loglevel = l; }
int get_loglevel(void) { return kernel_loglevel; }

void _putchar(char c) {
    if (global_fb && global_fb->ft_ctx) {
        flanterm_write(global_fb->ft_ctx, &c, 1);
    }
}