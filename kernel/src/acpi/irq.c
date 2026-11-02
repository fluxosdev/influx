/*
 * PCI INTx routing through the namespace. Each PCI root bridge (and any
 * PCI-PCI bridge the firmware describes) has a _PRT mapping (slot, pin) to
 * either a GSI or a link device whose _CRS holds the interrupt. Bridges
 * without a _PRT swizzle the pin and pass the lookup up to their parent bus.
 */
#include "acpi_internal.h"
#include "string.h"

struct root { struct aml_node *node; uint64_t bus; };
static struct root roots[8];
static int nroots = -1;

static bool root_cb(struct aml_node *n, int depth, void *ctx) {
    if (!n->obj || n->obj->type != AML_DEVICE) return false;
    if (nroots < (int)ARRAY_LEN(roots) && (ns_hid_matches(n, "PNP0A03") || ns_hid_matches(n, "PNP0A08"))) {
        uint64_t sta = 0xF;
        if (ns_child(n, "_STA")) aml_eval_int(n, "_STA", &sta);
        if (sta & 1) {
            uint64_t bus = 0;
            if (ns_child(n, "_BBN")) aml_eval_int(n, "_BBN", &bus);
            roots[nroots].node = n; roots[nroots].bus = bus; nroots++;
        }
        return false;
    }
    return true;
}
static void find_roots(void) {
    if (nroots >= 0) return;
    nroots = 0;
    struct aml_node *sb = ns_lookup(aml_root, "\\_SB_");
    if (sb) ns_walk(sb, root_cb, NULL);
}

static struct pci_device *bridge_to(uint8_t bus) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->header_type == 1 && d->secondary_bus == bus) return d;
    }
    return NULL;
}

static struct aml_node *adr_child(struct aml_node *parent, uint32_t adr) {
    for (struct aml_node *c = parent->children; c; c = c->next) {
        uint64_t a;
        if (!c->obj || c->obj->type != AML_DEVICE || !ns_child(c, "_ADR")) continue;
        if (aml_eval_int(c, "_ADR", &a) == 0 && (a == adr || a == ((adr & 0xFFFF0000u) | 0xFFFF))) return c;
    }
    return NULL;
}

/* the namespace device that represents PCI bus `bus` */
static struct aml_node *bus_node(uint8_t bus, int depth) {
    for (int i = 0; i < nroots; i++) if (roots[i].bus == bus) return roots[i].node;
    if (depth > 8) return NULL;
    struct pci_device *br = bridge_to(bus);
    if (!br) return NULL;
    struct aml_node *parent = bus_node(br->bus, depth + 1);
    return parent ? adr_child(parent, (uint32_t)br->slot << 16 | br->func) : NULL;
}

/* first interrupt in a resource template: IRQ() or Interrupt() */
static bool parse_irq(struct aml_obj *buf, uint32_t *irq, bool *level, bool *low, size_t *desc_off) {
    for (size_t i = 0; i < buf->buf.len; ) {
        uint8_t *b = buf->buf.b + i, tag = b[0];
        if (tag & 0x80) {
            if (i + 3 > buf->buf.len) return false;
            size_t len = b[1] | b[2] << 8;
            if ((tag & 0x7F) == 0x09 && len >= 6 && i + 3 + len <= buf->buf.len) {   /* Extended Interrupt */
                *level = !(b[3] & 2); *low = b[3] & 4;
                *irq = b[5] | b[6] << 8 | b[7] << 16 | (uint32_t)b[8] << 24;
                if (desc_off) *desc_off = i;
                return true;
            }
            i += 3 + len;
            continue;
        }
        uint8_t type = (tag >> 3) & 0xF, len = tag & 7;
        if (type == 0xF) break;
        if (type == 0x4 && len >= 2 && i + 2 < buf->buf.len) {   /* IRQ */
            uint16_t mask = b[1] | b[2] << 8;
            uint8_t info = len >= 3 ? b[3] : 0x01;   /* no info byte: edge, active high */
            *level = !(info & 1); *low = info & 8;
            *irq = mask ? __builtin_ctz(mask) : 0;
            if (desc_off) *desc_off = i;
            return true;
        }
        i += 1 + len;
    }
    return false;
}

