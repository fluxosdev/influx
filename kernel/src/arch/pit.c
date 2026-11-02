#include "arch.h"
#include "io.h"

/* The PIT is only used for calibration and early busy-waits. */
void pit_init(void) {
    outb(0x43, 0x34);           /* channel 0, lo/hi, rate generator */
    outb(0x40, 0xFF); outb(0x40, 0xFF);  /* ~18.2 Hz, we just read the counter */
}

static uint16_t pit_read(void) {
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40), hi = inb(0x40);
    return ((uint16_t)hi << 8) | lo;
}

void pit_sleep_ms(uint32_t ms) {
    /* 1193182 Hz input; count down from 65535 repeatedly */
    uint64_t ticks = (uint64_t)ms * 1193182 / 1000;
    uint16_t last = pit_read();
    while (ticks > 0) {
        uint16_t now = pit_read();
        uint16_t delta = (uint16_t)(last - now);   /* counter decrements, wraps */
        if (delta > ticks) delta = (uint16_t)ticks;
        ticks -= delta;
        last = now;
        pause();
    }
}
