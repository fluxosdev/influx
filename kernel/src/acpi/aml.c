/*
 * AML interpreter.
 *
 * Byte code is executed directly, without building a parse tree: table loads
 * walk the DSDT/SSDT term lists (creating namespace objects and running
 * module-level code), and method invocations walk the method body with a
 * frame of arguments and locals. Expressions are evaluated recursively and
 * produce reference-counted objects (see ns.c); every eval() result is owned
 * by the caller.
 *
 * Named objects created while a method runs are removed when it returns.
 * One recursive lock serialises the interpreter: AML from the SCI worker,
 * driver probes and the shell never runs concurrently, so AML mutexes and
 * events need no real implementation.
 */
#include "acpi_internal.h"
#include "mm.h"
#include "string.h"

#define MAX_DEPTH       128       /* nested expressions + terms + method calls, across re-entrant evaluations */
#define LOOP_TIMEOUT_MS 10000     /* a While that polls hardware forever gets abandoned */

enum { ST_OK, ST_BREAK, ST_CONTINUE, ST_RETURN, ST_ERROR };

struct frame {
    struct aml_obj *args[7], *locals[8];
    struct aml_obj *ret;
    struct aml_node *method;
    struct aml_node *dyn;         /* named objects created by this invocation */
    int gl_held;                  /* Acquire(\_GL) not yet released */
    struct frame *up;             /* the caller's frame */
};
struct state {
    const uint8_t *p, *end;       /* position, end of the method body / table */
    struct aml_node *scope;
    struct frame *f;              /* NULL while loading a table */
    int status;
};
struct name { bool root; int up, nseg; const uint8_t *segs; };

int aml_errors;
static const char *load_what;
static struct { const uint8_t *copy; size_t len; } *loaded;   /* loaded tables, for duplicate detection */
static int nloaded, loaded_cap;
static int depth;                 /* interpreter nesting; shared by re-entrant evaluations (under the lock) */
static struct aml_node *gl_node;  /* \_GL */

static struct aml_obj *eval(struct state *s);
static struct aml_obj *eval2(struct state *s);
static void exec_list(struct state *s, const uint8_t *end);
static void store(struct state *s, struct aml_obj *target, struct aml_obj *val);

/* ---- interpreter lock (recursive: notify handlers and region code may re-enter) ---- */
static struct mutex lock = MUTEX_INIT;
static struct thread *lock_owner;
static int lock_depth;
void aml_lock(void) {
    if (lock_owner == current_thread) { lock_depth++; return; }
    mutex_lock(&lock);
    lock_owner = current_thread; lock_depth = 1;
}
void aml_unlock(void) { if (--lock_depth == 0) { lock_owner = NULL; mutex_unlock(&lock); } }

