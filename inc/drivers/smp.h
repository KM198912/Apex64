#pragma once
#include <stdint.h>

/* AP entry point called from AP trampoline (long mode) */
void ap_entry(void);

/* SMP startup and management */
extern volatile uint32_t smp_started_count;
