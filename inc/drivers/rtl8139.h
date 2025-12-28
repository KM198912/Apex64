#ifndef DRIVERS_RTL8139_H
#define DRIVERS_RTL8139_H

#include <stdint.h>

/* Register the RTL8139 device-specific driver (call from init) */
void rtl8139_register(void);

#endif /* DRIVERS_RTL8139_H */
