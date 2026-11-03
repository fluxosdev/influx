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

/* the root table, if its signature and checksum hold */
static struct sdt *valid_root(uint64_t phys, const char *sig, bool strict) {
    struct sdt *t = map_table(phys);
    if (!t || memcmp(t->sig, sig, 4)) { if (strict) kprintf("acpi: %s at %lx unusable\n", sig, phys); return NULL; }
    uint8_t sum = 0;
    for (uint32_t i = 0; i < t->len; i++) sum += ((uint8_t *)t)[i];
    if (sum && strict) { kprintf("acpi: %s at %lx: bad checksum\n", sig, phys); return NULL; }
    return t;
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

/* MCFG: the PCI Express enhanced configuration (ECAM) windows, one per
 * segment group / bus range; 1 MiB per bus, mapped when first used */
#define MAX_MCFG 8
static struct { uint64_t base; uint16_t seg; uint8_t start, end; uint8_t *bus[256]; } mcfg[MAX_MCFG];
static int nmcfg;

static void parse_mcfg(void) {
    struct sdt *t = acpi_find_table("MCFG");
    if (!t) return;
    struct { uint64_t base; uint16_t seg; uint8_t start, end; uint32_t res; } PACKED *e = (void *)((uint8_t *)t + sizeof(struct sdt) + 8);
    for (int i = 0; (uint8_t *)(e + i + 1) <= (uint8_t *)t + t->len && nmcfg < MAX_MCFG; i++) {
        if (!e[i].base || e[i].start > e[i].end) { kprintf("acpi: MCFG entry %d unusable\n", i); continue; }
        mcfg[nmcfg].base = e[i].base; mcfg[nmcfg].seg = e[i].seg; mcfg[nmcfg].start = e[i].start; mcfg[nmcfg].end = e[i].end;
        kprintf("acpi: ECAM segment %u buses %u-%u at %lx\n", e[i].seg, e[i].start, e[i].end, e[i].base);
        nmcfg++;
    }
}

/* the bus's ECAM window (function at dev << 15 | fn << 12), NULL if the MCFG does not cover it */
void *acpi_ecam(uint16_t seg, uint8_t bus) {
    for (int i = 0; i < nmcfg; i++) {
        if (mcfg[i].seg != seg || bus < mcfg[i].start || bus > mcfg[i].end) continue;
        uint8_t *w = __atomic_load_n(&mcfg[i].bus[bus], __ATOMIC_ACQUIRE);
        if (!w) {   /* a racing second mapping of the same range is harmless */
            w = vmm_map_mmio(mcfg[i].base + ((uint64_t)bus << 20), 1u << 20);
            __atomic_store_n(&mcfg[i].bus[bus], w, __ATOMIC_RELEASE);
        }
        return w;
    }
    return NULL;
}

static void parse_madt(void) {
    for (int i = 0; i < 16; i++) { madt.iso_src[i] = i; madt.iso_flags[i] = 0; }
    madt.lapic_addr = 0xFEE00000; madt.ioapic_addr = 0xFEC00000;
    struct sdt *t = acpi_find_table("APIC");
    if (!t || t->len < sizeof(struct sdt) + 8) { kprintf("acpi: no MADT, assuming defaults\n"); return; }
    uint8_t *p = (uint8_t *)t + sizeof(struct sdt);
    madt.lapic_addr = *(uint32_t *)p; p += 8;
    uint8_t *end = (uint8_t *)t + t->len;
    /* firmware may list a CPU both as a local APIC (type 0) and as a local x2APIC (type 9): count each id once */
    uint64_t seen[4] = { 0 }; bool skipped_x2 = false;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) break;
        switch (type) {
        case 0:
            if (len >= 8 && (p[4] & 1) && !(seen[p[3] / 64] & 1ULL << (p[3] % 64))) { seen[p[3] / 64] |= 1ULL << (p[3] % 64); madt.cpu_count++; }
            break;
        case 9: {   /* local x2APIC; type 10 (x2APIC NMI) needs nothing: LINT NMIs are not programmed from the MADT */
            if (len < 16 || !(*(uint32_t *)(p + 8) & 1)) break;
            uint32_t id = *(uint32_t *)(p + 4);
            if (id > 255) { skipped_x2 = true; break; }   /* the LAPIC runs in xAPIC mode: 8-bit ids only */
            if (!(seen[id / 64] & 1ULL << (id % 64))) { seen[id / 64] |= 1ULL << (id % 64); madt.cpu_count++; }
            break;
        }
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
    if (skipped_x2) kprintf("acpi: CPUs with x2APIC ids above 255 are not supported, skipped\n");
}

void acpi_init(uint64_t rsdp_phys) {
    if (!rsdp_phys) { kprintf("acpi: no RSDP\n"); parse_madt(); return; }
    struct rsdp *r = acpi_map_phys(rsdp_phys, sizeof *r);
    /* the XSDT, else the RSDT; a bad checksum on both: whichever has the right signature */
    uint64_t x = r->rev >= 2 ? r->xsdt : 0;
    for (int strict = 1; !root && strict >= 0; strict--) {
        if (x && (root = valid_root(x, "XSDT", strict))) use_xsdt = true;
        else if (r->rsdt && (root = valid_root(r->rsdt, "RSDT", strict))) { use_xsdt = false; if (x) kprintf("acpi: using the RSDT instead of the XSDT\n"); }
    }
    if (!root) { kprintf("acpi: no usable RSDT/XSDT\n"); parse_madt(); return; }
    kprintf("acpi: %s at %p, OEM '%.6s'\n", use_xsdt ? "XSDT" : "RSDT", root, r->oem);
    parse_madt();
    parse_mcfg();
    kprintf("acpi: lapic=%lx ioapic=%lx (%d I/O APICs) cpus=%d\n", madt.lapic_addr, madt.ioapic_addr, nioapics, madt.cpu_count);
}
