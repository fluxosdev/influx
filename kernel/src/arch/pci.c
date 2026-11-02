#include "arch.h"
#include "io.h"

static struct pci_device devices[64];
static int ndev;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    return inl(0xCFC);
}
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) { return (pci_read32(bus, slot, func, off) >> ((off & 2) * 8)) & 0xFFFF; }
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    uint32_t cur = pci_read32(bus, slot, func, off);
    uint32_t shift = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(bus, slot, func, off, cur);
}

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
