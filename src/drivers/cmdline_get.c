#include <common/boot.h>
#include <kernel/kprintf.h>
#include <lib/string.h>
#include <stddef.h>

/* strdup is defined in lib/string.c */
char *strdup(const char *s);

char *cmdline_get(const char *src)
{
    const char* cmd_line = TitanBootInfo.cmdline;
    if (!cmd_line) {
        kprintf("No command line available\n");
        return NULL;
    }
    const char* p = cmd_line;
    size_t cmd_len = strlen(src);
    while (*p) {
        while (*p == ' ') p++;
        if (strncmp(p, src, cmd_len) == 0 && p[cmd_len] == '=') {
            p += cmd_len + 1;
            char value[256];
            size_t i = 0;
            while (*p && *p != ' ' && i < sizeof(value) - 1) {
                value[i++] = *p++;
            }
            value[i] = '\0';
            kprintf("Command '%s' value: %s\n", src, value);
            return strdup(value);
        }
        while (*p && *p != ' ') p++;
    }
    return NULL;
}
