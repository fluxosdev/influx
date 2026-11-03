/*
 * Operation regions and field units: SystemMemory, SystemIO, PCI_Config,
 * EmbeddedControl and CMOS address spaces; normal, index and bank fields;
 * buffer fields; Generic Address Structure access for the FADT registers.
 * Also the ACPI embedded controller (PNP0C09), which laptops route battery,
 * lid and hotkey events through.
 */
#include "acpi_internal.h"
#include "io.h"
#include "mm.h"
#include "string.h"

/* ---- raw accesses ---- */
static uint64_t mem_read(uint64_t phys, int width) {
    volatile void *p = acpi_map_phys(phys, width / 8);
    switch (width) {
    case 8: return *(volatile uint8_t *)p;
    case 16: return *(volatile uint16_t *)p;
    case 32: return *(volatile uint32_t *)p;
    default: return *(volatile uint64_t *)p;
    }
}
static void mem_write(uint64_t phys, int width, uint64_t v) {
    volatile void *p = acpi_map_phys(phys, width / 8);
    switch (width) {
    case 8: *(volatile uint8_t *)p = v; break;
    case 16: *(volatile uint16_t *)p = v; break;
    case 32: *(volatile uint32_t *)p = v; break;
    default: *(volatile uint64_t *)p = v; break;
    }
}
static uint64_t io_read(uint16_t port, int width) {
    switch (width) {
    case 8: return inb(port);
    case 16: return inw(port);
    case 32: return inl(port);
    default: return inl(port) | (uint64_t)inl(port + 4) << 32;
    }
}
static void io_write(uint16_t port, int width, uint64_t v) {
    switch (width) {
    case 8: outb(port, v); break;
    case 16: outw(port, v); break;
    case 32: outl(port, v); break;
    default: outl(port, v); outl(port + 4, v >> 32); break;
    }
}
/* PCI configuration space: ECAM (MCFG) for every offset when the firmware
 * provides it, else the legacy 0xCF8 mechanism for the first 256 bytes.
 * Natural-width chunks, no read-modify-write (RW1C neighbours would be cleared). */
static uint32_t cfg_rd(volatile uint8_t *e, struct aml_obj *r, uint16_t off, int w) {
    if (e) return w == 32 ? *(volatile uint32_t *)(e + off) : w == 16 ? *(volatile uint16_t *)(e + off) : e[off];
    uint32_t v = pci_read32(r->region.bus, r->region.dev, r->region.fn, off & ~3) >> ((off & 3) * 8);
    return w == 32 ? v : v & ((1u << w) - 1);
}
static void cfg_wr(volatile uint8_t *e, struct aml_obj *r, uint16_t off, int w, uint32_t v) {
    if (e) { if (w == 32) *(volatile uint32_t *)(e + off) = v; else if (w == 16) *(volatile uint16_t *)(e + off) = v; else e[off] = v; }
    else if (w == 32) pci_write32(r->region.bus, r->region.dev, r->region.fn, off, v);
    else if (w == 16) pci_write16(r->region.bus, r->region.dev, r->region.fn, off, v);
    else pci_write8(r->region.bus, r->region.dev, r->region.fn, off, v);
}
static volatile uint8_t *cfg_window(struct aml_obj *r, uint64_t off, int width, bool *ok) {
    uint8_t *bus = acpi_ecam(r->region.seg, r->region.bus);
    *ok = bus ? off + width / 8 <= 4096 : !r->region.seg && off + width / 8 <= 256;   /* legacy: segment 0 only */
    return bus ? bus + ((uint32_t)r->region.dev << 15 | (uint32_t)r->region.fn << 12) : NULL;
}
static int cfg_chunk(uint64_t off, int left) { return !(off & 3) && left >= 32 ? 32 : !(off & 1) && left >= 16 ? 16 : 8; }
static uint64_t pci_cfg_read(struct aml_obj *r, uint64_t off, int width) {
    bool ok; volatile uint8_t *e = cfg_window(r, off, width, &ok);
    if (!ok) return ~0ULL;
    uint64_t v = 0;
    for (int done = 0, w; done < width; done += w, off += w / 8) { w = cfg_chunk(off, width - done); v |= (uint64_t)cfg_rd(e, r, off, w) << done; }
    return v;
}
static void pci_cfg_write(struct aml_obj *r, uint64_t off, int width, uint64_t v) {
    bool ok; volatile uint8_t *e = cfg_window(r, off, width, &ok);
    if (!ok) return;
    for (int done = 0, w; done < width; done += w, off += w / 8) { w = cfg_chunk(off, width - done); cfg_wr(e, r, off, w, v >> done); }
}

