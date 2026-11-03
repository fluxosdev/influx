#include "arch.h"
#include "acpi_internal.h"
#include "mm.h"
#include "string.h"

struct rsdp { char sig[8]; uint8_t csum; char oem[6]; uint8_t rev; uint32_t rsdt; uint32_t len; uint64_t xsdt; uint8_t xcsum; uint8_t res[3]; } PACKED;
struct sdt { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6]; char oemtbl[8]; uint32_t oemrev, creator, creatorrev; } PACKED;

static struct sdt *root; static bool use_xsdt;
struct madt_info madt;
struct ioapic_info ioapics[MAX_IOAPICS];
int nioapics;

/* Limine's HHDM covers RAM but not every firmware region (ACPI NVS,
 * reserved ranges), so map what is missing before touching it. */
void *acpi_map_phys(uint64_t phys, size_t len) {
    for (uint64_t p = PAGE_ALIGN_DOWN(phys); p < phys + len; p += PAGE_SIZE)
        if (!vmm_get_phys(vmm_kernel_pml4(), hhdm_offset + p)) vmm_map_mmio(p, PAGE_SIZE);
    return P2V(phys);
}
static struct sdt *map_table(uint64_t phys) {
    struct sdt *t = acpi_map_phys(phys, sizeof(struct sdt));
    if (t->len < sizeof(struct sdt) || t->len > ACPI_MAX_TABLE) { kprintf("acpi: table at %lx has bad length %u\n", phys, t->len); return NULL; }
    return acpi_map_phys(phys, t->len);
}

void *acpi_table_at(int i, char sig[4]) {
    if (!root) return NULL;
    int n = (root->len - sizeof(struct sdt)) / (use_xsdt ? 8 : 4);
    if (i < 0 || i >= n) return NULL;
    uint64_t addr = use_xsdt ? ((uint64_t *)(root + 1))[i] : ((uint32_t *)(root + 1))[i];
    static struct sdt empty;   /* a zero entry: keep enumerating past it */
    struct sdt *t = addr ? map_table(addr) : NULL;
    if (!t) { memset(sig, 0, 4); return &empty; }
    memcpy(sig, t->sig, 4);
    return t;
}

void *acpi_find_table(const char *sig) {
    char s[5];
    for (int i = 0; acpi_table_at(i, s); i++) if (!memcmp(s, sig, 4)) return acpi_table_at(i, s);
    return NULL;
}

struct acpi_fadt *acpi_fadt(void) { return acpi_find_table("FACP"); }

static void parse_madt(void) {
    for (int i = 0; i < 16; i++) { madt.iso_src[i] = i; madt.iso_flags[i] = 0; }
    madt.lapic_addr = 0xFEE00000; madt.ioapic_addr = 0xFEC00000;
    struct sdt *t = acpi_find_table("APIC");
    if (!t || t->len < sizeof(struct sdt) + 8) { kprintf("acpi: no MADT, assuming defaults\n"); return; }
    uint8_t *p = (uint8_t *)t + sizeof(struct sdt);
    madt.lapic_addr = *(uint32_t *)p; p += 8;
    uint8_t *end = (uint8_t *)t + t->len;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) break;
        switch (type) {
        case 0: if (len >= 8 && (p[4] & 1)) madt.cpu_count++; break;
        case 1:
            if (len < 12 || nioapics >= MAX_IOAPICS) break;
            ioapics[nioapics].addr = *(uint32_t *)(p + 4); ioapics[nioapics].gsi_base = *(uint32_t *)(p + 8);
            if (!nioapics++) { madt.ioapic_addr = *(uint32_t *)(p + 4); madt.ioapic_gsi_base = *(uint32_t *)(p + 8); }
            break;
        case 2: { if (len < 10) break;
                  uint8_t src = p[3]; uint32_t gsi = *(uint32_t *)(p + 4); uint16_t fl = *(uint16_t *)(p + 8);
                  if (src < 16) { madt.iso_src[src] = gsi; madt.iso_flags[src] = fl; } break; }
        case 5: if (len >= 12) madt.lapic_addr = *(uint64_t *)(p + 4); break;
        }
        p += len;
    }
}

void acpi_init(uint64_t rsdp_phys) {
    if (!rsdp_phys) { kprintf("acpi: no RSDP\n"); parse_madt(); return; }
    struct rsdp *r = acpi_map_phys(rsdp_phys, sizeof *r);
    if (r->rev >= 2 && r->xsdt) { root = map_table(r->xsdt); use_xsdt = true; }
    else { root = map_table(r->rsdt); use_xsdt = false; }
    if (!root) { kprintf("acpi: unusable %s\n", use_xsdt ? "XSDT" : "RSDT"); parse_madt(); return; }
    kprintf("acpi: %s at %p, OEM '%.6s'\n", use_xsdt ? "XSDT" : "RSDT", root, r->oem);
    parse_madt();
    kprintf("acpi: lapic=%lx ioapic=%lx (%d I/O APICs) cpus=%d\n", madt.lapic_addr, madt.ioapic_addr, nioapics, madt.cpu_count);
}
