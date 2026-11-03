#include "arch.h"
#include "sched.h"

volatile uint64_t system_ticks;
static uint64_t boot_unix;
int64_t tz_offset_minutes = 0;

/* every CPU's LAPIC timer lands here; the BSP's keeps the time */
static void timer_irq(struct regs *r, void *ctx) {
    if (this_cpu()->id == 0) system_ticks++;
    sched_tick(r);   /* sends the EOI */
}

void timer_init(void) {
    struct rtc_time t;
    rtc_read(&t);
    boot_unix = rtc_to_unix(&t);
    kprintf("rtc: %04d-%02d-%02d %02d:%02d:%02d UTC\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    irq_register(VEC_LAPIC_TIMER, timer_irq, NULL);
    lapic_timer_start(1000);
}

uint64_t time_ms(void) { return system_ticks; }
uint64_t time_unix(void) { return boot_unix + system_ticks / 1000; }