/* a disabled link: pick its first possible interrupt and program it with _SRS */
static bool link_enable(struct aml_node *link) {
    struct aml_obj *prs;
    if (!ns_child(link, "_PRS") || !ns_child(link, "_SRS") || aml_eval_path(link, "_PRS", NULL, 0, &prs) < 0) return false;
    uint32_t irq; bool level, low; size_t off;
    bool ok = prs->type == AML_BUFFER && parse_irq(prs, &irq, &level, &low, &off) && irq;
    if (ok) {
        uint8_t *d = prs->buf.b + off;
        bool ext = d[0] & 0x80;
        size_t dlen = ext ? 9 : 1 + (d[0] & 7);   /* Interrupt(): flags, count 1, one GSI */
        struct aml_obj *srs = obj_buf(NULL, dlen + 2);
        memcpy(srs->buf.b, d, dlen);
        if (ext) { srs->buf.b[1] = 6; srs->buf.b[2] = 0; srs->buf.b[4] = 1; srs->buf.b[5] = irq; srs->buf.b[6] = irq >> 8; srs->buf.b[7] = irq >> 16; srs->buf.b[8] = irq >> 24; }
        else { srs->buf.b[1] = (1u << irq) & 0xFF; srs->buf.b[2] = (1u << irq) >> 8; }
        srs->buf.b[dlen] = 0x79;
        ok = aml_eval_path(link, "_SRS", &srs, 1, NULL) == 0;
        obj_put(srs);
    }
    obj_put(prs);
    return ok;
}

static bool link_irq(struct aml_node *link, struct acpi_irq_route *out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        struct aml_obj *crs;
        if (aml_eval_path(link, "_CRS", NULL, 0, &crs) < 0) return false;
        uint32_t irq = 0; bool level = true, low = true;
        bool ok = crs->type == AML_BUFFER && parse_irq(crs, &irq, &level, &low, NULL);
        obj_put(crs);
        if (ok && irq) { out->gsi = irq; out->level = level; out->active_low = low; return true; }
        if (attempt || !link_enable(link)) return false;
    }
    return false;
}

static bool prt_lookup(struct aml_node *busdev, uint8_t slot, uint8_t pin, struct acpi_irq_route *out) {
    struct aml_obj *prt;
    if (aml_eval_path(busdev, "_PRT", NULL, 0, &prt) < 0) return false;
    bool found = false;
    aml_lock();
    for (size_t i = 0; prt->type == AML_PACKAGE && i < prt->pkg.n && !found; i++) {
        struct aml_obj *e = aml_value_of(prt->pkg.e[i]);
        if (e->type == AML_PACKAGE && e->pkg.n >= 4) {
            uint64_t adr = 0, p = 9, idx = 0;
            struct aml_obj *v;
            v = aml_value_of(e->pkg.e[0]); obj_to_int(v, &adr); obj_put(v);
            v = aml_value_of(e->pkg.e[1]); obj_to_int(v, &p); obj_put(v);
            v = aml_value_of(e->pkg.e[3]); obj_to_int(v, &idx); obj_put(v);
            if (((adr >> 16) & 0xFFFF) == slot && p == (uint64_t)(pin - 1)) {
                struct aml_obj *src = e->pkg.e[2];
                struct aml_node *link = NULL;
                if (src->type == AML_REFERENCE && src->ref.kind == REF_NAMED) link = ns_resolve_alias(src->ref.node);
                else if (src->type == AML_NAMEREF) link = ns_lookup_search(src->nameref.scope, src->nameref.path);
                else if (src->type == AML_STRING) link = ns_lookup_search(busdev, src->str.s);
                if (link) found = link_irq(link, out);
                else { out->gsi = idx; out->level = true; out->active_low = true; found = true; }   /* hard-wired GSI */
            }
        }
        obj_put(e);
    }
    aml_unlock();
    obj_put(prt);
    return found;
}

bool acpi_pci_route(uint8_t bus, uint8_t slot, uint8_t func, uint8_t pin, struct acpi_irq_route *out) {
    if (!acpi_ready() || pin < 1 || pin > 4) return false;
    aml_lock();
    find_roots();
    bool ok = false;
    for (int hop = 0; hop < 8; hop++) {
        struct aml_node *bn = bus_node(bus, 0);
        if (bn && ns_child(bn, "_PRT")) { ok = prt_lookup(bn, slot, pin, out); break; }
        struct pci_device *br = bridge_to(bus);
        if (!br) break;
        pin = (pin - 1 + slot) % 4 + 1;   /* standard bridge swizzle */
        slot = br->slot; bus = br->bus;
    }
    aml_unlock();
    return ok;
}

void acpi_print_irqs(acpi_print_fn out, void *ctx) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        uint8_t pin = (pci_read32(d->bus, d->slot, d->func, 0x3C) >> 8) & 0xFF;
        if (!pin || pin > 4) continue;
        char line[128];
        struct acpi_irq_route r;
        if (acpi_pci_route(d->bus, d->slot, d->func, pin, &r))
            snprintf(line, sizeof line, "%02x:%02x.%d INT%c -> GSI %u (%s, active %s)  %s", d->bus, d->slot, d->func, 'A' + pin - 1, r.gsi,
                     r.level ? "level" : "edge", r.active_low ? "low" : "high", pci_class_name(d->class_code, d->subclass));
        else snprintf(line, sizeof line, "%02x:%02x.%d INT%c -> no route  %s", d->bus, d->slot, d->func, 'A' + pin - 1, pci_class_name(d->class_code, d->subclass));
        out(ctx, line);
    }
}
