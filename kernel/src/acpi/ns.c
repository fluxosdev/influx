/*
 * AML objects (reference counted) and the ACPI namespace tree, plus the
 * namespace dump / evaluate helpers behind the shell's `acpi` command.
 */
#include "acpi_internal.h"
#include "mm.h"
#include "string.h"

struct aml_node *aml_root;
bool aml_int64 = true;
static int node_count;

/* ---- objects ---- */
struct aml_obj *obj_new(int type) { struct aml_obj *o = kzalloc(sizeof *o); o->type = type; o->refs = 1; return o; }
struct aml_obj *obj_int(uint64_t v) { struct aml_obj *o = obj_new(AML_INTEGER); o->ival = v; return o; }
struct aml_obj *obj_str(const char *s, size_t len) {
    struct aml_obj *o = obj_new(AML_STRING);
    o->str.s = kmalloc(len + 1); if (len) memcpy(o->str.s, s, len); o->str.s[len] = 0; o->str.len = len;
    return o;
}
struct aml_obj *obj_buf(const void *data, size_t len) {
    struct aml_obj *o = obj_new(AML_BUFFER);
    o->buf.b = kzalloc(len ? len : 1); if (data && len) memcpy(o->buf.b, data, len); o->buf.len = len;
    return o;
}
struct aml_obj *obj_pkg(size_t n) {
    struct aml_obj *o = obj_new(AML_PACKAGE);
    o->pkg.e = kzalloc((n ? n : 1) * sizeof(struct aml_obj *)); o->pkg.n = n;
    for (size_t i = 0; i < n; i++) o->pkg.e[i] = obj_new(AML_UNINIT);
    return o;
}
struct aml_obj *obj_ref_named(struct aml_node *n) { struct aml_obj *o = obj_new(AML_REFERENCE); o->ref.kind = REF_NAMED; o->ref.node = n; return o; }

void obj_put(struct aml_obj *o) {
    if (!o || --o->refs > 0) return;
    switch (o->type) {
    case AML_STRING: kfree(o->str.s); break;
    case AML_BUFFER: kfree(o->buf.b); break;
    case AML_PACKAGE: for (size_t i = 0; i < o->pkg.n; i++) obj_put(o->pkg.e[i]); kfree(o->pkg.e); break;
    case AML_BUFFER_FIELD: obj_put(o->bfield.buf); break;
    case AML_REFERENCE: if (o->ref.kind == REF_INDEX) obj_put(o->ref.target); break;
    case AML_NAMEREF: kfree(o->nameref.path); break;
    }
    kfree(o);
}

