#include "arch.h"
#include "string.h"

struct rsdp { char sig[8]; uint8_t csum; char oem[6]; uint8_t rev; uint32_t rsdt; uint32_t len; uint64_t xsdt; uint8_t xcsum; uint8_t res[3]; } PACKED;
struct sdt { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6]; char oemtbl[8]; uint32_t oemrev, creator, creatorrev; } PACKED;

static struct sdt *root; static bool use_xsdt;
struct madt_info madt;

void *acpi_find_table(const char *sig) {
    if (!root) return NULL;
    int n = (root->len - sizeof(struct sdt)) / (use_xsdt ? 8 : 4);
    for (int i = 0; i < n; i++) {
        uint64_t addr = use_xsdt ? ((uint64_t *)(root + 1))[i] : ((uint32_t *)(root + 1))[i];
        struct sdt *t = P2V(addr);
        if (!memcmp(t->sig, sig, 4)) return t;
    }
    return NULL;
}

static void parse_madt(void) {
    for (int i = 0; i < 16; i++) { madt.iso_src[i] = i; madt.iso_flags[i] = 0; }
    madt.lapic_addr = 0xFEE00000; madt.ioapic_addr = 0xFEC00000;
    struct sdt *t = acpi_find_table("APIC");
    if (!t) { kprintf("acpi: no MADT, assuming defaults\n"); return; }
    uint8_t *p = (uint8_t *)t + sizeof(struct sdt);
    madt.lapic_addr = *(uint32_t *)p; p += 8;
    uint8_t *end = (uint8_t *)t + t->len;
    while (p < end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2) break;
        switch (type) {
        case 0: if (p[4] & 1) madt.cpu_count++; break;
        case 1: if (madt.ioapic_gsi_base == 0 && madt.ioapic_addr == 0xFEC00000) { madt.ioapic_addr = *(uint32_t *)(p + 4); madt.ioapic_gsi_base = *(uint32_t *)(p + 8); } break;
        case 2: { uint8_t src = p[3]; uint32_t gsi = *(uint32_t *)(p + 4); uint16_t fl = *(uint16_t *)(p + 8);
                  if (src < 16) { madt.iso_src[src] = gsi; madt.iso_flags[src] = fl; } break; }
        case 5: madt.lapic_addr = *(uint64_t *)(p + 4); break;
        }
        p += len;
    }
}

void acpi_init(uint64_t rsdp_phys) {
    if (!rsdp_phys) { kprintf("acpi: no RSDP\n"); parse_madt(); return; }
    struct rsdp *r = P2V(rsdp_phys);
    if (r->rev >= 2 && r->xsdt) { root = P2V(r->xsdt); use_xsdt = true; }
    else { root = P2V(r->rsdt); use_xsdt = false; }
    kprintf("acpi: %s at %p, OEM '%.6s'\n", use_xsdt ? "XSDT" : "RSDT", root, r->oem);
    parse_madt();
    kprintf("acpi: lapic=%lx ioapic=%lx cpus=%d\n", madt.lapic_addr, madt.ioapic_addr, madt.cpu_count);
}
