/*
 * Timekeeping.
 *
 * system_ticks counts the BSP's 1 kHz LAPIC timer interrupts (time_ms). A
 * finer monotonic clock, time_ns, reads the TSC when it ticks at a constant
 * rate (invariant TSC, or a hypervisor's), scaled with a fixed-point
 * multiplier; otherwise it falls back to the tick count. The wall clock is
 * that monotonic clock plus an offset: read from the RTC at boot and moved by
 * time_set_realtime_ns (SNTP, the date command), which also writes the RTC.
 */
#include "arch.h"
#include "io.h"
#include "sched.h"

volatile uint64_t system_ticks;
int64_t tz_offset_minutes = 0;

static uint64_t tsc0;                /* TSC at the time ns_at_tsc0 refers to */
static uint64_t tsc_mult;            /* ns per TSC tick, 32.32 fixed point */
static bool use_tsc;
static volatile uint64_t last_ns;    /* keeps time_ns monotonic across CPUs */
static volatile int64_t realtime_base_ns;   /* wall clock = realtime_base_ns + time_ns() */

/* every CPU's LAPIC timer lands here; the BSP's keeps the time */
static void timer_irq(struct regs *r, void *ctx) {
    if (this_cpu()->id == 0) system_ticks++;
    sched_tick(r);   /* sends the EOI */
}

static void cpuid4(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

void timer_init(void) {
    struct rtc_time t;
    rtc_read(&t);
    kprintf("rtc: %04d-%02d-%02d %02d:%02d:%02d UTC\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    irq_register(VEC_LAPIC_TIMER, timer_irq, NULL);
    lapic_timer_start(1000);   /* calibrates the TSC too */
    uint32_t a, b, c, d, maxext;
    cpuid4(1, &a, &b, &c, &d);
    bool hypervisor = c & (1u << 31);
    cpuid4(0x80000000, &maxext, &b, &c, &d);
    bool invariant = false;
    if (maxext >= 0x80000007) { cpuid4(0x80000007, &a, &b, &c, &d); invariant = d & (1 << 8); }
    uint64_t hz = cpu_tsc_hz();
    if (hz && (invariant || hypervisor)) {
        tsc_mult = (uint64_t)(((unsigned __int128)1000000000ull << 32) / hz);
        tsc0 = rdtsc();
        use_tsc = true;
    }
    realtime_base_ns = (int64_t)rtc_to_unix(&t) * 1000000000ll - (int64_t)time_ns();
    kprintf("clock: %s\n", use_tsc ? (invariant ? "invariant TSC" : "TSC (hypervisor)") : "timer ticks (1 ms)");
}

uint64_t time_ms(void) { return system_ticks; }

uint64_t time_ns(void) {
    uint64_t ns;
    if (use_tsc) {
        uint64_t delta = rdtsc() - tsc0;
        if ((int64_t)delta < 0) delta = 0;   /* a CPU whose TSC trails the BSP's by a little */
        ns = (uint64_t)(((unsigned __int128)delta * tsc_mult) >> 32);
    } else ns = system_ticks * 1000000ull;
    /* never go backwards, whichever CPU asks */
    uint64_t prev = __atomic_load_n(&last_ns, __ATOMIC_RELAXED);
    while (ns > prev && !__atomic_compare_exchange_n(&last_ns, &prev, ns, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return ns > prev ? ns : prev;
}

int64_t time_realtime_ns(void) { return realtime_base_ns + (int64_t)time_ns(); }
uint64_t time_unix(void) { int64_t ns = time_realtime_ns(); return ns < 0 ? 0 : (uint64_t)ns / 1000000000ull; }

void time_set_realtime_ns(int64_t ns) {
    realtime_base_ns = ns - (int64_t)time_ns();
    struct rtc_time t;   /* the RTC keeps whole seconds; up to one second off is fine for the next boot */
    unix_to_datetime((uint64_t)(ns / 1000000000ll), &t);
    rtc_write(&t);
}
