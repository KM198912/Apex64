#include <common/boot.h>
#include <kernel/kprintf.h>
#include <lib/string.h>
#include <stddef.h>
char* cmd_get(const char* src)
{
    const char* cmd_line = TitanBootInfo.cmdline;
    if (!cmd_line) {
        kprintf("No command line available\n");
        return NULL;
    }
    //find cmd value in cmdline
    const char* p = cmd_line;
    size_t cmd_len = strlen(src);
    while (*p) {
        //skip leading spaces
        while (*p == ' ') p++;
        //check if this is the command we want
        if (strncmp(p, src, cmd_len) == 0 && p[cmd_len] == '=') {
            p += cmd_len + 1; //move past "cmd="
            //copy value until space or end
            char value[256];
            size_t i = 0;
            while (*p && *p != ' ' && i < sizeof(value) - 1) {
                value[i++] = *p++;
            }
            value[i] = '\0';
            kprintf("Command '%s' value: %s\n", src, value);
            return strdup(value);
        }
        //move to next argument
        while (*p && *p != ' ') p++;
    }
    kprintf("Command '%s' not found in command line\n", src);
    return NULL;
}