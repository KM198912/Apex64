#include <kernel/kprintf.h>
#include <drivers/fb.h>
#include <lib/debug.h>
#include <drivers/pit.h>
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

int print_with_timeout(uint64_t secs, bool condition, const char *fmt, ...) {
//basically the same as kprintf but with a timeout, and on success in the timeout, we print with LOG_OK, and on failure we print with LOG_ERROR
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    uint64_t start = pit_get_ticks();
    while ((pit_get_ticks() - start) < (secs * 1000)) {
        if (condition) {
            kprintf(LOG_OK, "%s\n", buffer);
            return 1; //success
        }
        __asm__ volatile("hlt");
    }
    kprintf(LOG_ERROR, "%s\n", buffer);
    return 0; //failure
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