/* _ADR of a PCI device: device in the high word, function in the low one (0xFFFF: all functions, use 0) */
static void adr_decode(uint64_t adr, uint8_t *dev, uint8_t *fn) {
    uint32_t d = (adr >> 16) & 0xFFFF, f = adr & 0xFFFF;
    *dev = d & 0x1F; *fn = f == 0xFFFF ? 0 : f & 7;
}

/* PCI_Config regions address the device that contains them (as ACPICA
 * does): the nearest enclosing Device with an _ADR gives device/function,
 * the root bridge's _BBN the bus, and every PCI-PCI bridge on the way down
 * moves to its secondary bus. */
static bool pci_root(struct aml_node *d) { return ns_hid_matches(d, "PNP0A03") || ns_hid_matches(d, "PNP0A08") || ns_child(d, "_BBN"); }
static void pci_resolve(struct aml_obj *rgn) {
    if (rgn->region.pci_resolved) return;
    rgn->region.pci_resolved = true;
    struct aml_node *dev = NULL, *root = NULL, *chain[16]; int n = 0;
    for (struct aml_node *d = rgn->region.node ? rgn->region.node->parent : NULL; d && d != aml_root; d = d->parent) {
        if (!d->obj || d->obj->type != AML_DEVICE) continue;   /* methods, processors, ... */
        bool is_root = pci_root(d);
        if (!dev) { if (is_root || ns_child(d, "_ADR")) dev = d; }
        else if (!is_root && n < 16 && ns_child(d, "_ADR")) chain[n++] = d;
        if (is_root) { root = d; break; }
    }
    uint64_t bus = 0, adr = 0, seg = 0;
    if (root && ns_child(root, "_BBN")) aml_eval_int(root, "_BBN", &bus);
    if (root && ns_child(root, "_SEG")) aml_eval_int(root, "_SEG", &seg);
    rgn->region.seg = seg;
    for (int i = n - 1; i >= 0; i--) {   /* devices between the root and the region's device, top down */
        if (aml_eval_int(chain[i], "_ADR", &adr) < 0) break;
        uint8_t sl, f; adr_decode(adr, &sl, &f);
        if ((pci_read32(bus, sl, f, 0) & 0xFFFF) == 0xFFFF) break;
        uint8_t ht = (pci_read32(bus, sl, f, 0x0C) >> 16) & 0x7F;
        if (ht != 1 && ht != 2) break;   /* not a PCI-PCI / CardBus bridge: nothing below it is on PCI */
        uint8_t sec = (pci_read32(bus, sl, f, 0x18) >> 8) & 0xFF;
        if (!sec || sec == 0xFF) break;
        bus = sec;
    }
    adr = 0;
    if (dev && ns_child(dev, "_ADR")) aml_eval_int(dev, "_ADR", &adr);
    rgn->region.bus = bus; adr_decode(adr, &rgn->region.dev, &rgn->region.fn);
}

/* CMOS bank 0 at 0x70/0x71 (bit 7 of 0x70 is NMI disable: keep it), bank 1 at 0x72/0x73 */
static uint64_t cmos_rw(uint64_t a, bool write, uint8_t v) {
    if (a >= 256) return 0;
    if (a < 128) outb(0x70, (inb(0x70) & 0x80) | a); else outb(0x72, a);
    uint16_t data = a < 128 ? 0x71 : 0x73;
    if (write) { outb(data, v); return 0; }
    return inb(data);
}

static void unsupported(struct aml_obj *rgn) {
    static uint32_t warned;
    if (rgn->region.space < 32 && !(warned & (1u << rgn->region.space))) {
        warned |= 1u << rgn->region.space;
        kprintf("acpi: address space %d is not supported (reads return 0)\n", rgn->region.space);
    }
}

static uint8_t ec_read(uint8_t addr);
static void ec_write(uint8_t addr, uint8_t v);

