#include "arch.h"
#include "io.h"
#include "mm.h"

static volatile uint32_t *lapic;
static volatile uint32_t *ioapic;
static int ioapic_entries;

#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

static inline uint32_t lapic_read(uint32_t r) { return lapic[r / 4]; }
static inline void lapic_write(uint32_t r, uint32_t v) { lapic[r / 4] = v; }

void lapic_init(void) {
    /* Mask the legacy PICs */
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
    /* remap anyway so spurious legacy IRQs don't land on exception vectors */
    outb(0x20, 0x11); outb(0xA0, 0x11); outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 4); outb(0xA1, 2); outb(0x21, 1); outb(0xA1, 1); outb(0x21, 0xFF); outb(0xA1, 0xFF);

    uint64_t base = rdmsr(0x1B);
    wrmsr(0x1B, base | (1 << 11));   /* ensure enabled */
    lapic = vmm_map_mmio(madt.lapic_addr, 0x1000);
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);   /* masked for now */
    kprintf("lapic: id %u enabled\n", lapic_id());
}
uint32_t lapic_id(void) { return lapic_read(LAPIC_ID) >> 24; }
void lapic_eoi(void) { if (lapic) lapic_write(LAPIC_EOI, 0); }

static uint32_t timer_ticks_per_ms;

void lapic_timer_start(uint32_t hz) {
    /* Calibrate against the PIT: run the timer for 50 ms with divider 16 */
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    uint64_t t0 = rdtsc();
    pit_sleep_ms(50);
    uint64_t t1 = rdtsc();
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    timer_ticks_per_ms = elapsed / 50;
    extern void cpu_set_tsc_hz(uint64_t);
    cpu_set_tsc_hz((t1 - t0) * 20);
    kprintf("lapic: timer %u ticks/ms, tsc %lu MHz\n", timer_ticks_per_ms, cpu_tsc_hz() / 1000000);
    uint32_t count = timer_ticks_per_ms * 1000 / hz;
    if (count == 0) count = 1;
    lapic_write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | (1 << 17));   /* periodic */
    lapic_write(LAPIC_TIMER_INIT, count);
}

/* ---- I/O APIC ---- */
static inline uint32_t ioapic_read(uint32_t reg) { ioapic[0] = reg; return ioapic[4]; }
static inline void ioapic_write(uint32_t reg, uint32_t v) { ioapic[0] = reg; ioapic[4] = v; }

void ioapic_init(void) {
    ioapic = vmm_map_mmio(madt.ioapic_addr, 0x1000);
    uint32_t ver = ioapic_read(1);
    int entries = ((ver >> 16) & 0xFF) + 1;
    ioapic_entries = entries;
    for (int i = 0; i < entries; i++) { ioapic_write(0x10 + i * 2, 0x10000); ioapic_write(0x11 + i * 2, 0); }
    kprintf("ioapic: %d redirection entries\n", entries);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector) {
    uint32_t gsi = irq < 16 ? madt.iso_src[irq] : irq;
    uint16_t flags = irq < 16 ? madt.iso_flags[irq] : 0;
    uint32_t idx = gsi - madt.ioapic_gsi_base;
    uint32_t lo = vector;
    if ((flags & 3) == 3) lo |= (1 << 13);     /* active low */
    if (((flags >> 2) & 3) == 3) lo |= (1 << 15); /* level triggered */
    ioapic_write(0x11 + idx * 2, lapic_id() << 24);
    ioapic_write(0x10 + idx * 2, lo);
}

bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool level, bool active_low) {
    if (gsi < madt.ioapic_gsi_base || gsi - madt.ioapic_gsi_base >= (uint32_t)ioapic_entries) return false;
    uint32_t idx = gsi - madt.ioapic_gsi_base;
    ioapic_write(0x11 + idx * 2, lapic_id() << 24);
    ioapic_write(0x10 + idx * 2, vector | (active_low ? 1 << 13 : 0) | (level ? 1 << 15 : 0));
    return true;
}

void ioapic_mask_irq(uint8_t irq, bool mask) {
    uint32_t gsi = irq < 16 ? madt.iso_src[irq] : irq;
    uint32_t idx = gsi - madt.ioapic_gsi_base;
    uint32_t lo = ioapic_read(0x10 + idx * 2);
    if (mask) lo |= 0x10000; else lo &= ~0x10000u;
    ioapic_write(0x10 + idx * 2, lo);
}
