#include "arch.h"
#include "acpi_internal.h"
#include "io.h"
#include "mm.h"

static volatile uint32_t *lapic;
static struct { volatile uint32_t *regs; uint32_t gsi_base; int entries; } ioa[MAX_IOAPICS];
static int nioa;

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
static inline uint32_t ioapic_read(int a, uint32_t reg) { ioa[a].regs[0] = reg; return ioa[a].regs[4]; }
static inline void ioapic_write(int a, uint32_t reg, uint32_t v) { ioa[a].regs[0] = reg; ioa[a].regs[4] = v; }

/* the I/O APIC serving a GSI, and the pin on it */
static int ioapic_for(uint32_t gsi, uint32_t *idx) {
    for (int a = 0; a < nioa; a++)
        if (gsi >= ioa[a].gsi_base && gsi - ioa[a].gsi_base < (uint32_t)ioa[a].entries) { *idx = gsi - ioa[a].gsi_base; return a; }
    return -1;
}

void ioapic_init(void) {
    int n = nioapics ? nioapics : 1;
    for (int a = 0; a < n; a++) {
        ioa[a].regs = vmm_map_mmio(nioapics ? ioapics[a].addr : madt.ioapic_addr, 0x1000);
        ioa[a].gsi_base = nioapics ? ioapics[a].gsi_base : madt.ioapic_gsi_base;
        nioa = a + 1;
        int entries = ((ioapic_read(a, 1) >> 16) & 0xFF) + 1;
        ioa[a].entries = entries;
        for (int i = 0; i < entries; i++) { ioapic_write(a, 0x10 + i * 2, 0x10000); ioapic_write(a, 0x11 + i * 2, 0); }
        kprintf("ioapic: GSI %u-%u, %d redirection entries\n", ioa[a].gsi_base, ioa[a].gsi_base + entries - 1, entries);
    }
}

void ioapic_route_irq(uint8_t irq, uint8_t vector) {
    uint32_t gsi = irq < 16 ? madt.iso_src[irq] : irq;
    uint16_t flags = irq < 16 ? madt.iso_flags[irq] : 0;
    uint32_t idx; int a = ioapic_for(gsi, &idx);
    if (a < 0) { kprintf("ioapic: IRQ %u (GSI %u) has no I/O APIC\n", irq, gsi); return; }
    uint32_t lo = vector;
    if ((flags & 3) == 3) lo |= (1 << 13);     /* active low */
    if (((flags >> 2) & 3) == 3) lo |= (1 << 15); /* level triggered */
    ioapic_write(a, 0x11 + idx * 2, lapic_id() << 24);
    ioapic_write(a, 0x10 + idx * 2, lo);
}

bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool level, bool active_low) {
    uint32_t idx; int a = ioapic_for(gsi, &idx);
    if (a < 0) return false;
    ioapic_write(a, 0x11 + idx * 2, lapic_id() << 24);
    ioapic_write(a, 0x10 + idx * 2, vector | (active_low ? 1 << 13 : 0) | (level ? 1 << 15 : 0));
    return true;
}

void ioapic_mask_irq(uint8_t irq, bool mask) {
    uint32_t gsi = irq < 16 ? madt.iso_src[irq] : irq;
    uint32_t idx; int a = ioapic_for(gsi, &idx);
    if (a < 0) return;
    uint32_t lo = ioapic_read(a, 0x10 + idx * 2);
    if (mask) lo |= 0x10000; else lo &= ~0x10000u;
    ioapic_write(a, 0x10 + idx * 2, lo);
}