/* as ACPICA's AE_AML_REGION_LIMIT: an access beyond the region's length is refused */
static bool in_region(struct aml_obj *rgn, uint64_t off, int width) {
    uint64_t n = width / 8;
    if (off <= rgn->region.len && n <= rgn->region.len - off) return true;
    if (!rgn->region.limit_warned) {
        rgn->region.limit_warned = true; aml_errors++;
        char p[96]; if (rgn->region.node) ns_path(rgn->region.node, p, sizeof p); else strlcpy(p, "?", sizeof p);
        kprintf("acpi: %d-bit access at 0x%lx beyond region %s (0x%lx bytes) refused\n", width, off, p, rgn->region.len);
    }
    return false;
}

uint64_t region_read(struct aml_obj *rgn, uint64_t off, int width) {
    if (!in_region(rgn, off, width)) return 0;
    uint64_t a = rgn->region.base + off;
    switch (rgn->region.space) {
    case SPACE_MEMORY: return mem_read(a, width);
    case SPACE_IO: return io_read(a, width);
    case SPACE_PCI: pci_resolve(rgn); return pci_cfg_read(rgn, a, width);
    case SPACE_EC: { uint64_t v = 0; for (int i = 0; i < width / 8; i++) v |= (uint64_t)ec_read(a + i) << (8 * i); return v; }
    case SPACE_CMOS: { uint64_t v = 0; for (int i = 0; i < width / 8; i++) v |= cmos_rw(a + i, false, 0) << (8 * i); return v; }
    default: unsupported(rgn); return 0;
    }
}

void region_write(struct aml_obj *rgn, uint64_t off, int width, uint64_t v) {
    if (!in_region(rgn, off, width)) return;
    uint64_t a = rgn->region.base + off;
    switch (rgn->region.space) {
    case SPACE_MEMORY: mem_write(a, width, v); break;
    case SPACE_IO: io_write(a, width, v); break;
    case SPACE_PCI: pci_resolve(rgn); pci_cfg_write(rgn, a, width, v); break;
    case SPACE_EC: for (int i = 0; i < width / 8; i++) ec_write(a + i, v >> (8 * i)); break;
    case SPACE_CMOS: for (int i = 0; i < width / 8; i++) cmos_rw(a + i, true, v >> (8 * i)); break;
    default: unsupported(rgn); break;
    }
}

/* ---- field units ---- */
static struct aml_obj *field_region(struct aml_obj *f) {
    struct aml_node *n = ns_resolve_alias(f->field.region);
    return n && n->obj && n->obj->type == AML_REGION ? n->obj : NULL;
}

static int access_bits(struct aml_obj *f) {
    struct aml_obj *rgn = f->field.kind != FIELD_INDEX ? field_region(f) : NULL;
    if (rgn && (rgn->region.space == SPACE_EC || rgn->region.space == SPACE_CMOS)) return 8;
    switch (f->field.flags & 0xF) {
    case 1: return 8;
    case 2: return 16;
    case 3: return 32;
    case 4: return 64;
    default:   /* AnyAcc / BufferAcc: the smallest aligned unit holding the whole field, if the region does */
        for (int w = 8; w <= 32; w *= 2)
            if (f->field.bit_off / w == (f->field.bit_off + f->field.bit_len - 1) / w)
                return !rgn || (f->field.bit_off / w + 1) * (w / 8) <= rgn->region.len ? w : 8;
        return 8;
    }
}

static uint64_t unit_read(struct aml_obj *f, uint64_t byte_off, int w) {
    switch (f->field.kind) {
    case FIELD_INDEX: {
        struct aml_node *idx = ns_resolve_alias(f->field.index), *data = ns_resolve_alias(f->field.data);
        if (!idx->obj || !data->obj || idx->obj->type != AML_FIELD || data->obj->type != AML_FIELD) return 0;
        struct aml_obj *o = obj_int(byte_off); field_write(idx->obj, o); obj_put(o);
        struct aml_obj *v = field_read(data->obj); uint64_t x = 0; obj_to_int(v, &x); obj_put(v);
        return x;
    }
    case FIELD_BANK: {
        struct aml_node *bank = ns_resolve_alias(f->field.bank);
        if (bank->obj && bank->obj->type == AML_FIELD) { struct aml_obj *o = obj_int(f->field.bank_val); field_write(bank->obj, o); obj_put(o); }
    }
    /* fallthrough */
    default: {
        struct aml_obj *rgn = field_region(f);
        return rgn ? region_read(rgn, byte_off, w) : 0;
    }
    }
}

