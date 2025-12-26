#include <lib/sys/io.h>
#include <kernel/kprintf.h>
#include <drivers/idt.h>
#include <drivers/pit.h>
uint64_t pit_counter = 0;

void pit_handler(void) {
	pit_counter++;
   if (pit_counter % 1000 == 0) {
       kprintf("PIT: %llu seconds elapsed\n", pit_counter / 1000);
   }
	interrupts_eoi();
}

void pit_init() {
	uint16_t div = PIT_FREQ / 1000;
	uint8_t mode = 0b110110;
	outb(PIT_MODE_PORT, mode);
	outb(PIT_CHANNEL0_PORT, (uint8_t)div);
	outb(PIT_CHANNEL0_PORT, (uint8_t)(div >> 8));
	interrupts_set_handler(32, pit_handler);
}

void pit_wait(uint64_t ms) {
	uint64_t expected = pit_counter + ms;
	while (pit_counter < expected) {
        __asm__ volatile("hlt");
    }
}