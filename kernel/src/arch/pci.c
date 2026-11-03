#include "arch.h"
#include "acpi_internal.h"
#include "io.h"
#include "string.h"

static struct pci_device devices[64];
static int ndev;

static inline void cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC));
}
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) { cfg_addr(bus, slot, func, off); return inl(0xCFC); }
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) { cfg_addr(bus, slot, func, off); outl(0xCFC, v); }
/* narrow writes go out narrow: a read-modify-write of the dword would write
 * back (and so clear) RW1C bits next to the target, e.g. Status after Command */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) { cfg_addr(bus, slot, func, off); return inw(0xCFC + (off & 2)); }
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) { cfg_addr(bus, slot, func, off); outw(0xCFC + (off & 2), v); }
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) { cfg_addr(bus, slot, func, off); return inb(0xCFC + (off & 3)); }
void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint8_t v) { cfg_addr(bus, slot, func, off); outb(0xCFC + (off & 3), v); }

static void probe(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read32(bus, slot, func, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndev >= 64) return;
    struct pci_device *d = &devices[ndev++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = id & 0xFFFF; d->device = id >> 16;
    uint32_t cls = pci_read32(bus, slot, func, 8);
    d->class_code = cls >> 24; d->subclass = (cls >> 16) & 0xFF; d->prog_if = (cls >> 8) & 0xFF;
    d->irq_line = pci_read32(bus, slot, func, 0x3C) & 0xFF;
    uint8_t hdr = (pci_read32(bus, slot, func, 0x0C) >> 16) & 0x7F;
    d->header_type = hdr;
    if (hdr == 1) d->secondary_bus = (pci_read32(bus, slot, func, 0x18) >> 8) & 0xFF;
    int nbars = hdr == 0 ? 6 : 2;
    for (int i = 0; i < nbars; i++) {
        uint32_t b = pci_read32(bus, slot, func, 0x10 + i * 4);
        d->bar[i] = b;
        if (b & 1) { d->bar_addr[i] = b & ~3u; d->bar_is_mmio[i] = false; }
        else {
            d->bar_is_mmio[i] = true;
            uint64_t a = b & ~0xFull;
            if (((b >> 1) & 3) == 2 && i + 1 < nbars) { a |= (uint64_t)pci_read32(bus, slot, func, 0x10 + (i + 1) * 4) << 32; d->bar_addr[i] = a; i++; d->bar_addr[i] = 0; }
            else d->bar_addr[i] = a;
        }
    }
}

void pci_init(void) {
    for (int bus = 0; bus < 256; bus++)
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint8_t hdr = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF;
            int nf = (hdr & 0x80) ? 8 : 1;
            for (int f = 0; f < nf; f++) probe(bus, slot, f);
        }
    kprintf("pci: %d devices\n", ndev);
    for (int i = 0; i < ndev; i++) {
        struct pci_device *d = &devices[i];
        kprintf("pci: %02x:%02x.%d %04x:%04x class %02x.%02x.%02x %s\n", d->bus, d->slot, d->func, d->vendor, d->device,
                d->class_code, d->subclass, d->prog_if, pci_class_name(d->class_code, d->subclass));
    }
}
int pci_device_count(void) { return ndev; }
struct pci_device *pci_device_at(int i) { return i >= 0 && i < ndev ? &devices[i] : NULL; }

void pci_enable_bus_master(struct pci_device *d) {
    uint16_t cmd = pci_read16(d->bus, d->slot, d->func, 4);
    cmd |= (1 << 2) | (1 << 1) | (1 << 0);   /* bus master, memory, io */
    cmd &= ~(1 << 10);                        /* enable INTx */
    pci_write16(d->bus, d->slot, d->func, 4, cmd);
}

uint8_t pci_find_cap(struct pci_device *d, uint8_t id) {
    if (!(pci_read16(d->bus, d->slot, d->func, 0x06) & (1 << 4))) return 0;
    uint8_t p = pci_read32(d->bus, d->slot, d->func, 0x34) & 0xFC;
    for (int guard = 0; p && guard < 48; guard++) {
        uint32_t v = pci_read32(d->bus, d->slot, d->func, p);
        if ((v & 0xFF) == id) return p;
        p = (v >> 8) & 0xFC;
    }
    return 0;
}

/* INTx lines can be shared (with each other and with the ACPI SCI), so every
 * installed handler goes through a dispatcher that calls all handlers on the
 * vector and then sends the EOI. */
struct pci_irq { uint32_t gsi; bool msi; int vector, n; irq_handler_t h[8]; void *ctx[8]; };
static struct pci_irq pirqs[16];
static int npirqs;