static void unit_write(struct aml_obj *f, uint64_t byte_off, int w, uint64_t v) {
    switch (f->field.kind) {
    case FIELD_INDEX: {
        struct aml_node *idx = ns_resolve_alias(f->field.index), *data = ns_resolve_alias(f->field.data);
        if (!idx->obj || !data->obj || idx->obj->type != AML_FIELD || data->obj->type != AML_FIELD) return;
        struct aml_obj *o = obj_int(byte_off); field_write(idx->obj, o); obj_put(o);
        o = obj_int(v); field_write(data->obj, o); obj_put(o);
        return;
    }
    case FIELD_BANK: {
        struct aml_node *bank = ns_resolve_alias(f->field.bank);
        if (bank->obj && bank->obj->type == AML_FIELD) { struct aml_obj *o = obj_int(f->field.bank_val); field_write(bank->obj, o); obj_put(o); }
    }
    /* fallthrough */
    default: {
        struct aml_obj *rgn = field_region(f);
        if (rgn) region_write(rgn, byte_off, w, v);
    }
    }
}

static void put_bits(uint8_t *data, uint64_t pos, uint64_t v, int n) {
    for (int i = 0; i < n; i++) if ((v >> i) & 1) data[(pos + i) / 8] |= 1 << ((pos + i) % 8);
}
static uint64_t get_bits(const uint8_t *data, uint64_t pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) if (data[(pos + i) / 8] & (1 << ((pos + i) % 8))) v |= 1ULL << i;
    return v;
}

static void field_xfer(struct aml_obj *f, uint8_t *data, bool write) {
    uint64_t off = f->field.bit_off, len = f->field.bit_len;
    int w = access_bits(f);
    int rule = (f->field.flags >> 5) & 3;
    for (uint64_t pos = 0; pos < len; ) {
        uint64_t abs = off + pos, unit = abs / w * w;
        int shift = abs - unit;
        int n = MIN((uint64_t)(w - shift), len - pos);
        uint64_t mask = n >= 64 ? ~0ULL : (1ULL << n) - 1;
        if (!write) put_bits(data, pos, (unit_read(f, unit / 8, w) >> shift) & mask, n);
        else {
            uint64_t bits = get_bits(data, pos, n), v = bits;
            if (n < w) {
                uint64_t base = rule == 0 ? unit_read(f, unit / 8, w) : rule == 1 ? ~0ULL : 0;
                v = (base & ~(mask << shift)) | (bits << shift);
            }
            unit_write(f, unit / 8, w, v);
        }
        pos += n;
    }
}

static struct aml_obj *bits_to_obj(const uint8_t *data, uint64_t len) {
    if (len > (aml_int64 ? 64u : 32u)) return obj_buf(data, (len + 7) / 8);   /* wider than an Integer */
    uint64_t v = 0;
    for (uint64_t i = 0; i < (len + 7) / 8; i++) v |= (uint64_t)data[i] << (8 * i);
    return obj_int(v);
}
static uint8_t *obj_to_bits(struct aml_obj *v, uint64_t len) {
    size_t n = (len + 7) / 8 + 8;
    uint8_t *data = kzalloc(n);
    if (v->type == AML_INTEGER) memcpy(data, &v->ival, 8);
    else if (v->type == AML_BUFFER) memcpy(data, v->buf.b, MIN(v->buf.len, n));
    else if (v->type == AML_STRING) memcpy(data, v->str.s, MIN(v->str.len, n));
    return data;
}

/* LockRule = Lock: hold the global lock across the access (on a timeout, go ahead as before) */
#define GL_FIELD_TIMEOUT_MS 1000
static bool field_lock(struct aml_obj *f) { return (f->field.flags & 0x10) && acpi_gl_acquire(GL_FIELD_TIMEOUT_MS); }

struct aml_obj *field_read(struct aml_obj *f) {
    uint8_t *data = kzalloc((f->field.bit_len + 7) / 8 + 8);
    bool lk = field_lock(f);
    field_xfer(f, data, false);
    if (lk) acpi_gl_release();
    struct aml_obj *r = bits_to_obj(data, f->field.bit_len);
    kfree(data);
    return r;
}

void field_write(struct aml_obj *f, struct aml_obj *v) {
    uint8_t *data = obj_to_bits(v, f->field.bit_len);
    bool lk = field_lock(f);
    field_xfer(f, data, true);
    if (lk) acpi_gl_release();
    kfree(data);
}