static void fail(struct state *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(struct state *s, const char *fmt, ...) {
    if (s->status == ST_ERROR) return;
    s->status = ST_ERROR;
    aml_errors++;
    char msg[128]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    char where[96];
    if (s->f && s->f->method) ns_path(s->f->method, where, sizeof where);
    else strlcpy(where, load_what ? load_what : "AML", sizeof where);
    kprintf("acpi: error in %s: %s\n", where, msg);
}

/* ---- byte stream ---- */
static inline bool more(struct state *s) { return s->p < s->end; }
static uint8_t u8(struct state *s) { if (s->p >= s->end) { fail(s, "truncated byte code"); return 0; } return *s->p++; }
static uint64_t uN(struct state *s, int n) {
    if (s->p + n > s->end) { fail(s, "truncated byte code"); s->p = s->end; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)s->p[i] << (8 * i);
    s->p += n;
    return v;
}
static uint32_t pkglen(struct state *s) {
    uint8_t b0 = u8(s);
    int n = b0 >> 6;
    if (!n) return b0 & 0x3F;
    uint32_t len = b0 & 0x0F;
    for (int i = 0; i < n; i++) len |= (uint32_t)u8(s) << (4 + 8 * i);
    return len;
}
static const uint8_t *pkg_end(struct state *s) {
    const uint8_t *start = s->p;
    uint32_t len = pkglen(s);
    const uint8_t *e = start + len;
    if (e > s->end || e < s->p) { fail(s, "bad package length"); return s->p; }
    return e;
}

/* ---- names ---- */
static bool name_lead(uint8_t c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
static bool name_start(uint8_t c) { return name_lead(c) || c == '\\' || c == '^' || c == 0x2E || c == 0x2F; }

static void parse_name(struct state *s, struct name *n) {
    n->root = false; n->up = 0; n->nseg = 0; n->segs = s->p;
    if (more(s) && *s->p == '\\') { n->root = true; s->p++; }
    else while (more(s) && *s->p == '^') { n->up++; s->p++; }
    uint8_t c = u8(s);
    if (c == 0x00) n->nseg = 0;
    else if (c == 0x2E) n->nseg = 2;
    else if (c == 0x2F) n->nseg = u8(s);
    else { s->p--; n->nseg = 1; }
    if (s->p + n->nseg * 4 > s->end) { fail(s, "truncated name"); n->nseg = 0; return; }
    n->segs = s->p;
    s->p += n->nseg * 4;
}

static void name_str(const struct name *n, char *out, size_t size) {
    size_t pos = 0;
    if (n->root && pos + 1 < size) out[pos++] = '\\';
    for (int i = 0; i < n->up && pos + 1 < size; i++) out[pos++] = '^';
    for (int i = 0; i < n->nseg && pos + 6 < size; i++) {
        if (i) out[pos++] = '.';
        memcpy(out + pos, n->segs + i * 4, 4); pos += 4;
    }
    out[pos] = 0;
}

static struct aml_node *resolve(struct state *s, const struct name *n, bool search) {
    struct aml_node *base = n->root ? aml_root : s->scope;
    for (int i = 0; i < n->up; i++) if (base->parent) base = base->parent;
    if (n->nseg == 0) return base;
    if (n->nseg == 1 && !n->root && !n->up && search) {
        for (struct aml_node *sc = base; sc; sc = sc->parent) { struct aml_node *c = ns_child(sc, (const char *)n->segs); if (c) return ns_resolve_alias(c); }
        return NULL;
    }
    for (int i = 0; i < n->nseg; i++) {
        struct aml_node *c = ns_child(base, (const char *)n->segs + i * 4);
        if (!c) return NULL;
        base = ns_resolve_alias(c);
    }
    return base;
}

/* find or create the node a definition names; *existed tells which */
static struct aml_node *create_node(struct state *s, const struct name *n, bool *existed) {
    *existed = false;
    if (n->nseg == 0) { fail(s, "definition without a name"); return NULL; }
    struct name parent = *n; parent.nseg--;
    struct aml_node *pn = resolve(s, &parent, false);
    if (!pn) { char b[64]; name_str(n, b, sizeof b); fail(s, "parent scope of %s does not exist", b); return NULL; }
    const char *seg = (const char *)n->segs + (n->nseg - 1) * 4;
    struct aml_node *node = ns_child(pn, seg);
    if (node) { *existed = true; return node; }
    node = ns_add(pn, seg);
    if (s->f) { node->dyn_next = s->f->dyn; s->f->dyn = node; }
    return node;
}

/* ---- values ---- */
static bool node_has_value(struct aml_obj *o) {
    return o && (o->type <= AML_FIELD || o->type == AML_BUFFER_FIELD);
}
static struct aml_obj *node_value(struct aml_node *n) {
    n = ns_resolve_alias(n);
    struct aml_obj *o = n->obj;
    if (!o) return obj_new(AML_UNINIT);
    if (o->type == AML_FIELD) return field_read(o);
    if (o->type == AML_BUFFER_FIELD) return bfield_read(o);
    if (node_has_value(o)) return obj_get(o);
    return obj_ref_named(n);
}

static struct aml_obj *index_value(struct aml_obj *ref) {
    struct aml_obj *c = ref->ref.target; uint64_t i = ref->ref.index;
    if (c->type == AML_PACKAGE && i < c->pkg.n) return obj_get(c->pkg.e[i]);
    if (c->type == AML_BUFFER && i < c->buf.len) return obj_int(c->buf.b[i]);
    if (c->type == AML_STRING && i < c->str.len) return obj_int((uint8_t)c->str.s[i]);
    return obj_new(AML_UNINIT);
}

struct aml_obj *aml_value_of(struct aml_obj *o) {
    o = obj_get(o);
    for (int k = 0; k < 8; k++) {
        struct aml_obj *next;
        if (o->type == AML_REFERENCE) {
            switch (o->ref.kind) {
            case REF_NAMED: { struct aml_node *n = ns_resolve_alias(o->ref.node); if (!node_has_value(n->obj)) return o; next = node_value(n); break; }
            case REF_LOCAL: case REF_ARG: next = *o->ref.slot ? obj_get(*o->ref.slot) : obj_new(AML_UNINIT); break;
            case REF_INDEX: next = index_value(o); break;
            default: return o;
            }
        } else if (o->type == AML_NAMEREF) {
            struct aml_node *n = ns_lookup_search(o->nameref.scope, o->nameref.path);
            if (!n) return o;
            next = node_has_value(n->obj) ? node_value(n) : obj_ref_named(n);
            if (next->type == AML_REFERENCE) { obj_put(o); return next; }
        } else if (o->type == AML_FIELD) next = field_read(o);
        else if (o->type == AML_BUFFER_FIELD) next = bfield_read(o);
        else return o;
        obj_put(o);
        o = next;
    }
    return o;
}

static uint64_t to_int(struct state *s, struct aml_obj *o) {
    struct aml_obj *v = aml_value_of(o);
    uint64_t x = 0;
    if (!obj_to_int(v, &x)) fail(s, "%s used where an integer is required", obj_type_name(v->type));
    obj_put(v);
    return x;
}
static uint64_t eval_int(struct state *s) {
    struct aml_obj *o = eval(s);
    uint64_t x = s->status == ST_ERROR ? 0 : to_int(s, o);
    obj_put(o);
    return x;
}
static struct aml_obj *eval_value(struct state *s) {
    struct aml_obj *o = eval(s);
    struct aml_obj *v = aml_value_of(o);
    obj_put(o);
    return v;
}
static struct aml_obj *new_ref(int kind) { struct aml_obj *r = obj_new(AML_REFERENCE); r->ref.kind = kind; return r; }

/* ---- targets ---- */
static struct aml_obj *parse_supername(struct state *s, bool allow_missing) {
    if (!more(s)) { fail(s, "truncated byte code"); return new_ref(REF_NULL); }
    uint8_t op = *s->p;
    if (op >= 0x60 && op <= 0x6E) {
        s->p++;
        if (!s->f) { fail(s, "Local/Arg outside a method"); return new_ref(REF_NULL); }
        struct aml_obj *r = new_ref(op <= 0x67 ? REF_LOCAL : REF_ARG);
        r->ref.slot = op <= 0x67 ? &s->f->locals[op - 0x60] : &s->f->args[op - 0x68];
        return r;
    }
    if (op == 0x5B && s->p + 1 < s->end && s->p[1] == 0x31) { s->p += 2; return new_ref(REF_DEBUG); }
    if (name_start(op)) {
        struct name n; parse_name(s, &n);
        struct aml_node *node = resolve(s, &n, true);
        if (!node) {
            if (allow_missing) return NULL;
            char b[64]; name_str(&n, b, sizeof b); fail(s, "%s does not exist", b);
            return new_ref(REF_NULL);
        }
        return obj_ref_named(node);
    }
    if (op == 0x83) {   /* DerefOf as a target: the object the reference points at */
        s->p++;
        struct aml_obj *o = eval2(s);
        if (o->type == AML_REFERENCE) return o;
        if (o->type == AML_STRING) { struct aml_node *n = ns_lookup_search(s->scope, o->str.s); obj_put(o); if (n) return obj_ref_named(n); }
        else obj_put(o);
        fail(s, "DerefOf target is not a reference");
        return new_ref(REF_NULL);
    }
    struct aml_obj *o = eval2(s);   /* Index(), RefOf() */
    if (o->type == AML_REFERENCE) return o;
    obj_put(o);
    fail(s, "invalid target");
    return new_ref(REF_NULL);
}
static struct aml_obj *parse_target(struct state *s) {
    if (more(s) && *s->p == 0x00) { s->p++; return new_ref(REF_NULL); }
    return parse_supername(s, false);
}

static void debug_print(struct aml_obj *v) {
    struct aml_obj *x = aml_value_of(v);
    if (x->type == AML_INTEGER) kprintf("acpi: [Debug] 0x%lx\n", x->ival);
    else if (x->type == AML_STRING) kprintf("acpi: [Debug] \"%s\"\n", x->str.s);
    else if (x->type == AML_BUFFER) kprintf("acpi: [Debug] Buffer (%lu bytes)\n", x->buf.len);
    else if (x->type == AML_PACKAGE) kprintf("acpi: [Debug] Package (%lu elements)\n", x->pkg.n);
    else kprintf("acpi: [Debug] %s\n", obj_type_name(x->type));
    obj_put(x);
}

static void store_named(struct state *s, struct aml_node *node, struct aml_obj *val, bool copy_object) {
    node = ns_resolve_alias(node);
    struct aml_obj *cur = node->obj;
    struct aml_obj *v = aml_value_of(val);
    /* CopyObject replaces the object, except that a field unit or buffer field
     * keeps its type and is written through (as ACPICA) */
    bool field = cur && (cur->type == AML_FIELD || cur->type == AML_BUFFER_FIELD);
    if ((copy_object && !field) || !cur) { obj_put(cur); node->obj = obj_copy(v); obj_put(v); return; }
    switch (cur->type) {
    case AML_FIELD: field_write(cur, v); break;
    case AML_BUFFER_FIELD: bfield_write(cur, v); break;
    case AML_INTEGER: {
        uint64_t x;
        if (!obj_to_int(v, &x)) { fail(s, "cannot store a %s into an Integer", obj_type_name(v->type)); break; }
        obj_put(cur); node->obj = obj_int(aml_trunc(x));
        break;
    }
    case AML_BUFFER: {   /* in place, so buffer fields created on it keep working */
        struct aml_obj *b = obj_to_buffer(v);
        if (!b) { fail(s, "cannot store a %s into a Buffer", obj_type_name(v->type)); break; }
        if (b != cur) {   /* storing a buffer into itself: already there (zeroing first would lose it) */
            memset(cur->buf.b, 0, cur->buf.len);
            memcpy(cur->buf.b, b->buf.b, MIN(b->buf.len, cur->buf.len));
        }
        obj_put(b);
        break;
    }
    case AML_STRING: {
        struct aml_obj *str = obj_to_string(v);
        if (!str) { fail(s, "cannot store a %s into a String", obj_type_name(v->type)); break; }
        obj_put(cur); node->obj = obj_str(str->str.s, str->str.len); obj_put(str);
        break;
    }
    case AML_UNINIT: case AML_PACKAGE: case AML_REFERENCE:
        obj_put(cur); node->obj = obj_copy(v);
        break;
    default: {
        char p[96]; ns_path(node, p, sizeof p);
        fail(s, "cannot store into %s (%s)", p, obj_type_name(cur->type));
    }
    }
    obj_put(v);
}

/* References to a frame's Locals/Args or to the named objects the invocation
 * created die with the frame. A value leaving the frame takes the referenced
 * value instead: on Return (references into this frame), and on a store
 * anywhere but this frame's own slots (references into any running frame). */
static bool in_frame(struct frame *f, struct aml_obj *r) {
    if (r->ref.kind == REF_LOCAL || r->ref.kind == REF_ARG) return (void *)r->ref.slot >= (void *)f && (void *)r->ref.slot < (void *)(f + 1);
    if (r->ref.kind != REF_NAMED) return false;
    struct aml_node *n = ns_resolve_alias(r->ref.node);
    for (struct aml_node *d = f->dyn; d; d = d->dyn_next) if (d == n) return true;
    return false;
}
static bool frame_ref(struct frame *f, bool up, struct aml_obj *o, int lvl) {
    if (o->type == AML_PACKAGE) {
        for (size_t i = 0; lvl < 8 && i < o->pkg.n; i++) if (frame_ref(f, up, o->pkg.e[i], lvl + 1)) return true;
        return false;
    }
    if (o->type != AML_REFERENCE) return false;
    if (o->ref.kind == REF_INDEX) {   /* the package element it designates may name a method-local object */
        struct aml_obj *c = o->ref.target;
        return lvl < 8 && c->type == AML_PACKAGE && o->ref.index < c->pkg.n && frame_ref(f, up, c->pkg.e[o->ref.index], lvl + 1);
    }
    for (; f; f = up ? f->up : NULL) if (in_frame(f, o)) return true;
    return false;
}
static struct aml_obj *detach(struct state *s, struct aml_obj *o, bool up, int lvl) {
    if (!s->f || lvl >= 8 || !frame_ref(s->f, up, o, 0)) return obj_get(o);
    if (o->type == AML_PACKAGE) {
        struct aml_obj *p = obj_pkg(o->pkg.n);
        for (size_t i = 0; i < o->pkg.n; i++) { obj_put(p->pkg.e[i]); p->pkg.e[i] = detach(s, o->pkg.e[i], up, lvl + 1); }
        return p;
    }
    struct aml_obj *v = aml_value_of(o);
    if (v->type == AML_REFERENCE && frame_ref(s->f, up, v, 0)) {
        obj_put(v);
        fail(s, "a reference to a method-local object outlives the method");
        return obj_new(AML_UNINIT);
    }
    struct aml_obj *r = detach(s, v, up, lvl + 1);
    obj_put(v);
    return r;
}

static void store_raw(struct state *s, struct aml_obj *t, struct aml_obj *val);
static void store(struct state *s, struct aml_obj *t, struct aml_obj *val) {
    if (s->status == ST_ERROR) return;
    bool own_slot = s->f && (t->ref.kind == REF_LOCAL || t->ref.kind == REF_ARG) && in_frame(s->f, t);
    if ((own_slot && !(t->ref.kind == REF_ARG && *t->ref.slot && (*t->ref.slot)->type == AML_REFERENCE)) || t->ref.kind == REF_NULL || t->ref.kind == REF_DEBUG) {
        store_raw(s, t, val);
        return;
    }
    struct aml_obj *d = detach(s, val, true, 0);
    store_raw(s, t, d);
    obj_put(d);
}
static void store_raw(struct state *s, struct aml_obj *t, struct aml_obj *val) {
    if (s->status == ST_ERROR) return;
    switch (t->ref.kind) {
    case REF_NULL: return;
    case REF_DEBUG: debug_print(val); return;
    case REF_ARG:
        if (*t->ref.slot && (*t->ref.slot)->type == AML_REFERENCE) {   /* through the reference the Arg holds */
            static int hops;   /* bounded like aml_value_of: Arg0 = RefOf(Arg0) makes a cycle */
            if (hops >= 8) { fail(s, "reference chain too long (a reference cycle?)"); return; }
            hops++; store(s, *t->ref.slot, val); hops--;
            return;
        }
        /* fallthrough */
    case REF_LOCAL: { struct aml_obj *c = obj_copy(val); obj_put(*t->ref.slot); *t->ref.slot = c; return; }
    case REF_NAMED: store_named(s, t->ref.node, val, false); return;
    case REF_INDEX: {
        struct aml_obj *c = t->ref.target; uint64_t i = t->ref.index;
        if (c->type == AML_PACKAGE && i < c->pkg.n) { struct aml_obj *n = obj_copy(val); obj_put(c->pkg.e[i]); c->pkg.e[i] = n; }
        else if ((c->type == AML_BUFFER && i < c->buf.len) || (c->type == AML_STRING && i < c->str.len)) {
            uint8_t b = (uint8_t)to_int(s, val);
            if (c->type == AML_BUFFER) c->buf.b[i] = b; else c->str.s[i] = b;
        } else fail(s, "index %lu out of range", i);
        return;
    }
    }
}

static struct aml_obj *put_result(struct state *s, struct aml_obj *target, struct aml_obj *result) {
    store(s, target, result);
    obj_put(target);
    return result;
}

/* ---- method invocation ---- */
static struct aml_obj *invoke(struct state *caller, struct aml_node *mnode, struct aml_obj **args, int argc) {
    struct aml_obj *m = mnode->obj;
    if (m->method.native) {
        struct aml_obj *vals[7];
        for (int i = 0; i < argc; i++) vals[i] = aml_value_of(args[i]);
        struct aml_obj *r = m->method.native(vals, argc);
        for (int i = 0; i < argc; i++) obj_put(vals[i]);
        return r ? r : obj_int(0);
    }
    if (depth >= MAX_DEPTH) { fail(caller, "method calls nested too deeply"); return obj_int(0); }
    depth++;
    struct frame *f = kzalloc(sizeof *f);
    f->method = mnode; f->up = caller->f;
    for (int i = 0; i < argc && i < 7; i++) f->args[i] = obj_get(args[i]);
    struct state st = { .p = m->method.code, .end = m->method.code + m->method.len, .scope = mnode, .f = f };
    exec_list(&st, st.end);
    struct aml_obj *r = f->ret ? f->ret : obj_int(0);
    if (st.status == ST_ERROR) caller->status = ST_ERROR;   /* already reported */
    while (f->gl_held > 0) { f->gl_held--; acpi_gl_release(); }   /* as ACPICA: a method's mutexes go when it ends */
    while (f->dyn) { struct aml_node *n = f->dyn; f->dyn = n->dyn_next; ns_remove(n); }
    for (int i = 0; i < 7; i++) obj_put(f->args[i]);
    for (int i = 0; i < 8; i++) obj_put(f->locals[i]);
    kfree(f);
    depth--;
    return r;
}

static struct aml_obj *eval_name(struct state *s) {
    struct name n; parse_name(s, &n);
    struct aml_node *node = resolve(s, &n, true);
    if (!node) { char b[64]; name_str(&n, b, sizeof b); fail(s, "%s does not exist", b); return NULL; }
    if (node->obj && node->obj->type == AML_METHOD) {
        int argc = node->obj->method.flags & 7;
        struct aml_obj *args[7];
        for (int i = 0; i < argc; i++) args[i] = eval(s);
        struct aml_obj *r = s->status == ST_ERROR ? NULL : invoke(s, node, args, argc);
        for (int i = 0; i < argc; i++) obj_put(args[i]);
        return r;
    }
    return node_value(node);
}

/* ---- data objects ---- */
static struct aml_obj *parse_string(struct state *s) {
    const uint8_t *start = s->p;
    while (more(s) && *s->p) s->p++;
    size_t len = s->p - start;
    u8(s);   /* NUL */
    return obj_str((const char *)start, len);
}

static struct aml_obj *parse_buffer(struct state *s) {
    const uint8_t *end = pkg_end(s);
    uint64_t size = eval_int(s);
    if (s->status == ST_ERROR) return NULL;
    size_t init = end > s->p ? (size_t)(end - s->p) : 0;
    if (size > 1024 * 1024) { fail(s, "buffer of %lu bytes", size); return NULL; }
    struct aml_obj *b = obj_buf(NULL, MAX(size, init));
    memcpy(b->buf.b, s->p, init);
    s->p = end;
    return b;
}

static struct aml_obj *parse_package(struct state *s, bool var) {
    const uint8_t *end = pkg_end(s);
    uint64_t n = var ? eval_int(s) : u8(s);
    if (s->status == ST_ERROR) return NULL;
    if (n > 65536) { fail(s, "package of %lu elements", n); return NULL; }
    struct aml_obj *pkg = obj_pkg(n);
    for (size_t i = 0; s->p < end && s->status == ST_OK; i++) {
        struct aml_obj *e;
        if (name_start(*s->p)) {   /* names are references, not evaluated: a later Store to a named Integer shows through, as in ACPICA */
            struct name nm; parse_name(s, &nm);
            struct aml_node *node = resolve(s, &nm, true);
            if (node) e = obj_ref_named(node);
            else { char b[64]; name_str(&nm, b, sizeof b); e = obj_new(AML_NAMEREF); e->nameref.path = kstrdup(b); e->nameref.scope = s->scope; }
        } else e = eval2(s);
        if (i < n) { obj_put(pkg->pkg.e[i]); pkg->pkg.e[i] = e; } else obj_put(e);
    }
    s->p = end;
    return pkg;
}

/* ---- operators ---- */
static struct aml_obj *op_store(struct state *s, bool copy_object) {
    struct aml_obj *v = eval2(s);
    struct aml_obj *t = parse_supername(s, false);
    if (s->status == ST_OK) {
        if (copy_object && t->ref.kind == REF_NAMED) { struct aml_obj *d = detach(s, v, true, 0); store_named(s, t->ref.node, d, true); obj_put(d); }
        else store(s, t, v);
    }
    obj_put(t);
    return v;
}

static struct aml_obj *op_binary(struct state *s, uint8_t op) {
    uint64_t a = eval_int(s), b = eval_int(s), r = 0;
    struct aml_obj *t = parse_target(s);
    switch (op) {
    case 0x72: r = a + b; break;
    case 0x74: r = a - b; break;
    case 0x77: r = a * b; break;
    case 0x79: r = b >= 64 ? 0 : a << b; break;
    case 0x7A: r = b >= 64 ? 0 : a >> b; break;
    case 0x7B: r = a & b; break;
    case 0x7C: r = ~(a & b); break;
    case 0x7D: r = a | b; break;
    case 0x7E: r = ~(a | b); break;
    case 0x7F: r = a ^ b; break;
    case 0x85: if (!b) fail(s, "Mod by zero"); else r = a % b; break;
    }
    return put_result(s, t, obj_int(aml_trunc(r)));
}

static struct aml_obj *op_unary(struct state *s, uint8_t op) {
    uint64_t a = eval_int(s), r;
    struct aml_obj *t = parse_target(s);
    if (op == 0x80) r = ~a;
    else if (op == 0x81) r = a ? 64 - __builtin_clzll(a) : 0;
    else r = a ? __builtin_ctzll(a) + 1 : 0;
    return put_result(s, t, obj_int(aml_trunc(r)));
}

static struct aml_obj *op_divide(struct state *s) {
    uint64_t a = eval_int(s), b = eval_int(s);
    struct aml_obj *rem = parse_target(s), *quo = parse_target(s);
    if (!b && s->status == ST_OK) fail(s, "Divide by zero");
    uint64_t q = b ? a / b : 0, r = b ? a % b : 0;
    obj_put(put_result(s, rem, obj_int(r)));
    return put_result(s, quo, obj_int(q));
}

static struct aml_obj *op_incdec(struct state *s, bool inc) {
    struct aml_obj *t = parse_supername(s, false);
    uint64_t v = s->status == ST_OK ? to_int(s, t) : 0;
    return put_result(s, t, obj_int(aml_trunc(inc ? v + 1 : v - 1)));
}

static int compare(struct state *s, struct aml_obj *a, struct aml_obj *b) {
    if (a->type == AML_INTEGER) { uint64_t y = to_int(s, b); return a->ival < y ? -1 : a->ival > y; }
    struct aml_obj *bb = a->type == AML_STRING ? obj_to_string(b) : a->type == AML_BUFFER ? obj_to_buffer(b) : NULL;
    if (!bb) { fail(s, "cannot compare a %s", obj_type_name(a->type)); return 0; }
    const uint8_t *x = a->type == AML_STRING ? (const uint8_t *)a->str.s : a->buf.b;
    const uint8_t *y = a->type == AML_STRING ? (const uint8_t *)bb->str.s : bb->buf.b;
    size_t lx = a->type == AML_STRING ? a->str.len : a->buf.len, ly = a->type == AML_STRING ? bb->str.len : bb->buf.len;
    int c = memcmp(x, y, MIN(lx, ly));
    if (!c) c = lx < ly ? -1 : lx > ly;
    obj_put(bb);
    return c;
}
static struct aml_obj *op_compare(struct state *s, uint8_t op) {
    struct aml_obj *a = eval_value(s), *b = eval_value(s);
    int c = s->status == ST_OK ? compare(s, a, b) : 0;
    obj_put(a); obj_put(b);
    bool r = op == 0x93 ? c == 0 : op == 0x94 ? c > 0 : c < 0;
    return obj_int(r ? aml_ones() : 0);
}

static struct aml_obj *op_concat(struct state *s) {
    struct aml_obj *a = eval_value(s), *b = eval_value(s);
    struct aml_obj *t = parse_target(s), *r = NULL;
    if (s->status == ST_OK) {
        if (a->type == AML_INTEGER) {
            uint64_t y = to_int(s, b); size_t w = aml_int64 ? 8 : 4;
            r = obj_buf(NULL, w * 2); memcpy(r->buf.b, &a->ival, w); memcpy(r->buf.b + w, &y, w);
        } else if (a->type == AML_STRING) {
            struct aml_obj *bs = obj_to_string(b);
            if (!bs) fail(s, "cannot concatenate a %s to a String", obj_type_name(b->type));
            else {
                size_t len = a->str.len + bs->str.len; char *tmp = kmalloc(len + 1);
                memcpy(tmp, a->str.s, a->str.len); memcpy(tmp + a->str.len, bs->str.s, bs->str.len);
                r = obj_str(tmp, len);
                kfree(tmp); obj_put(bs);
            }
        } else if (a->type == AML_BUFFER) {
            struct aml_obj *bb = obj_to_buffer(b);
            if (!bb) fail(s, "cannot concatenate a %s to a Buffer", obj_type_name(b->type));
            else { r = obj_buf(NULL, a->buf.len + bb->buf.len); memcpy(r->buf.b, a->buf.b, a->buf.len); memcpy(r->buf.b + a->buf.len, bb->buf.b, bb->buf.len); obj_put(bb); }
        } else fail(s, "cannot concatenate a %s", obj_type_name(a->type));
    }
    obj_put(a); obj_put(b);
    return put_result(s, t, r ? r : obj_int(0));
}

static struct aml_obj *op_concat_res(struct state *s) {
    struct aml_obj *a = eval_value(s), *b = eval_value(s);
    struct aml_obj *t = parse_target(s), *r = NULL;
    if (s->status == ST_OK && a->type == AML_BUFFER && b->type == AML_BUFFER) {
        size_t la = a->buf.len >= 2 && a->buf.b[a->buf.len - 2] == 0x79 ? a->buf.len - 2 : a->buf.len;
        size_t lb = b->buf.len >= 2 && b->buf.b[b->buf.len - 2] == 0x79 ? b->buf.len - 2 : b->buf.len;
        r = obj_buf(NULL, la + lb + 2);
        memcpy(r->buf.b, a->buf.b, la); memcpy(r->buf.b + la, b->buf.b, lb);
        r->buf.b[la + lb] = 0x79;
    } else if (s->status == ST_OK) fail(s, "ConcatenateResTemplate needs two buffers");
    obj_put(a); obj_put(b);
    return put_result(s, t, r ? r : obj_int(0));
}

static struct aml_obj *op_sizeof(struct state *s) {
    struct aml_obj *t = parse_supername(s, false);
    struct aml_obj *v = aml_value_of(t);
    uint64_t n = 0;
    if (v->type == AML_STRING) n = v->str.len;
    else if (v->type == AML_BUFFER) n = v->buf.len;
    else if (v->type == AML_PACKAGE) n = v->pkg.n;
    else if (s->status == ST_OK) fail(s, "SizeOf a %s", obj_type_name(v->type));
    obj_put(v); obj_put(t);
    return obj_int(n);
}

static struct aml_obj *op_index(struct state *s) {
    struct aml_obj *src = eval(s);
    uint64_t i = eval_int(s);
    struct aml_obj *t = parse_target(s);
    struct aml_obj *c = aml_value_of(src);
    obj_put(src);
    size_t size = c->type == AML_PACKAGE ? c->pkg.n : c->type == AML_BUFFER ? c->buf.len : c->type == AML_STRING ? c->str.len : 0;
    if (s->status == ST_OK && c->type != AML_PACKAGE && c->type != AML_BUFFER && c->type != AML_STRING) fail(s, "Index into a %s", obj_type_name(c->type));
    else if (s->status == ST_OK && i >= size) fail(s, "Index %lu beyond the end (%lu)", i, size);
    struct aml_obj *r = new_ref(REF_INDEX);
    r->ref.target = c; r->ref.index = i;
    return put_result(s, t, r);
}

static bool match_op(uint8_t op, uint64_t a, uint64_t b) {
    switch (op) { case 0: return true; case 1: return a == b; case 2: return a <= b; case 3: return a < b; case 4: return a >= b; case 5: return a > b; }
    return false;
}
static struct aml_obj *op_match(struct state *s) {
    struct aml_obj *pkg = eval_value(s);
    uint8_t op1 = u8(s); uint64_t v1 = eval_int(s);
    uint8_t op2 = u8(s); uint64_t v2 = eval_int(s);
    uint64_t start = eval_int(s), r = aml_ones();
    if (s->status == ST_OK && pkg->type != AML_PACKAGE) fail(s, "Match on a %s", obj_type_name(pkg->type));
    else for (uint64_t i = start; s->status == ST_OK && i < pkg->pkg.n; i++) {
        struct aml_obj *e = aml_value_of(pkg->pkg.e[i]); uint64_t x;
        bool ok = obj_to_int(e, &x) && match_op(op1, x, v1) && match_op(op2, x, v2);
        obj_put(e);
        if (ok) { r = i; break; }
    }
    obj_put(pkg);
    return obj_int(r);
}

static struct aml_obj *op_objtype(struct state *s) {
    struct aml_obj *t = parse_supername(s, false);
    int type = AML_UNINIT;
    if (t->ref.kind == REF_DEBUG) type = AML_DEBUG;
    else if (t->ref.kind == REF_NAMED) { struct aml_node *n = ns_resolve_alias(t->ref.node); type = n->obj ? n->obj->type : AML_UNINIT; }
    else if (t->ref.kind != REF_NULL) {
        struct aml_obj *v = t->ref.kind == REF_INDEX ? index_value(t) : obj_get(*t->ref.slot);
        if (v && v->type == AML_REFERENCE && v->ref.kind == REF_NAMED) { struct aml_node *n = ns_resolve_alias(v->ref.node); type = n->obj ? n->obj->type : AML_UNINIT; }
        else if (v) type = v->type;
        obj_put(v);
    }
    obj_put(t);
    return obj_int(type >= AML_REFERENCE ? AML_UNINIT : type);
}

static struct aml_obj *op_derefof(struct state *s) {
    struct aml_obj *o = eval(s), *r = NULL;
    if (s->status != ST_OK) { obj_put(o); return NULL; }
    if (o->type == AML_REFERENCE) r = aml_value_of(o);   /* a non-data object stays a reference */
    else if (o->type == AML_STRING) {
        struct aml_node *n = ns_lookup_search(s->scope, o->str.s);
        if (n) r = node_value(n); else fail(s, "DerefOf(\"%s\"): no such object", o->str.s);
    } else fail(s, "DerefOf a %s", obj_type_name(o->type));
    obj_put(o);
    return r;
}

static struct aml_obj *op_condrefof(struct state *s) {
    struct aml_obj *src = parse_supername(s, true);
    struct aml_obj *t = parse_target(s);
    bool exists = src && src->ref.kind != REF_NULL;
    if (exists && (src->ref.kind == REF_LOCAL || src->ref.kind == REF_ARG)) exists = *src->ref.slot != NULL;
    if (exists) store(s, t, src);
    obj_put(src); obj_put(t);
    return obj_int(exists ? aml_ones() : 0);
}

static uint64_t parse_explicit_int(const char *str) {
    while (*str == ' ' || *str == '\t') str++;
    uint64_t v = (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) ? strtoul(str + 2, NULL, 16) : strtoul(str, NULL, 10);
    return aml_trunc(v);
}

static struct aml_obj *op_convert(struct state *s, uint8_t op) {
    struct aml_obj *v = eval_value(s), *r = NULL;
    uint64_t maxlen = op == 0x9C ? eval_int(s) : 0;
    struct aml_obj *t = parse_target(s);
    if (s->status != ST_OK) { obj_put(v); return put_result(s, t, obj_int(0)); }
    char tmp[24];
    switch (op) {
    case 0x96:   /* ToBuffer */
        r = v->type == AML_BUFFER ? obj_copy(v) : obj_to_buffer(v);
        break;
    case 0x97: case 0x98:   /* ToDecimalString, ToHexString */
        if (v->type == AML_INTEGER) { int n = snprintf(tmp, sizeof tmp, op == 0x97 ? "%lu" : "%lX", v->ival); r = obj_str(tmp, n); }
        else if (v->type == AML_STRING) r = obj_copy(v);
        else if (v->type == AML_BUFFER) {
            size_t cap = v->buf.len * 5 + 1, len = 0; char *out = kmalloc(cap);
            for (size_t i = 0; i < v->buf.len; i++) len += snprintf(out + len, cap - len, op == 0x97 ? "%s%u" : "%s0x%02X", i ? "," : "", v->buf.b[i]);
            r = obj_str(out, len); kfree(out);
        }
        break;
    case 0x99:   /* ToInteger */
        if (v->type == AML_STRING) r = obj_int(parse_explicit_int(v->str.s));
        else { uint64_t x; if (obj_to_int(v, &x)) r = obj_int(x); }
        break;
    case 0x9C:   /* ToString */
        if (v->type == AML_BUFFER) { size_t n = 0; while (n < v->buf.len && n < maxlen && v->buf.b[n]) n++; r = obj_str((const char *)v->buf.b, n); }
        else if (v->type == AML_STRING) r = obj_copy(v);
        break;
    }
    if (!r) { fail(s, "cannot convert a %s", obj_type_name(v->type)); r = obj_int(0); }
    obj_put(v);
    return put_result(s, t, r);
}

static struct aml_obj *op_mid(struct state *s) {
    struct aml_obj *v = eval_value(s);
    uint64_t idx = eval_int(s), len = eval_int(s);
    struct aml_obj *t = parse_target(s), *r = NULL;
    if (v->type == AML_STRING || v->type == AML_BUFFER) {
        size_t total = v->type == AML_STRING ? v->str.len : v->buf.len;
        size_t start = MIN(idx, total), n = MIN(len, total - start);
        r = v->type == AML_STRING ? obj_str(v->str.s + start, n) : obj_buf(v->buf.b + start, n);
    } else if (s->status == ST_OK) fail(s, "Mid on a %s", obj_type_name(v->type));
    obj_put(v);
    return put_result(s, t, r ? r : obj_int(0));
}

static struct aml_obj *op_bcd(struct state *s, bool to_bcd) {
    uint64_t v = eval_int(s), r = 0;
    struct aml_obj *t = parse_target(s);
    if (to_bcd) { for (int sh = 0; v && sh < 64; sh += 4, v /= 10) r |= (v % 10) << sh; }
    else { uint64_t mul = 1; for (; v; v >>= 4, mul *= 10) r += (v & 0xF) * mul; }
    return put_result(s, t, obj_int(aml_trunc(r)));
}

/* Load(): a table image in an operation region or a buffer */
static struct aml_obj *op_load(struct state *s) {
    struct name n; parse_name(s, &n);
    struct aml_node *node = resolve(s, &n, true);
    struct aml_obj *t = parse_target(s);
    bool ok = false;
    if (node && node->obj && node->obj->type == AML_REGION && node->obj->region.space == SPACE_MEMORY && node->obj->region.len >= 36) {
        uint64_t base = node->obj->region.base;
        uint32_t *hdr = acpi_map_phys(base, 36);
        uint32_t len = hdr[1];
        if (len >= 36 && len <= node->obj->region.len && len < 4 * 1024 * 1024) ok = aml_load_table(acpi_map_phys(base, len), len, "SSDT (Load)") == 0;
    } else if (node) {
        struct aml_obj *v = node_value(node);
        if (v->type == AML_BUFFER && v->buf.len >= 36 && *(uint32_t *)(v->buf.b + 4) <= v->buf.len) ok = aml_load_table(v->buf.b, *(uint32_t *)(v->buf.b + 4), "SSDT (Load)") == 0;
        obj_put(v);
    }
    if (!ok) kprintf("acpi: Load() failed\n");
    return put_result(s, t, obj_int(ok ? aml_ones() : 0));
}

static struct aml_obj *op_loadtable(struct state *s) {
    struct aml_obj *a[6];
    for (int i = 0; i < 6; i++) a[i] = eval_value(s);
    bool ok = false;
    if (s->status == ST_OK && a[0]->type == AML_STRING && a[0]->str.len >= 4) {   /* memcmp reads 4 bytes */
        char sig[5];
        for (int i = 0; acpi_table_at(i, sig); i++) {
            const uint8_t *tbl = acpi_table_at(i, sig);
            if (memcmp(sig, a[0]->str.s, 4)) continue;
            if (a[1]->type == AML_STRING && a[1]->str.len && strncmp((const char *)tbl + 10, a[1]->str.s, MIN(a[1]->str.len, 6u))) continue;
            if (a[2]->type == AML_STRING && a[2]->str.len && strncmp((const char *)tbl + 16, a[2]->str.s, MIN(a[2]->str.len, 8u))) continue;
            uint32_t len = *(const uint32_t *)(tbl + 4);
            ok = aml_load_table(acpi_map_phys(V2P(tbl), len), len, "SSDT (LoadTable)") == 0;
            break;
        }
        if (ok && a[4]->type == AML_STRING && a[4]->str.len) {
            struct aml_node *p = ns_lookup_search(s->scope, a[4]->str.s);
            if (p) store_named(s, p, a[5], false);
        }
    }
    for (int i = 0; i < 6; i++) obj_put(a[i]);
    return obj_int(ok ? aml_ones() : 0);
}

#define GL_MAX_WAIT_MS 2000
static bool is_gl(struct aml_obj *m) { return m->type == AML_REFERENCE && m->ref.kind == REF_NAMED && gl_node && ns_resolve_alias(m->ref.node) == gl_node; }

static struct aml_obj *eval_ext(struct state *s) {
    uint8_t op = u8(s);
    switch (op) {
    case 0x30: return obj_int(2);                          /* Revision */
    case 0x33: return obj_int(aml_trunc(acpi_timer_100ns()));   /* Timer */
    case 0x12: return op_condrefof(s);
    case 0x23: {   /* Acquire: one interpreter thread, so only \_GL (shared with the firmware) is real */
        struct aml_obj *m = parse_supername(s, false);
        uint32_t timeout = uN(s, 2);
        bool ok = true;
        if (s->status == ST_OK && is_gl(m)) {
            ok = acpi_gl_acquire(timeout == 0xFFFF ? GL_MAX_WAIT_MS : MIN(timeout, (uint32_t)GL_MAX_WAIT_MS));
            if (ok && s->f) s->f->gl_held++;
        }
        obj_put(m);
        return obj_int(ok ? 0 : aml_ones());
    }
    case 0x25: { struct aml_obj *e = parse_supername(s, false); eval_int(s); obj_put(e); return obj_int(0); }   /* Wait */
    case 0x28: case 0x29: return op_bcd(s, op == 0x29);
    case 0x1F: return op_loadtable(s);
    case 0x20: return op_load(s);
    }
    s->p -= 2;
    fail(s, "unexpected opcode 0x5b%02x in an expression", op);
    return NULL;
}

static struct aml_obj *eval_op(struct state *s) {
    if (!more(s)) { fail(s, "truncated byte code"); return NULL; }
    uint8_t op = *s->p;
    if (name_start(op)) return eval_name(s);
    s->p++;
    if (op >= 0x60 && op <= 0x67) {
        if (!s->f) { fail(s, "Local outside a method"); return NULL; }
        struct aml_obj *o = s->f->locals[op - 0x60];
        if (!o) { fail(s, "Local%d is uninitialized", op - 0x60); return NULL; }
        return obj_get(o);
    }
    if (op >= 0x68 && op <= 0x6E) {
        if (!s->f) { fail(s, "Arg outside a method"); return NULL; }
        struct aml_obj *o = s->f->args[op - 0x68];
        return o ? obj_get(o) : obj_new(AML_UNINIT);
    }
    switch (op) {
    case 0x00: return obj_int(0);
    case 0x01: return obj_int(1);
    case 0xFF: return obj_int(aml_ones());
    case 0x0A: return obj_int(u8(s));
    case 0x0B: return obj_int(uN(s, 2));
    case 0x0C: return obj_int(uN(s, 4));
    case 0x0E: return obj_int(aml_trunc(uN(s, 8)));
    case 0x0D: return parse_string(s);
    case 0x11: return parse_buffer(s);
    case 0x12: return parse_package(s, false);
    case 0x13: return parse_package(s, true);
    case 0x70: return op_store(s, false);
    case 0x9D: return op_store(s, true);
    case 0x71: return parse_supername(s, false);           /* RefOf */
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85: return op_binary(s, op);
    case 0x75: case 0x76: return op_incdec(s, op == 0x75);
    case 0x78: return op_divide(s);
    case 0x80: case 0x81: case 0x82: return op_unary(s, op);
    case 0x83: return op_derefof(s);
    case 0x73: return op_concat(s);
    case 0x84: return op_concat_res(s);
    case 0x87: return op_sizeof(s);
    case 0x88: return op_index(s);
    case 0x89: return op_match(s);
    case 0x8E: return op_objtype(s);
    case 0x90: case 0x91: { uint64_t a = eval_int(s), b = eval_int(s); bool r = op == 0x90 ? (a && b) : (a || b); return obj_int(r ? aml_ones() : 0); }
    case 0x92: return obj_int(eval_int(s) ? 0 : aml_ones());   /* LNot; LNotEqual etc. are LNot(LEqual(..)) */
    case 0x93: case 0x94: case 0x95: return op_compare(s, op);
    case 0x96: case 0x97: case 0x98: case 0x99: case 0x9C: return op_convert(s, op);
    case 0x9E: return op_mid(s);
    case 0x5B: return eval_ext(s);
    }
    s->p--;
    fail(s, "unexpected opcode 0x%02x in an expression", op);
    return NULL;
}

static struct aml_obj *eval(struct state *s) {
    if (s->status != ST_OK) return obj_int(0);
    if (depth >= MAX_DEPTH) { fail(s, "expression nested too deeply"); return obj_int(0); }
    depth++;
    struct aml_obj *r = eval_op(s);
    depth--;
    return r ? r : obj_int(0);
}
/* eval() from a caller with a large frame of its own: counts as two levels, so
 * MAX_DEPTH bounds the stack (about 200 bytes a level) whatever the nesting */
static struct aml_obj *eval2(struct state *s) { depth++; struct aml_obj *r = eval(s); depth--; return r; }

/* ---- resynchronising after a failed term at table load ----
 * As ACPICA, a term that fails while a table loads is skipped and loading
 * goes on with the next one. A failure can leave the position anywhere in
 * the term, so its end is found by re-parsing it from its start without
 * executing anything. A method call takes as many arguments as the method
 * declares, so a name in argument position must resolve; anything this
 * cannot size (unresolved name, unknown opcode) gives up and the caller
 * skips the rest of the enclosing block instead. */
struct skip { const uint8_t *p, *end; struct aml_node *scope; int depth; };
static bool sk_bytes(struct skip *k, size_t n) { if ((size_t)(k->end - k->p) < n) return false; k->p += n; return true; }
static bool sk_pkg(struct skip *k) {   /* at a PkgLength: to the end of what it covers */
    const uint8_t *start = k->p;
    if (k->p >= k->end) return false;
    int n = *k->p >> 6;
    if (k->end - k->p < n + 1) return false;
    uint32_t len = n ? *k->p & 0x0F : *k->p & 0x3F;
    for (int i = 0; i < n; i++) len |= (uint32_t)k->p[1 + i] << (4 + 8 * i);
    if (len < (uint32_t)n + 1 || len > (size_t)(k->end - start)) return false;
    k->p = start + len;
    return true;
}
static bool sk_name(struct skip *k, struct name *n) {
    n->root = false; n->up = 0;
    if (k->p < k->end && *k->p == '\\') { n->root = true; k->p++; }
    else while (k->p < k->end && *k->p == '^') { n->up++; k->p++; }
    if (k->p >= k->end) return false;
    uint8_t c = *k->p;
    if (c == 0x00) { n->nseg = 0; k->p++; }
    else if (c == 0x2E) { n->nseg = 2; k->p++; }
    else if (c == 0x2F) { if (k->end - k->p < 2) return false; n->nseg = k->p[1]; k->p += 2; }
    else if (name_lead(c)) n->nseg = 1;
    else return false;
    n->segs = k->p;
    return sk_bytes(k, (size_t)n->nseg * 4);
}
static bool sk_term(struct skip *k);
static bool sk_args(struct skip *k, int n) { while (n-- > 0) if (!sk_term(k)) return false; return true; }
static bool sk_super(struct skip *k) {   /* SuperName / Target: a plain name here is not a call */
    struct name n;
    if (k->p < k->end && *k->p == 0x00) { k->p++; return true; }
    if (k->p < k->end && name_start(*k->p)) return sk_name(k, &n);
    return sk_term(k);
}
static bool sk_ext(struct skip *k) {
    struct name n;
    if (k->p >= k->end) return false;
    switch (*k->p++) {
    case 0x30: case 0x31: case 0x33: return true;                                     /* Revision, Debug, Timer */
    case 0x01: return sk_name(k, &n) && sk_bytes(k, 1);                               /* Mutex */
    case 0x02: return sk_name(k, &n);                                                 /* Event */
    case 0x12: return sk_super(k) && sk_super(k);                                     /* CondRefOf */
    case 0x13: return sk_args(k, 3) && sk_name(k, &n);                                /* CreateField */
    case 0x80: return sk_name(k, &n) && sk_bytes(k, 1) && sk_args(k, 2);              /* OperationRegion */
    case 0x88: return sk_name(k, &n) && sk_args(k, 3);                                /* DataTableRegion */
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: return sk_pkg(k);
    case 0x21: case 0x22: return sk_term(k);                                          /* Stall, Sleep */
    case 0x24: case 0x26: case 0x27: case 0x2A: return sk_super(k);                   /* Signal, Reset, Release, Unload */
    case 0x23: return sk_super(k) && sk_bytes(k, 2);                                  /* Acquire */
    case 0x25: return sk_super(k) && sk_term(k);                                      /* Wait */
    case 0x28: case 0x29: return sk_term(k) && sk_super(k);                           /* FromBCD, ToBCD */
    case 0x32: return sk_bytes(k, 5) && sk_term(k);                                   /* Fatal */
    case 0x1F: return sk_args(k, 6);                                                  /* LoadTable */
    case 0x20: return sk_name(k, &n) && sk_super(k);                                  /* Load */
    }
    return false;
}
static bool sk_op(struct skip *k, uint8_t op) {
    struct name n;
    switch (op) {
    case 0x00: case 0x01: case 0xFF: case 0xA3: case 0xCC: case 0xA5: case 0x9F: return true;
    case 0x0A: return sk_bytes(k, 1);
    case 0x0B: return sk_bytes(k, 2);
    case 0x0C: return sk_bytes(k, 4);
    case 0x0E: return sk_bytes(k, 8);
    case 0x0D: while (k->p < k->end && *k->p) k->p++; return sk_bytes(k, 1);
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0xA0: case 0xA1: case 0xA2: return sk_pkg(k);
    case 0x08: return sk_name(k, &n) && sk_term(k);                                   /* Name */
    case 0x06: return sk_name(k, &n) && sk_name(k, &n);                               /* Alias */
    case 0x15: return sk_name(k, &n) && sk_bytes(k, 2);                               /* External */
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: return sk_args(k, 2) && sk_name(k, &n);
    case 0x70: case 0x9D: case 0x80: case 0x81: case 0x82: case 0x96: case 0x97: case 0x98: case 0x99: return sk_term(k) && sk_super(k);
    case 0x71: case 0x75: case 0x76: case 0x87: case 0x8E: return sk_super(k);
    case 0x86: return sk_super(k) && sk_term(k);                                      /* Notify */
    case 0x72: case 0x73: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D:
    case 0x7E: case 0x7F: case 0x84: case 0x85: case 0x88: case 0x9C: return sk_args(k, 2) && sk_super(k);
    case 0x78: return sk_args(k, 2) && sk_super(k) && sk_super(k);                    /* Divide */
    case 0x9E: return sk_args(k, 3) && sk_super(k);                                   /* Mid */
    case 0x83: case 0x92: case 0xA4: return sk_term(k);                               /* DerefOf, LNot, Return */
    case 0x90: case 0x91: case 0x93: case 0x94: case 0x95: return sk_args(k, 2);
    case 0x89: return sk_term(k) && sk_bytes(k, 1) && sk_term(k) && sk_bytes(k, 1) && sk_args(k, 2);   /* Match */
    case 0x5B: return sk_ext(k);
    }
    return false;
}
static __attribute__((noinline)) int sk_argc(struct skip *k) {   /* a name in argument position: -1 if it does not resolve */
    struct name n; struct state t = { .scope = k->scope };
    struct aml_node *node = sk_name(k, &n) ? resolve(&t, &n, true) : NULL;
    return !node ? -1 : node->obj && node->obj->type == AML_METHOD ? node->obj->method.flags & 7 : 0;
}
static bool sk_term(struct skip *k) {
    if (k->p >= k->end || k->depth >= 32) return false;
    uint8_t op = *k->p;
    bool ok;
    k->depth++;
    if (name_start(op)) { int argc = sk_argc(k); ok = argc >= 0 && sk_args(k, argc); }   /* a method call takes its arguments */
    else { k->p++; ok = (op >= 0x60 && op <= 0x6E) || sk_op(k, op); }
    k->depth--;
    return ok;
}
/* the end of the term at start, or NULL if it cannot be determined */
static const uint8_t *term_end(struct state *s, const uint8_t *start, const uint8_t *end) {
    struct skip k = { start, end, s->scope, 0 };
    return sk_term(&k) && k.p > start && k.p <= end ? k.p : NULL;
}
static const uint8_t *load_base;   /* the table being loaded, for offsets in messages */

/* ---- definitions ---- */
static void run_block(struct state *s, struct aml_node *scope, const uint8_t *end) {
    struct aml_node *saved = s->scope;
    s->scope = scope;
    depth++;   /* def_device's frame plus exec_term's: two levels */
    exec_list(s, end);
    depth--;
    s->scope = saved;
    if (s->status == ST_ERROR && !s->f) s->status = ST_OK;   /* table load: carry on after the block */
    if (s->status == ST_OK) s->p = end;
}

static void def_name(struct state *s) {
    struct name n; parse_name(s, &n);
    struct aml_obj *v = eval(s);
    bool existed;
    struct aml_node *node = s->status == ST_OK ? create_node(s, &n, &existed) : NULL;
    if (!node || (existed && node->obj)) { obj_put(v); return; }
    obj_put(node->obj);
    node->obj = v;
}

static void def_alias(struct state *s) {
    struct name src, al; parse_name(s, &src); parse_name(s, &al);
    struct aml_node *target = resolve(s, &src, true);
    if (!target) { char b[64]; name_str(&src, b, sizeof b); kprintf("acpi: Alias of missing object %s ignored\n", b); return; }
    bool existed;
    struct aml_node *node = create_node(s, &al, &existed);
    if (node && !existed) node->alias = target;
}

static void def_scope(struct state *s) {
    const uint8_t *end = pkg_end(s);
    struct name n; parse_name(s, &n);
    if (s->status != ST_OK) return;
    struct aml_node *node = resolve(s, &n, true);
    if (!node) { char b[64]; name_str(&n, b, sizeof b); kprintf("acpi: Scope(%s) does not exist, skipped\n", b); s->p = end; return; }
    run_block(s, node, end);
}

static void def_device(struct state *s, int type) {
    const uint8_t *end = pkg_end(s);
    struct name n; parse_name(s, &n);
    uint8_t id = 0; uint32_t pblk = 0; uint8_t pblk_len = 0;
    if (type == AML_PROCESSOR) { id = u8(s); pblk = uN(s, 4); pblk_len = u8(s); }
    else if (type == AML_POWER) { u8(s); uN(s, 2); }
    if (s->status != ST_OK) return;
    bool existed;
    struct aml_node *node = create_node(s, &n, &existed);
    if (!node) { if (!s->f) s->status = ST_OK; s->p = end; return; }
    if (!existed || !node->obj) {
        obj_put(node->obj);
        node->obj = obj_new(type);
        if (type == AML_PROCESSOR) { node->obj->proc.id = id; node->obj->proc.pblk = pblk; node->obj->proc.pblk_len = pblk_len; }
    }
    run_block(s, node, end);
}

static void def_method(struct state *s) {
    const uint8_t *end = pkg_end(s);
    struct name n; parse_name(s, &n);
    uint8_t flags = u8(s);
    if (s->status != ST_OK) return;
    bool existed;
    struct aml_node *node = create_node(s, &n, &existed);
    if (node && (!existed || !node->obj)) {
        struct aml_obj *m = obj_new(AML_METHOD);
        m->method.code = s->p; m->method.len = end - s->p; m->method.flags = flags;
        obj_put(node->obj);
        node->obj = m;
    }
    if (!node && !s->f) s->status = ST_OK;
    s->p = end;
}

static void def_simple(struct state *s, int type) {   /* Mutex, Event */
    struct name n; parse_name(s, &n);
    uint8_t sync = type == AML_MUTEX ? u8(s) : 0;
    bool existed;
    struct aml_node *node = s->status == ST_OK ? create_node(s, &n, &existed) : NULL;
    if (node && !existed) { node->obj = obj_new(type); if (type == AML_MUTEX) node->obj->mutex.sync = sync & 0xF; }
}

static void def_region(struct state *s, bool data_table) {
    struct name n; parse_name(s, &n);
    struct aml_obj *r = obj_new(AML_REGION);
    if (data_table) {   /* DataTableRegion(name, signature, OEM ID, OEM table ID) */
        struct aml_obj *a[3];
        for (int i = 0; i < 3; i++) a[i] = eval_value(s);
        char sig[5]; bool found = false;
        for (int i = 0; s->status == ST_OK && a[0]->type == AML_STRING && a[0]->str.len >= 4 && acpi_table_at(i, sig); i++) {
            const uint8_t *tbl = acpi_table_at(i, sig);
            if (memcmp(sig, a[0]->str.s, 4)) continue;
            r->region.space = SPACE_MEMORY; r->region.base = V2P(tbl); r->region.len = *(const uint32_t *)(tbl + 4);
            found = true;
            break;
        }
        /* no such table: a region of length 0, so every access to its fields is refused */
        if (!found && s->status == ST_OK) { char b[64]; name_str(&n, b, sizeof b); kprintf("acpi: DataTableRegion %s: table not found, unusable\n", b); r->region.space = SPACE_NONE; }
        for (int i = 0; i < 3; i++) obj_put(a[i]);
    } else {
        r->region.space = u8(s);
        r->region.base = eval_int(s);
        r->region.len = eval_int(s);
        if (r->region.base + r->region.len < r->region.base) {   /* wraps: unusable, as a missing data table */
            char b[64]; name_str(&n, b, sizeof b);
            kprintf("acpi: OperationRegion %s [0x%lx + 0x%lx] wraps around, unusable\n", b, r->region.base, r->region.len);
            r->region.len = 0;
        }
    }
    bool existed;
    struct aml_node *node = s->status == ST_OK ? create_node(s, &n, &existed) : NULL;
    if (!node || (existed && node->obj)) { obj_put(r); return; }
    r->region.node = node;
    node->obj = r;
}

#define FIELD_MAX_BITS (1u << 23)   /* 1 MiB: field_read buffers the whole field */
static void parse_field_list(struct state *s, const uint8_t *end, struct aml_obj *tmpl) {
    uint64_t bit = 0;
    uint8_t flags = tmpl->field.flags, attrib = 0;
    while (s->p < end && s->status == ST_OK) {
        uint8_t c = *s->p;
        if (c == 0x00) { s->p++; bit += pkglen(s); }                                       /* ReservedField */
        else if (c == 0x01) { s->p++; flags = (flags & ~0xF) | (u8(s) & 0xF); attrib = u8(s); }   /* AccessField */
        else if (c == 0x03) { s->p++; flags = (flags & ~0xF) | (u8(s) & 0xF); attrib = u8(s); u8(s); }   /* ExtendedAccessField */
        else if (c == 0x02) {   /* ConnectField (GPIO / serial bus connections): parsed and ignored */
            s->p++;
            if (more(s) && *s->p == 0x11) { s->p++; s->p = pkg_end(s); }
            else { struct name n; parse_name(s, &n); }
        } else {
            if (s->p + 4 > end) { fail(s, "truncated field list"); return; }
            struct name n = { false, 0, 1, s->p };
            s->p += 4;
            uint32_t len = pkglen(s);
            if (s->status != ST_OK) return;
            if (len > FIELD_MAX_BITS) { kprintf("acpi: field %.4s of %u bits ignored\n", (const char *)n.segs, len); bit += len; continue; }
            struct aml_node *rn = tmpl->field.kind != FIELD_INDEX ? ns_resolve_alias(tmpl->field.region) : NULL;
            if (rn && rn->obj && rn->obj->type == AML_REGION && (bit + len + 7) / 8 > rn->obj->region.len)   /* accesses will be refused */
                kprintf("acpi: field %.4s [bit %lu + %u] lies beyond its %lu-byte region %.4s\n", (const char *)n.segs, bit, len, rn->obj->region.len, rn->name);
            bool existed;
            struct aml_node *node = create_node(s, &n, &existed);
            if (node && !(existed && node->obj)) {
                struct aml_obj *f = obj_new(AML_FIELD);
                f->field = tmpl->field;
                f->field.flags = flags; f->field.attrib = attrib;
                f->field.bit_off = bit; f->field.bit_len = len;
                obj_put(node->obj);
                node->obj = f;
            }
            bit += len;
        }
    }
    s->p = end;
}

static void def_field(struct state *s, int kind) {
    const uint8_t *end = pkg_end(s);
    struct name a, b;
    struct aml_obj tmpl; memset(&tmpl, 0, sizeof tmpl);
    tmpl.field.kind = kind;
    parse_name(s, &a);
    if (kind != FIELD_NORMAL) parse_name(s, &b);
    if (kind == FIELD_BANK) tmpl.field.bank_val = eval_int(s);
    tmpl.field.flags = u8(s);
    if (s->status != ST_OK) return;
    struct aml_node *na = resolve(s, &a, true), *nb = kind != FIELD_NORMAL ? resolve(s, &b, true) : NULL;
    if (!na || (kind != FIELD_NORMAL && !nb)) {
        char buf[64]; name_str(!na ? &a : &b, buf, sizeof buf);
        kprintf("acpi: field list on missing %s skipped\n", buf);
        s->p = end;
        return;
    }
    if (kind == FIELD_NORMAL) tmpl.field.region = na;
    else if (kind == FIELD_INDEX) { tmpl.field.index = na; tmpl.field.data = nb; }
    else { tmpl.field.region = na; tmpl.field.bank = nb; }
    parse_field_list(s, end, &tmpl);
}

static void def_create_field(struct state *s, uint8_t op) {
    struct aml_obj *src = eval(s);
    uint64_t off, len;
    if (op == 0x13) { off = eval_int(s); len = eval_int(s); }
    else {
        uint64_t i = eval_int(s);
        switch (op) {
        case 0x8D: off = i; len = 1; break;
        case 0x8C: off = i * 8; len = 8; break;
        case 0x8B: off = i * 8; len = 16; break;
        case 0x8A: off = i * 8; len = 32; break;
        default:   off = i * 8; len = 64; break;
        }
    }
    struct name n; parse_name(s, &n);
    struct aml_obj *buf = aml_value_of(src);
    obj_put(src);
    if (s->status == ST_OK && buf->type != AML_BUFFER) fail(s, "buffer field on a %s", obj_type_name(buf->type));
    if (s->status == ST_OK && off + len > buf->buf.len * 8) fail(s, "buffer field [%lu+%lu bits] beyond a %lu-byte buffer", off, len, buf->buf.len);
    bool existed;
    struct aml_node *node = s->status == ST_OK ? create_node(s, &n, &existed) : NULL;
    if (!node) { obj_put(buf); return; }
    struct aml_obj *f = obj_new(AML_BUFFER_FIELD);
    f->bfield.buf = buf; f->bfield.bit_off = off; f->bfield.bit_len = len;
    obj_put(node->obj);
    node->obj = f;
}

/* ---- statements ---- */
static void stmt_if(struct state *s) {
    const uint8_t *end = pkg_end(s);
    uint64_t pred = eval_int(s);
    if (s->status != ST_OK) return;
    if (pred) {
        exec_list(s, end);
        if (s->status != ST_OK) return;
        s->p = end;
        if (more(s) && *s->p == 0xA1) { s->p++; s->p = pkg_end(s); }
    } else {
        s->p = end;
        if (more(s) && *s->p == 0xA1) {
            s->p++;
            const uint8_t *else_end = pkg_end(s);
            exec_list(s, else_end);
            if (s->status == ST_OK) s->p = else_end;
        }
    }
}

static void stmt_while(struct state *s) {
    const uint8_t *end = pkg_end(s), *pred_at = s->p;
    uint64_t start = time_ms();
    for (;;) {
        s->p = pred_at;
        uint64_t pred = eval_int(s);
        if (s->status != ST_OK || !pred) break;
        exec_list(s, end);
        if (s->status == ST_BREAK) { s->status = ST_OK; break; }
        if (s->status == ST_CONTINUE) s->status = ST_OK;
        if (s->status != ST_OK) return;
        if (time_ms() - start > LOOP_TIMEOUT_MS) { fail(s, "While loop did not finish in %d ms", LOOP_TIMEOUT_MS); return; }
    }
    if (s->status == ST_OK) s->p = end;
}

static void exec_ext(struct state *s) {
    uint8_t op = s->p + 1 < s->end ? s->p[1] : 0;
    switch (op) {
    case 0x01: s->p += 2; def_simple(s, AML_MUTEX); return;
    case 0x02: s->p += 2; def_simple(s, AML_EVENT); return;
    case 0x13: s->p += 2; def_create_field(s, 0x13); return;
    case 0x80: s->p += 2; def_region(s, false); return;
    case 0x88: s->p += 2; def_region(s, true); return;
    case 0x81: s->p += 2; def_field(s, FIELD_NORMAL); return;
    case 0x86: s->p += 2; def_field(s, FIELD_INDEX); return;
    case 0x87: s->p += 2; def_field(s, FIELD_BANK); return;
    case 0x82: s->p += 2; def_device(s, AML_DEVICE); return;
    case 0x83: s->p += 2; def_device(s, AML_PROCESSOR); return;
    case 0x84: s->p += 2; def_device(s, AML_POWER); return;
    case 0x85: s->p += 2; def_device(s, AML_THERMAL); return;
    case 0x21: { s->p += 2; uint64_t us = eval_int(s); acpi_stall_us(MIN(us, 1000u)); return; }   /* Stall */
    case 0x22: { s->p += 2; uint64_t ms = eval_int(s); if (s->status == ST_OK) thread_sleep_ms(MIN(ms, 5000u)); return; }   /* Sleep */
    case 0x27: {   /* Release */
        s->p += 2;
        struct aml_obj *m = parse_supername(s, false);
        if (s->status == ST_OK && is_gl(m) && (!s->f || s->f->gl_held > 0)) { if (s->f) s->f->gl_held--; acpi_gl_release(); }
        obj_put(m);
        return;
    }
    case 0x24: case 0x26: case 0x2A: { s->p += 2; obj_put(parse_supername(s, false)); return; }   /* Signal, Reset, Unload */
    case 0x32: { s->p += 2; uint8_t type = u8(s); uint32_t code = uN(s, 4); uint64_t arg = eval_int(s); kprintf("acpi: firmware Fatal(%u, 0x%x, 0x%lx)\n", type, code, arg); return; }
    }
    obj_put(eval(s));
}

/* the object a Notify target designates: ArgX/LocalX holding a reference resolve through it */
static struct aml_node *ref_node(struct aml_obj *t) {
    for (int k = 0; t && t->type == AML_REFERENCE && k < 8; k++) {
        if (t->ref.kind == REF_NAMED) return ns_resolve_alias(t->ref.node);
        if (t->ref.kind == REF_LOCAL || t->ref.kind == REF_ARG) t = *t->ref.slot;
        else if (t->ref.kind == REF_INDEX && t->ref.target->type == AML_PACKAGE && t->ref.index < t->ref.target->pkg.n) t = t->ref.target->pkg.e[t->ref.index];
        else return NULL;
    }
    return NULL;
}

static void exec_term(struct state *s) {
    if (depth >= MAX_DEPTH) { fail(s, "terms nested too deeply"); return; }
    depth++;
    uint8_t op = *s->p;
    switch (op) {
    case 0x08: s->p++; def_name(s); break;
    case 0x06: s->p++; def_alias(s); break;
    case 0x10: s->p++; def_scope(s); break;
    case 0x14: s->p++; def_method(s); break;
    case 0x15: { s->p++; struct name n; parse_name(s, &n); uN(s, 2); break; }   /* External */
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: s->p++; def_create_field(s, op); break;
    case 0xA0: s->p++; stmt_if(s); break;
    case 0xA1: s->p++; s->p = pkg_end(s); break;                                 /* Else without If */
    case 0xA2: s->p++; stmt_while(s); break;
    case 0xA3: case 0xCC: s->p++; break;                                         /* Noop, BreakPoint */
    case 0xA4: {
        s->p++;
        struct aml_obj *v = eval(s);
        if (s->status != ST_OK) { obj_put(v); break; }
        struct aml_obj *d = detach(s, v, false, 0);
        obj_put(v);
        if (s->status != ST_OK) { obj_put(d); break; }
        if (s->f) { obj_put(s->f->ret); s->f->ret = d; } else obj_put(d);
        s->status = ST_RETURN;
        break;
    }
    case 0xA5: s->p++; s->status = ST_BREAK; break;
    case 0x9F: s->p++; s->status = ST_CONTINUE; break;
    case 0x86: {   /* Notify */
        s->p++;
        struct aml_obj *t = parse_supername(s, false);
        uint64_t v = eval_int(s);
        struct aml_node *n = s->status == ST_OK ? ref_node(t) : NULL;
        if (n) acpi_notify(n, v);
        obj_put(t);
        break;
    }
    case 0x5B: exec_ext(s); break;
    default: obj_put(eval(s)); break;
    }
    depth--;
}

/* the term list of a block: s->end is narrowed to the block, so lookahead
 * (an If checking for its Else) cannot run into the enclosing block */
static void exec_list(struct state *s, const uint8_t *end) {
    const uint8_t *saved = s->end;
    if (end < s->end) s->end = end;
    while (s->p < s->end && s->status == ST_OK) {
        const uint8_t *start = s->p;
        exec_term(s);
        if (s->status != ST_ERROR || s->f) continue;   /* in a method, an error aborts it */
        const uint8_t *next = term_end(s, start, s->end);   /* table load: skip just the failed term */
        if (!next) { kprintf("acpi: %s: cannot size the failed term at 0x%lx, skipping the rest of its block\n", load_what, (uint64_t)(start - load_base)); break; }
        s->p = next; s->status = ST_OK;
    }
    s->end = saved;
}

/* ---- entry points ---- */
int aml_load_table(const void *table, size_t len, const char *what) {
    if (len < 36 || len > ACPI_MAX_TABLE || *(const uint32_t *)((const uint8_t *)table + 4) != len) { kprintf("acpi: %s: bad table length %lu\n", what, len); return -EINVAL; }
    aml_lock();
    /* the same table again (as ACPICA: same length and contents) is a no-op */
    for (int i = 0; i < nloaded; i++) if (loaded[i].len == len && !memcmp(loaded[i].copy, table, len)) { aml_unlock(); return 0; }
    if (nloaded == loaded_cap) {
        int cap = loaded_cap ? loaded_cap * 2 : 16;
        void *n = krealloc(loaded, cap * sizeof *loaded);
        if (n) { loaded = n; loaded_cap = cap; }
    }
    uint8_t *copy = kmalloc(len);   /* methods point into it: kept forever */
    memcpy(copy, table, len);
    if (nloaded < loaded_cap) { loaded[nloaded].copy = copy; loaded[nloaded].len = len; nloaded++; }
    const char *saved = load_what; const uint8_t *saved_base = load_base;
    load_what = what; load_base = copy;
    struct state s = { .p = copy + 36, .end = copy + len, .scope = aml_root };
    while (s.p < s.end) {
        const uint8_t *start = s.p;
        exec_term(&s);
        if (s.status == ST_OK) continue;
        /* a failed term (as in exec_list), or a stray Return/Break that may have stopped inside one */
        const uint8_t *next = term_end(&s, start, s.end);
        if (!next) { kprintf("acpi: %s: cannot size the term at 0x%lx, giving up\n", what, (uint64_t)(start - copy)); break; }
        s.p = next; s.status = ST_OK;
    }
    load_what = saved; load_base = saved_base;
    aml_unlock();
    return s.status == ST_ERROR ? -EIO : 0;
}

/* A package element naming an Integer, String, Buffer or Package is a
 * reference, so AML sees later stores to that name. C callers get such
 * elements as values instead (ACPICA also hands out packages with named data
 * resolved), so pm.c, irq.c and _CID matching need not chase them; devices
 * and other objects stay references. Copies only packages that need it. */
static bool named_data(struct aml_obj *e) {
    if (e->type != AML_REFERENCE || e->ref.kind != REF_NAMED) return false;
    struct aml_obj *o = ns_resolve_alias(e->ref.node)->obj;
    return o && o->type >= AML_INTEGER && o->type <= AML_PACKAGE;
}
static bool has_named_data(struct aml_obj *o, int lvl) {
    if (named_data(o)) return true;
    for (size_t i = 0; o->type == AML_PACKAGE && lvl < 8 && i < o->pkg.n; i++) if (has_named_data(o->pkg.e[i], lvl + 1)) return true;
    return false;
}
static struct aml_obj *resolve_named_data(struct aml_obj *o, int lvl) {
    if (lvl >= 8 || !has_named_data(o, lvl)) return obj_get(o);
    if (o->type != AML_PACKAGE) { struct aml_obj *v = aml_value_of(o), *r = resolve_named_data(v, lvl + 1); obj_put(v); return r; }
    struct aml_obj *p = obj_pkg(o->pkg.n);
    for (size_t i = 0; i < o->pkg.n; i++) { obj_put(p->pkg.e[i]); p->pkg.e[i] = resolve_named_data(o->pkg.e[i], lvl + 1); }
    return p;
}

int aml_eval(struct aml_node *n, struct aml_obj **args, int argc, struct aml_obj **result) {
    aml_lock();
    n = ns_resolve_alias(n);
    struct aml_obj *r = NULL;
    int rc = 0;
    if (!n->obj) rc = -ENOENT;
    else if (n->obj->type == AML_METHOD) {
        struct state caller = { .status = ST_OK };
        r = invoke(&caller, n, args, argc);
        if (caller.status == ST_ERROR) rc = -EIO;
    } else r = node_value(n);
    if (rc == 0 && r && r->type == AML_PACKAGE) { struct aml_obj *c = resolve_named_data(r, 0); obj_put(r); r = c; }
    aml_unlock();
    if (rc < 0) { obj_put(r); r = NULL; }
    if (result) *result = r; else obj_put(r);
    return rc;
}

int aml_eval_path(struct aml_node *scope, const char *path, struct aml_obj **args, int argc, struct aml_obj **result) {
    if (result) *result = NULL;
    if (!aml_root) return -ENOENT;
    aml_lock();
    struct aml_node *n = ns_lookup(scope, path);
    int rc = n ? aml_eval(n, args, argc, result) : -ENOENT;
    aml_unlock();
    return rc;
}

int aml_eval_int(struct aml_node *scope, const char *path, uint64_t *out) {
    struct aml_obj *r;
    int rc = aml_eval_path(scope, path, NULL, 0, &r);
    if (rc < 0) return rc;
    aml_lock();
    struct aml_obj *v = aml_value_of(r);
    bool ok = obj_to_int(v, out);
    obj_put(v); obj_put(r);
    aml_unlock();
    return ok ? 0 : -EINVAL;
}

/* ---- predefined objects ---- */
static struct aml_obj *native_osi(struct aml_obj **args, int argc) {
    static const char *const supported[] = {
        "Windows 2000", "Windows 2001", "Windows 2001 SP1", "Windows 2001.1", "Windows 2001 SP2", "Windows 2001.1 SP1",
        "Windows 2006", "Windows 2006.1", "Windows 2006 SP1", "Windows 2006 SP2", "Windows 2009", "Windows 2012",
        "Windows 2013", "Windows 2015", "Windows 2016", "Windows 2017", "Windows 2017.2", "Windows 2018",
        "Windows 2018.2", "Windows 2019", "Windows 2020", "Windows 2021", "Windows 2022",
        "Module Device", "Processor Device", "3.0 Thermal Model", "3.0 _SCP Extensions", "Processor Aggregator Device",
        "Extended Address Space Descriptor",
    };
    if (argc < 1 || args[0]->type != AML_STRING) return obj_int(0);
    for (size_t i = 0; i < ARRAY_LEN(supported); i++) if (!strcmp(args[0]->str.s, supported[i])) return obj_int(aml_ones());
    return obj_int(0);
}

void aml_init(void) {
    ns_init();
    static const char *const scopes[] = { "_GPE", "_PR_", "_SB_", "_SI_", "_TZ_" };
    for (size_t i = 0; i < ARRAY_LEN(scopes); i++) ns_add(aml_root, scopes[i])->obj = obj_new(AML_DEVICE);
    gl_node = ns_add(aml_root, "_GL_");
    gl_node->obj = obj_new(AML_MUTEX);
    ns_add(aml_root, "_OS_")->obj = obj_str("Microsoft Windows NT", 20);
    ns_add(aml_root, "_REV")->obj = obj_int(2);
    struct aml_obj *osi = obj_new(AML_METHOD);
    osi->method.flags = 1; osi->method.native = native_osi;
    ns_add(aml_root, "_OSI")->obj = osi;
}