static void pci_irq_dispatch(struct regs *r, void *p) {
    struct pci_irq *pi = p;
    for (int i = 0; i < pi->n; i++) pi->h[i](r, pi->ctx[i]);
    lapic_eoi();
}

static struct pci_irq *pirq_new(void) {
    if (npirqs >= (int)ARRAY_LEN(pirqs)) return NULL;
    struct pci_irq *pi = &pirqs[npirqs];
    if (!(pi->vector = irq_alloc_vector())) return NULL;
    npirqs++;
    irq_register(pi->vector, pci_irq_dispatch, pi);
    return pi;
}

/* add a handler to a GSI's dispatcher, routing the GSI on first use */
static const char *gsi_attach(uint32_t gsi, bool level, bool active_low, irq_handler_t h, void *ctx) {
    struct pci_irq *pi = NULL;
    for (int i = 0; i < npirqs; i++) if (!pirqs[i].msi && pirqs[i].gsi == gsi) pi = &pirqs[i];
    if (pi && pi->n >= (int)ARRAY_LEN(pi->h)) return "too many handlers on the GSI";
    bool fresh = !pi;
    if (fresh) { if (!(pi = pirq_new())) return "no free vector"; pi->gsi = gsi; }
    uint64_t fl = irq_save();
    pi->h[pi->n] = h; pi->ctx[pi->n] = ctx; pi->n++;
    irq_restore(fl);
    if (fresh && !ioapic_route_gsi(gsi, pi->vector, level, active_low)) { pi->n = 0; pi->gsi = ~0u; return "GSI outside the I/O APICs"; }
    return NULL;
}
bool pci_irq_share_gsi(uint32_t gsi, bool level, bool active_low, irq_handler_t h, void *ctx) { return !gsi_attach(gsi, level, active_low, h, ctx); }

bool pci_irq_install(struct pci_device *d, irq_handler_t h, void *ctx, bool allow_msi, char *how, size_t how_len) {
    uint8_t msi = allow_msi ? pci_find_cap(d, 0x05) : 0;
    if (msi) {
        struct pci_irq *pi = pirq_new();
        if (!pi) { snprintf(how, how_len, "no free vector"); return false; }
        pi->msi = true; pi->h[0] = h; pi->ctx[0] = ctx; pi->n = 1;
        uint16_t ctl = pci_read16(d->bus, d->slot, d->func, msi + 2);
        pci_write32(d->bus, d->slot, d->func, msi + 4, 0xFEE00000u | (lapic_id() << 12));
        if (ctl & (1 << 7)) { pci_write32(d->bus, d->slot, d->func, msi + 8, 0); pci_write16(d->bus, d->slot, d->func, msi + 12, pi->vector); }
        else pci_write16(d->bus, d->slot, d->func, msi + 8, pi->vector);
        pci_write16(d->bus, d->slot, d->func, msi + 2, (ctl & ~(7 << 4)) | 1);   /* one vector, enabled */
        pci_write16(d->bus, d->slot, d->func, 4, pci_read16(d->bus, d->slot, d->func, 4) | (1 << 10));   /* INTx off */
        snprintf(how, how_len, "MSI, vector 0x%x", pi->vector);
        return true;
    }
    uint8_t pin = (pci_read32(d->bus, d->slot, d->func, 0x3C) >> 8) & 0xFF;
    struct acpi_irq_route rt;
    if (!pin) { snprintf(how, how_len, "no interrupt pin"); return false; }
    if (!acpi_pci_route(d->bus, d->slot, d->func, pin, &rt)) { snprintf(how, how_len, "INT%c not routed by ACPI", 'A' + pin - 1); return false; }
    const char *err = gsi_attach(rt.gsi, rt.level, rt.active_low, h, ctx);
    if (err) { snprintf(how, how_len, "GSI %u: %s", rt.gsi, err); return false; }
    snprintf(how, how_len, "INT%c, GSI %u (%s, active %s)", 'A' + pin - 1, rt.gsi, rt.level ? "level" : "edge", rt.active_low ? "low" : "high");
    return true;
}

const char *pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x00: return "Unclassified";
    case 0x01: switch (sub) { case 0x01: return "IDE controller"; case 0x06: return "SATA controller"; case 0x08: return "NVMe controller"; default: return "Storage controller"; }
    case 0x02: return "Network controller";
    case 0x03: return "Display controller";
    case 0x04: return "Multimedia controller";
    case 0x06: switch (sub) { case 0x00: return "Host bridge"; case 0x01: return "ISA bridge"; case 0x04: return "PCI bridge"; default: return "Bridge"; }
    case 0x0C: switch (sub) { case 0x03: return "USB controller"; case 0x05: return "SMBus"; default: return "Serial bus controller"; }
    default: return "Device";
    }
}