struct aml_obj *bfield_read(struct aml_obj *f) {
    struct aml_obj *b = f->bfield.buf;
    uint64_t len = f->bfield.bit_len;
    uint8_t *data = kzalloc((len + 7) / 8 + 8);
    for (uint64_t i = 0; i < len && f->bfield.bit_off + i < b->buf.len * 8; i++) {
        uint64_t s = f->bfield.bit_off + i;
        if (b->buf.b[s / 8] & (1 << (s % 8))) data[i / 8] |= 1 << (i % 8);
    }
    struct aml_obj *r = bits_to_obj(data, len);
    kfree(data);
    return r;
}

void bfield_write(struct aml_obj *f, struct aml_obj *v) {
    struct aml_obj *b = f->bfield.buf;
    uint64_t len = f->bfield.bit_len;
    uint8_t *data = obj_to_bits(v, len);
    for (uint64_t i = 0; i < len && f->bfield.bit_off + i < b->buf.len * 8; i++) {
        uint64_t d = f->bfield.bit_off + i;
        if (data[i / 8] & (1 << (i % 8))) b->buf.b[d / 8] |= 1 << (d % 8);
        else b->buf.b[d / 8] &= ~(1 << (d % 8));
    }
    kfree(data);
}

/* ---- Generic Address Structure ---- */
static int gas_width(const struct acpi_gas *g) {
    if (g->access_size >= 1 && g->access_size <= 4) return 8 << (g->access_size - 1);
    return g->bit_width >= 64 ? 64 : g->bit_width >= 32 ? 32 : g->bit_width >= 16 ? 16 : 8;
}
uint64_t gas_read(const struct acpi_gas *g) {
    if (!g->address) return 0;
    int w = gas_width(g);
    if (g->space == SPACE_MEMORY) return mem_read(g->address, w);
    if (g->space == SPACE_IO) return io_read(g->address, w);
    return 0;
}
void gas_write(const struct acpi_gas *g, uint64_t v) {
    if (!g->address) return;
    int w = gas_width(g);
    if (g->space == SPACE_MEMORY) mem_write(g->address, w, v);
    else if (g->space == SPACE_IO) io_write(g->address, w, v);
}

/* ---- embedded controller ---- */
#define EC_OBF     0x01
#define EC_IBF     0x02
#define EC_SCI_EVT 0x20
#define EC_READ    0x80
#define EC_WRITE   0x81
#define EC_QUERY   0x84

static uint16_t ec_data, ec_cmd;
static bool ec_present;
static int ec_gpe_num = -1;
static struct aml_node *ec_node;
static char ec_desc[96];

static bool ec_wait(uint8_t mask, uint8_t want) {
    for (int i = 0; i < 20000; i++) {   /* ~200 ms */
        if ((inb(ec_cmd) & mask) == want) return true;
        acpi_stall_us(10);
    }
    /* a dead EC would cost this timeout on every field access: stop using it */
    kprintf("acpi: embedded controller not responding, disabled\n");
    ec_present = false;
    return false;
}
static uint8_t ec_read(uint8_t addr) {
    if (!ec_present) return 0;
    if (!ec_wait(EC_IBF, 0)) return 0;
    outb(ec_cmd, EC_READ);
    if (!ec_wait(EC_IBF, 0)) return 0;
    outb(ec_data, addr);
    if (!ec_wait(EC_OBF, EC_OBF)) return 0;
    return inb(ec_data);
}
static void ec_write(uint8_t addr, uint8_t v) {
    if (!ec_present) return;
    if (!ec_wait(EC_IBF, 0)) return;
    outb(ec_cmd, EC_WRITE);
    if (!ec_wait(EC_IBF, 0)) return;
    outb(ec_data, addr);
    if (!ec_wait(EC_IBF, 0)) return;
    outb(ec_data, v);
    ec_wait(EC_IBF, 0);
}

struct ec_find { struct aml_node *found; };
static bool ec_find_cb(struct aml_node *n, int depth, void *ctx) {
    struct ec_find *f = ctx;
    if (f->found) return false;
    if (!n->obj || n->obj->type != AML_DEVICE) return n->obj && n->obj->type != AML_METHOD;
    if (ns_hid_matches(n, "PNP0C09")) { f->found = n; return false; }
    return true;
}

