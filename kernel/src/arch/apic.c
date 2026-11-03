#include "arch.h"
#include "acpi_internal.h"
#include "io.h"
#include "mm.h"

static volatile uint32_t *lapic;   /* every CPU sees its own LAPIC at this address */
static uint32_t bsp_lapic_id;
static struct { volatile uint32_t *regs; uint32_t gsi_base; int entries; } ioa[MAX_IOAPICS];
static int nioa;

#define LAPIC_ID        0x020
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_LINT0 0x350
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
    bsp_lapic_id = lapic_id();
    kprintf("lapic: id %u enabled\n", lapic_id());
}

void lapic_init_ap(void) {
    wrmsr(0x1B, rdmsr(0x1B) | (1 << 11));
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_LINT0, 0x10000);   /* the PIC's ExtINT belongs to the BSP */
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
}

uint32_t lapic_id(void) { return lapic_read(LAPIC_ID) >> 24; }
uint32_t lapic_bsp_id(void) { return bsp_lapic_id; }

/* fixed delivery, physical destination; interrupts off so nothing on this CPU interleaves its ICR writes */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    uint64_t f = irq_save();
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12)) pause();   /* previous IPI still pending */
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, vector | (1 << 14));
    irq_restore(f);
}
void lapic_eoi(void) { if (lapic) lapic_write(LAPIC_EOI, 0); }

static uint32_t timer_ticks_per_ms;

/* ---- calibration references for the LAPIC timer and the TSC ----
 * The PIT first; firmware on many newer Intel platforms clock-gates or leaves
 * out the 8254, so then the ACPI PM timer, the TSC frequency CPUID reports,
 * and as a last resort an assumed LAPIC clock. */
#define CALIB_MS         50
#define STUCK_READS      10000           /* unchanged over this many reads: the counter is not running */
#define ASSUMED_LAPIC_HZ 100000000ULL    /* the bus clock of Intel before Skylake and of AMD */
enum { REF_PIT, REF_PMTIMER, REF_CPUID, REF_ASSUMED, NREFS };
static const char *const ref_names[NREFS] = { "PIT", "ACPI PM timer", "CPUID TSC frequency", "assumed 100 MHz LAPIC clock" };

