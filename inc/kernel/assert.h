#pragma once
#include <stdint.h>
#include <stddef.h>
#include <kernel/kprintf.h>

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        kprintf(LOG_ERROR "Assertion failed: (%s) %s, in %s:%d\n", \
                #cond, msg, __FILE__, __LINE__); \
        __asm__ volatile("cli"); \
        for (;;) __asm__ volatile("hlt"); \
    } \
} while (0)