/* the I/O port descriptors of _CRS: data port first, command/status second */
static int crs_io_ports(struct aml_obj *buf, uint16_t *ports, int max) {
    int n = 0;
    for (size_t i = 0; i < buf->buf.len && n < max; ) {
        uint8_t tag = buf->buf.b[i];
        if (tag & 0x80) {   /* large descriptor */
            if (i + 3 > buf->buf.len) break;
            i += 3 + (buf->buf.b[i + 1] | buf->buf.b[i + 2] << 8);
            continue;
        }
        uint8_t type = (tag >> 3) & 0xF, len = tag & 7;
        if (type == 0xF) break;
        if (type == 0x8 && len >= 7 && i + 3 < buf->buf.len) ports[n++] = buf->buf.b[i + 2] | buf->buf.b[i + 3] << 8;
        else if (type == 0x9 && len >= 3 && i + 2 < buf->buf.len) ports[n++] = (buf->buf.b[i + 1] | buf->buf.b[i + 2] << 8) & 0x3FF;
        i += 1 + len;
    }
    return n;
}

bool ec_init(void) {
    /* the ECDT describes the EC before the namespace is walked; else find the device */
    struct ecdt { char sig[4]; uint32_t len; uint8_t hdr[28]; struct acpi_gas ctl, data; uint32_t uid; uint8_t gpe; char id[]; } PACKED;
    char sig[5];
    for (int i = 0; acpi_table_at(i, sig); i++) {
        if (memcmp(sig, "ECDT", 4)) continue;
        struct ecdt *e = acpi_table_at(i, sig);
        ec_cmd = e->ctl.address; ec_data = e->data.address; ec_gpe_num = e->gpe;
        ec_node = ns_lookup(aml_root, e->id);
    }
    if (!ec_node) {
        struct ec_find f = { NULL };
        ns_walk(ns_lookup(aml_root, "\\_SB_"), ec_find_cb, &f);
        ec_node = f.found;
    }
    if (!ec_node) return false;
    if (!ec_cmd) {
        struct aml_obj *crs;
        if (aml_eval_path(ec_node, "_CRS", NULL, 0, &crs) == 0) {
            uint16_t ports[2];
            if (crs->type == AML_BUFFER && crs_io_ports(crs, ports, 2) == 2) { ec_data = ports[0]; ec_cmd = ports[1]; }
            obj_put(crs);
        }
    }
    uint64_t gpe;
    if (ec_gpe_num < 0 && aml_eval_int(ec_node, "_GPE", &gpe) == 0) ec_gpe_num = gpe;
    if (!ec_cmd || !ec_data) { kprintf("acpi: embedded controller without ports\n"); ec_node = NULL; return false; }
    ec_present = true;
    char path[64]; ns_path(ec_node, path, sizeof path);
    snprintf(ec_desc, sizeof ec_desc, "%s, ports 0x%x/0x%x, GPE %d", path, ec_data, ec_cmd, ec_gpe_num);
    kprintf("acpi: embedded controller %s\n", ec_desc);
    /* tell the firmware the EC address space is usable now */
    if (ns_child(ec_node, "_REG")) {
        struct aml_obj *args[2] = { obj_int(SPACE_EC), obj_int(1) };
        aml_eval_path(ec_node, "_REG", args, 2, NULL);
        obj_put(args[0]); obj_put(args[1]);
    }
    return true;
}

int ec_gpe(void) { return ec_present ? ec_gpe_num : -1; }
const char *ec_describe(void) { return ec_present ? ec_desc : NULL; }

/* SCI_EVT: query event numbers and run the matching _Qxx methods */
void ec_handle_events(void) {
    aml_lock();   /* EC transactions must not interleave with AML touching the EC */
    for (int guard = 0; ec_present && guard < 32 && (inb(ec_cmd) & EC_SCI_EVT); guard++) {
        if (!ec_wait(EC_IBF, 0)) break;
        outb(ec_cmd, EC_QUERY);
        if (!ec_wait(EC_OBF, EC_OBF)) break;
        uint8_t q = inb(ec_data);
        if (!q) break;
        char name[8]; snprintf(name, sizeof name, "_Q%02X", q);
        if (ns_child(ec_node, name)) aml_eval_path(ec_node, name, NULL, 0, NULL);
    }
    aml_unlock();
}