/* ACPI PM timer: 3.579545 MHz, 24 or 32 bits, at an I/O port or (X_PM_TMR_BLK) in memory */
static uint16_t pmt_port;
static volatile uint32_t *pmt_mmio;
static uint32_t pmt_mask;
static bool pmt_find(void) {
    struct acpi_fadt *f = acpi_fadt();
    if (!f || f->len < offsetof(struct acpi_fadt, pm_tmr_len) + 1) return false;
    if (f->len >= offsetof(struct acpi_fadt, flags) + 4 && (f->flags & FADT_HW_REDUCED)) return false;   /* PM_TMR fields are ignored */
    pmt_mask = f->len >= offsetof(struct acpi_fadt, flags) + 4 && (f->flags & FADT_TMR_VAL_EXT) ? 0xFFFFFFFF : 0xFFFFFF;
    const struct acpi_gas *x = f->len >= offsetof(struct acpi_fadt, x_pm_tmr) + sizeof(struct acpi_gas) ? &f->x_pm_tmr : NULL;
    if (x && x->address && x->space == SPACE_MEMORY) pmt_mmio = acpi_map_phys(x->address, 4);
    else if (x && x->address && x->space == SPACE_IO && x->address <= 0xFFFC) pmt_port = x->address;
    else if (f->pm_tmr && f->pm_tmr_len == 4 && f->pm_tmr <= 0xFFFC) pmt_port = f->pm_tmr;
    return pmt_mmio || pmt_port;
}
static uint32_t pmt_read(void) { return (pmt_mmio ? *pmt_mmio : inl(pmt_port)) & pmt_mask; }
static bool pmt_sleep_ms(uint32_t ms) {   /* false if the timer does not count (also gated on some chipsets) */
    uint64_t ticks = (uint64_t)ms * 3579545 / 1000;
    uint32_t last = pmt_read(), still = 0;
    while (ticks > 0) {
        uint32_t now = pmt_read(), delta = (now - last) & pmt_mask;   /* counts up, wraps */
        if (!delta && ++still >= STUCK_READS) return false;
        if (delta) still = 0;
        ticks -= MIN(delta, ticks);
        last = now;
        pause();
    }
    return true;
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
/* TSC Hz from CPUID 0x15 (crystal Hz x ratio) or 0x16 (base MHz), 0 if neither says */
static uint64_t cpuid_tsc_hz(void) {
    uint32_t max, a, b, c, d;
    uint64_t r = 0;
    cpuid(0, &max, &b, &c, &d);
    if (max >= 0x15) { cpuid(0x15, &a, &b, &c, &d); if (a && b && c) r = (uint64_t)c * b / a; }   /* c: crystal Hz, 0 if unreported */
    if (!r && max >= 0x16) { cpuid(0x16, &a, &b, &c, &d); r = (uint64_t)(a & 0xFFFF) * 1000000; }
    return r >= 100000000 && r <= 10000000000ULL ? r : 0;   /* hypervisors may report nonsense */
}

/* busy-wait CALIB_MS by reference `ref`; false if it is missing or not counting */
static bool ref_sleep(int ref, uint64_t cpuid_hz) {
    switch (ref) {
    case REF_PIT: return pit_sleep_ms(CALIB_MS);
    case REF_PMTIMER: return pmt_find() && pmt_sleep_ms(CALIB_MS);
    case REF_CPUID: {
        if (!cpuid_hz) return false;
        uint64_t end = rdtsc() + cpuid_hz / 1000 * CALIB_MS;
        while (rdtsc() < end) pause();
        return true;
    }
    default:   /* the LAPIC timer itself, at a guessed rate */
        while (0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR) < ASSUMED_LAPIC_HZ / 16 / 1000 * CALIB_MS) pause();
        return true;
    }
}

void lapic_timer_start(uint32_t hz) {
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    if (timer_ticks_per_ms) goto program;   /* an AP: every LAPIC timer runs off the same clock */
    /* Calibrate: run the timer (divider 16) for CALIB_MS by the first reference that works */
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    uint64_t cpuid_hz = cpuid_tsc_hz(), tsc_hz = 0;
    int ref;
    for (ref = 0; ref < NREFS; ref++) {
        lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
        uint64_t t0 = rdtsc();
        bool ok = ref_sleep(ref, cpuid_hz);
        uint64_t t1 = rdtsc();
        uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
        if ((ok && elapsed >= CALIB_MS && t1 > t0) || ref == REF_ASSUMED) {
            timer_ticks_per_ms = elapsed / CALIB_MS;
            tsc_hz = ref == REF_CPUID ? cpuid_hz : (t1 - t0) * (1000 / CALIB_MS);
            break;
        }
        kprintf("lapic: calibration reference %s unusable (missing or not counting)\n", ref_names[ref]);
    }
    lapic_write(LAPIC_LVT_TIMER, 0x10000);
    extern void cpu_set_tsc_hz(uint64_t);
    cpu_set_tsc_hz(tsc_hz);
    if (ref == REF_ASSUMED) kprintf("lapic: no reference clock, timekeeping may be off\n");
    kprintf("lapic: timer %u ticks/ms, tsc %lu MHz (reference: %s)\n", timer_ticks_per_ms, cpu_tsc_hz() / 1000000, ref_names[ref]);
program:;
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
    ioapic_write(a, 0x11 + idx * 2, bsp_lapic_id << 24);
    ioapic_write(a, 0x10 + idx * 2, lo);
}

bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool level, bool active_low) {
    uint32_t idx; int a = ioapic_for(gsi, &idx);
    if (a < 0) return false;
    ioapic_write(a, 0x11 + idx * 2, bsp_lapic_id << 24);
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