struct aml_obj *obj_copy(struct aml_obj *o) {
    switch (o->type) {
    case AML_INTEGER: return obj_int(o->ival);
    case AML_STRING: return obj_str(o->str.s, o->str.len);
    case AML_BUFFER: return obj_buf(o->buf.b, o->buf.len);
    case AML_PACKAGE: {
        struct aml_obj *p = obj_new(AML_PACKAGE);
        p->pkg.e = kzalloc((o->pkg.n ? o->pkg.n : 1) * sizeof(struct aml_obj *)); p->pkg.n = o->pkg.n;
        for (size_t i = 0; i < o->pkg.n; i++) p->pkg.e[i] = obj_copy(o->pkg.e[i]);
        return p;
    }
    default: return obj_get(o);
    }
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool obj_to_int(struct aml_obj *o, uint64_t *out) {
    switch (o->type) {
    case AML_INTEGER: *out = o->ival; return true;
    case AML_STRING: {   /* implicit conversion reads the string as hex */
        const char *s = o->str.s; uint64_t v = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        for (int n = 0; hexval(*s) >= 0 && n < 16; s++, n++) v = (v << 4) | hexval(*s);
        *out = aml_trunc(v); return true;
    }
    case AML_BUFFER: {
        uint64_t v = 0; size_t n = MIN(o->buf.len, aml_int64 ? 8u : 4u);
        for (size_t i = 0; i < n; i++) v |= (uint64_t)o->buf.b[i] << (8 * i);
        *out = v; return true;
    }
    default: return false;
    }
}

struct aml_obj *obj_to_buffer(struct aml_obj *o) {
    switch (o->type) {
    case AML_INTEGER: { uint64_t v = o->ival; return obj_buf(&v, aml_int64 ? 8 : 4); }
    case AML_STRING: return obj_buf(o->str.s, o->str.len + 1);   /* the NUL travels along, as in ACPICA */
    case AML_BUFFER: return obj_get(o);
    default: return NULL;
    }
}

struct aml_obj *obj_to_string(struct aml_obj *o) {
    switch (o->type) {
    case AML_INTEGER: { char s[20]; int n = snprintf(s, sizeof s, aml_int64 ? "%016lX" : "%08lX", o->ival); return obj_str(s, n); }
    case AML_STRING: return obj_get(o);
    case AML_BUFFER: {
        size_t n = o->buf.len ? o->buf.len * 3 - 1 : 0;
        char *tmp = kmalloc(n + 4);
        for (size_t i = 0; i < o->buf.len; i++) snprintf(tmp + i * 3, 4, i + 1 < o->buf.len ? "%02X " : "%02X", o->buf.b[i]);
        struct aml_obj *r = obj_str(tmp, n);
        kfree(tmp);
        return r;
    }
    default: return NULL;
    }
}

const char *obj_type_name(int type) {
    static const char *names[] = { "Uninitialized", "Integer", "String", "Buffer", "Package", "FieldUnit", "Device", "Event", "Method",
                                   "Mutex", "OpRegion", "PowerResource", "Processor", "ThermalZone", "BufferField", "DDBHandle", "Debug" };
    if (type >= 0 && type < (int)ARRAY_LEN(names)) return names[type];
    if (type == AML_REFERENCE) return "Reference";
    if (type == AML_NAMEREF) return "NameReference";
    return "?";
}

/* ---- namespace ---- */
static void seg_norm(char out[4], const char *seg, size_t len) {
    for (int i = 0; i < 4; i++) out[i] = i < (int)len ? seg[i] : '_';
}

void ns_init(void) {
    aml_root = kzalloc(sizeof *aml_root);
    strcpy(aml_root->name, "\\");
    aml_root->obj = obj_new(AML_DEVICE);
    node_count = 1;
}
int ns_node_count(void) { return node_count; }

struct aml_node *ns_child(struct aml_node *parent, const char *seg) {
    parent = ns_resolve_alias(parent);
    for (struct aml_node *c = parent->children; c; c = c->next) if (!memcmp(c->name, seg, 4)) return c;
    return NULL;
}

struct aml_node *ns_add(struct aml_node *parent, const char *seg) {
    parent = ns_resolve_alias(parent);
    struct aml_node *n = kzalloc(sizeof *n);
    memcpy(n->name, seg, 4);
    n->parent = parent;
    struct aml_node **pp = &parent->children;
    while (*pp) pp = &(*pp)->next;
    *pp = n;
    node_count++;
    return n;
}

static void free_subtree(struct aml_node *n) {
    struct aml_node *c = n->children;
    while (c) { struct aml_node *next = c->next; free_subtree(c); c = next; }
    obj_put(n->obj);
    kfree(n);
    node_count--;
}

void ns_remove(struct aml_node *n) {
    if (!n || !n->parent) return;
    struct aml_node **pp = &n->parent->children;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    free_subtree(n);
}

/* C-side lookup: relative paths are exact (no AML search toward the root) */
struct aml_node *ns_lookup(struct aml_node *scope, const char *path) {
    struct aml_node *n = scope ? scope : aml_root;
    const char *p = path;
    if (*p == '\\') { n = aml_root; p++; }
    while (*p == '^') { if (n->parent) n = n->parent; p++; }
    if (!*p) return n;
    while (*p) {
        const char *dot = strchr(p, '.'); size_t l = dot ? (size_t)(dot - p) : strlen(p);
        char seg[4]; seg_norm(seg, p, MIN(l, 4u));
        struct aml_node *c = ns_child(n, seg);
        if (!c) return NULL;
        n = ns_resolve_alias(c);
        p += l; if (*p == '.') p++;
    }
    return n;
}

struct aml_node *ns_lookup_search(struct aml_node *scope, const char *path) {
    if (path[0] == '\\' || path[0] == '^' || strchr(path, '.')) return ns_lookup(scope, path);
    char seg[4]; seg_norm(seg, path, MIN(strlen(path), 4u));
    for (struct aml_node *s = scope ? scope : aml_root; s; s = s->parent) { struct aml_node *c = ns_child(s, seg); if (c) return ns_resolve_alias(c); }
    return NULL;
}

void ns_path(struct aml_node *n, char *out, size_t size) {
    if (!n || !n->parent) { strlcpy(out, "\\", size); return; }
    struct aml_node *chain[64]; int depth = 0;
    for (struct aml_node *p = n; p && p->parent && depth < 64; p = p->parent) chain[depth++] = p;
    size_t pos = 0; out[0] = 0;
    for (int i = depth - 1; i >= 0 && pos + 6 < size; i--) {
        out[pos++] = i == depth - 1 ? '\\' : '.';
        memcpy(out + pos, chain[i]->name, 4); pos += 4;
    }
    out[pos] = 0;
}

static void walk(struct aml_node *n, int depth, bool (*fn)(struct aml_node *, int, void *), void *ctx) {
    for (struct aml_node *c = n->children; c; c = c->next) if (fn(c, depth, ctx)) walk(c, depth + 1, fn, ctx);
}
void ns_walk(struct aml_node *start, bool (*fn)(struct aml_node *n, int depth, void *ctx), void *ctx) {
    walk(start ? start : aml_root, 0, fn, ctx);
}

static void eisa_decode(uint32_t v, char out[8]) {
    uint32_t id = __builtin_bswap32(v);
    out[0] = ((id >> 26) & 0x1F) + 0x40; out[1] = ((id >> 21) & 0x1F) + 0x40; out[2] = ((id >> 16) & 0x1F) + 0x40;
    snprintf(out + 3, 5, "%04X", id & 0xFFFF);
}
static bool id_matches(struct aml_obj *o, const char *want) {
    if (o->type == AML_INTEGER) { char s[8]; eisa_decode((uint32_t)o->ival, s); return !strcmp(s, want); }
    if (o->type == AML_STRING) return !strcmp(o->str.s, want);
    return false;
}
bool ns_hid_matches(struct aml_node *dev, const char *eisa) {
    bool match = false;
    struct aml_obj *r;
    if (ns_child(dev, "_HID") && aml_eval_path(dev, "_HID", NULL, 0, &r) == 0) { match = id_matches(r, eisa); obj_put(r); }
    if (!match && ns_child(dev, "_CID") && aml_eval_path(dev, "_CID", NULL, 0, &r) == 0) {
        if (r->type == AML_PACKAGE) { for (size_t i = 0; i < r->pkg.n && !match; i++) match = id_matches(r->pkg.e[i], eisa); }
        else match = id_matches(r, eisa);
        obj_put(r);
    }
    return match;
}

/* ---- shell helpers ---- */
struct print_ctx { acpi_print_fn out; void *ctx; };

static void print_obj(struct print_ctx *pc, struct aml_obj *o, int indent, const char *prefix) {
    char line[160]; int n = snprintf(line, sizeof line, "%*s%s", indent * 2, "", prefix);
    switch (o->type) {
    case AML_INTEGER: snprintf(line + n, sizeof line - n, "0x%lx (%lu)", o->ival, o->ival); break;
    case AML_STRING: snprintf(line + n, sizeof line - n, "\"%.100s\"", o->str.s); break;
    case AML_BUFFER: {
        n += snprintf(line + n, sizeof line - n, "Buffer (%lu) {", o->buf.len);
        for (size_t i = 0; i < o->buf.len && n < (int)sizeof line - 8; i++) n += snprintf(line + n, sizeof line - n, " %02x", o->buf.b[i]);
        if (n < (int)sizeof line - 3) snprintf(line + n, sizeof line - n, o->buf.len * 3 + 20 > sizeof line ? " ...}" : " }");
        break;
    }
    case AML_PACKAGE:
        snprintf(line + n, sizeof line - n, "Package (%lu) {", o->pkg.n);
        pc->out(pc->ctx, line);
        for (size_t i = 0; i < o->pkg.n; i++) print_obj(pc, o->pkg.e[i], indent + 1, "");
        snprintf(line, sizeof line, "%*s}", indent * 2, "");
        break;
    case AML_REFERENCE:
        if (o->ref.kind == REF_NAMED) { char p[128]; ns_path(o->ref.node, p, sizeof p); snprintf(line + n, sizeof line - n, "Reference to %s", p); }
        else if (o->ref.kind == REF_INDEX) snprintf(line + n, sizeof line - n, "Reference to element %lu of a %s", o->ref.index, obj_type_name(o->ref.target->type));
        else snprintf(line + n, sizeof line - n, "Reference");
        break;
    case AML_NAMEREF: snprintf(line + n, sizeof line - n, "Name %s (unresolved)", o->nameref.path); break;
    default: snprintf(line + n, sizeof line - n, "[%s]", obj_type_name(o->type)); break;
    }
    pc->out(pc->ctx, line);
}

static bool tree_cb(struct aml_node *n, int depth, void *ctx) {
    struct print_ctx *pc = ctx;
    char line[128];
    struct aml_obj *o = n->obj;
    int type = o ? o->type : AML_UNINIT;
    int len = snprintf(line, sizeof line, "%*s%.4s  %s", depth * 2, "", n->name, n->alias ? "Alias" : obj_type_name(type));
    if (o && type == AML_INTEGER) snprintf(line + len, sizeof line - len, " = 0x%lx", o->ival);
    else if (o && type == AML_STRING) snprintf(line + len, sizeof line - len, " = \"%.40s\"", o->str.s);
    else if (o && type == AML_METHOD) snprintf(line + len, sizeof line - len, " (%d args)", o->method.flags & 7);
    else if (o && type == AML_REGION) snprintf(line + len, sizeof line - len, " space %d @ 0x%lx+0x%lx", o->region.space, o->region.base, o->region.len);
    pc->out(pc->ctx, line);
    return true;
}

int acpi_print_tree(const char *path, acpi_print_fn out, void *ctx) {
    if (!aml_root) return -ENOENT;
    aml_lock();
    struct aml_node *start = path && path[0] ? ns_lookup(aml_root, path) : aml_root;
    if (!start) { aml_unlock(); return -ENOENT; }
    struct print_ctx pc = { out, ctx };
    if (start != aml_root) tree_cb(start, 0, &pc);
    walk(start, start == aml_root ? 0 : 1, tree_cb, &pc);
    aml_unlock();
    return 0;
}

int acpi_print_eval(const char *path, int argc, const uint64_t *args, acpi_print_fn out, void *ctx) {
    if (!aml_root) return -ENOENT;
    struct aml_obj *argv[7]; argc = MIN(argc, 7);
    for (int i = 0; i < argc; i++) argv[i] = obj_int(args[i]);
    struct aml_obj *r = NULL;
    int rc = aml_eval_path(aml_root, path, argv, argc, &r);
    for (int i = 0; i < argc; i++) obj_put(argv[i]);
    if (rc < 0) return rc;
    struct print_ctx pc = { out, ctx };
    if (r) { print_obj(&pc, r, 0, ""); obj_put(r); }
    else out(ctx, "(no value)");
    return 0;
}
