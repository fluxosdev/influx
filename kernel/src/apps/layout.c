/*
 * HTML layout for the web browser. Walks the DOM once per window width and
 * produces a display list in document coordinates: inline text is broken into
 * words and packed into lines, blocks stack with collapsed margins, lists get
 * markers, and tables are sized from the minimum and preferred widths of their
 * cells (measured with the same code in a non-emitting pass).
 */
#include "html.h"
#include "ui.h"
#include "mm.h"
#include "string.h"
#include "net.h"

#define PAGE_MARGIN 14
#define MAX_LAYOUT_DEPTH 100   /* x ~384 bytes of stack per level (flow_node + flow_table every other level) */
#define MAX_COLS 64
#define MAX_ROWS 2048

struct style {
    int family;                 /* 0 sans, 1 mono */
    bool bold, italic, underline, strike, pre, nowrap;
    int size;                   /* 0..5: 11 13 15 18 22 28 px */
    color_t color;
    int link;
    int list_depth;
};

/* ---- fonts ---- */
static const int size_px[6] = { 11, 13, 15, 18, 22, 28 };
static struct font *font_cache[2][2][6];
static bool fonts_loaded;
static struct mutex font_lock = MUTEX_INIT;

static void load_fonts(void) {
    mutex_lock(&font_lock);
    if (!fonts_loaded) {
        char path[64];
        for (int b = 0; b < 2; b++)
            for (int s = 0; s < 6; s++) {
                snprintf(path, sizeof path, "/sys/fonts/inter-%s-%d.fnt", b ? "bold" : "regular", size_px[s]);
                font_cache[0][b][s] = font_load(path);
                if (!font_cache[0][b][s]) font_cache[0][b][s] = b ? font_ui_bold : (s >= 3 ? font_ui_large : font_ui);
                font_cache[1][b][s] = b ? font_mono_bold : font_mono;
            }
        fonts_loaded = true;
    }
    mutex_unlock(&font_lock);
}
static struct font *style_font(const struct style *st) { return font_cache[st->family][st->bold][CLAMP(st->size, 0, 5)]; }
static int leading(struct font *f) { return font_height(f) * 2 / 5; }

/* ---- layout state ---- */
enum { FR_TEXT, FR_BOX };
struct frag {
    int kind, x, w, ascent, descent;
    struct font *font; color_t color; int flags, link;
    const char *text; int len;
    struct html_node *node; int item_type, h;
};

struct lay { struct layout *L; struct html_doc *doc; int depth; };

struct flow {
    struct lay *lay;
    int origin;                 /* left edge the flow started with (for natural widths) */
    int left, right, y, margin;
    struct frag *frags; int nfrags, cap;
    int line_w;
    bool space; int space_w;
    int align;
    char marker[16]; struct font *marker_font; color_t marker_color; bool has_marker;
    int max_right, min_w;
    bool emit;
    struct font *strut;
};

static bool starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) if (tolower(*s++) != *prefix++) return false;
    return true;
}

/* text that items point at must live as long as the layout */
static const char *keep_string(struct layout *L, const char *s) {
    L->strs = krealloc(L->strs, (L->nstrs + 1) * sizeof *L->strs);
    return L->strs[L->nstrs++] = kstrdup(s);
}

static int add_item(struct flow *F, const struct display_item *it) {
    struct layout *L = F->lay->L;
    if (!F->emit) return -1;
    if (L->nitems == L->cap) { L->cap = L->cap ? L->cap * 2 : 512; L->items = krealloc(L->items, L->cap * sizeof *L->items); }
    L->items[L->nitems] = *it;
    return L->nitems++;
}

static int add_link(struct layout *L, const char *url, struct html_node *n) {
    L->links = krealloc(L->links, (L->nlinks + 1) * sizeof *L->links);
    L->link_nodes = krealloc(L->link_nodes, (L->nlinks + 1) * sizeof *L->link_nodes);
    L->links[L->nlinks] = kstrdup(url);
    L->link_nodes[L->nlinks] = n;
    return L->nlinks++;
}

static void add_anchor(struct flow *F, const char *name) {
    struct layout *L = F->lay->L;
    if (!F->emit || !name || !*name) return;
    L->anchors = krealloc(L->anchors, (L->nanchors + 1) * sizeof *L->anchors);
    L->anchors[L->nanchors].name = kstrdup(name);
    L->anchors[L->nanchors].y = F->y + F->margin;
    L->nanchors++;
}

static void push_frag(struct flow *F, const struct frag *fr) {
    if (F->nfrags == F->cap) { F->cap = F->cap ? F->cap * 2 : 64; F->frags = krealloc(F->frags, F->cap * sizeof *F->frags); }
    F->frags[F->nfrags++] = *fr;
}

static void flush_line(struct flow *F, bool force) {
    if (!F->nfrags && !force && !F->has_marker) return;
    F->y += F->margin; F->margin = 0;
    int asc = 0, desc = 0;
    for (int i = 0; i < F->nfrags; i++) { asc = MAX(asc, F->frags[i].ascent); desc = MAX(desc, F->frags[i].descent); }
    if (!F->nfrags || F->has_marker) {
        struct font *f = F->has_marker && !F->nfrags ? F->marker_font : F->strut;
        int ld = leading(f);
        asc = MAX(asc, font_ascent(f) + ld / 2);
        desc = MAX(desc, font_height(f) - font_ascent(f) + ld - ld / 2);
    }
    int avail = F->right - F->left;
    int off = F->align == 1 ? (avail - F->line_w) / 2 : F->align == 2 ? avail - F->line_w : 0;
    if (off < 0) off = 0;
    if (F->emit) {
        for (int i = 0; i < F->nfrags; i++) {
            struct frag *fr = &F->frags[i];
            struct display_item it = { 0 };
            it.x = F->left + off + fr->x;
            it.w = fr->w;
            it.link = fr->link;
            it.color = fr->color;
            if (fr->kind == FR_TEXT) {
                it.type = DI_TEXT;
                it.font = fr->font; it.flags = fr->flags;
                it.text = fr->text; it.len = fr->len;
                it.y = F->y + asc - font_ascent(fr->font);
                it.h = font_height(fr->font);
                it.ul_w = fr->w;
                /* underline the gap to the next word of the same link */
                if (i + 1 < F->nfrags && fr->link >= 0 && F->frags[i + 1].link == fr->link && F->frags[i + 1].kind == FR_TEXT)
                    it.ul_w = F->frags[i + 1].x - fr->x;
            } else {
                it.type = fr->item_type;
                it.node = fr->node;
                it.y = F->y + asc - fr->ascent;
                it.h = fr->h;
            }
            add_item(F, &it);
        }
        if (F->has_marker) {
            struct display_item it = { 0 };
            it.type = DI_TEXT; it.font = F->marker_font; it.color = F->marker_color; it.link = -1;
            it.text = keep_string(F->lay->L, F->marker); it.len = strlen(F->marker);
            it.w = font_text_width(F->marker_font, F->marker);
            it.x = F->left - it.w - 8;
            it.y = F->y + asc - font_ascent(F->marker_font);
            it.h = font_height(F->marker_font);
            add_item(F, &it);
        }
    }
    F->max_right = MAX(F->max_right, F->left + F->line_w);
    F->has_marker = false;
    F->y += asc + desc;
    F->nfrags = 0; F->line_w = 0; F->space = false;
}

static void note_min(struct flow *F, int w) { F->min_w = MAX(F->min_w, F->left - F->origin + w); }

static void add_text_frag(struct flow *F, const struct style *st, struct font *f, const char *s, int n, int w, int gap) {
    struct frag fr = { 0 };
    int ld = leading(f);
    fr.kind = FR_TEXT; fr.x = F->line_w + gap; fr.w = w;
    fr.ascent = font_ascent(f) + ld / 2; fr.descent = font_height(f) - font_ascent(f) + ld - ld / 2;
    fr.font = f; fr.color = st->color; fr.link = st->link; fr.text = s; fr.len = n;
    fr.flags = (st->underline ? DIF_UNDERLINE : 0) | (st->strike ? DIF_STRIKE : 0) | (st->italic ? DIF_ITALIC : 0);
    push_frag(F, &fr);
    F->line_w += gap + w;
}

static void add_word(struct flow *F, const struct style *st, struct font *f, const char *s, int n) {
    int w = font_text_width_n(f, s, n);
    int gap = F->space && F->nfrags ? F->space_w : 0;
    F->space = false;
    int avail = F->right - F->left;
    if (F->nfrags && !st->nowrap && F->line_w + gap + w > avail) { flush_line(F, false); gap = 0; }
    if (!F->nfrags && w > avail && avail > 20 && !st->nowrap) {
        /* a word wider than the line: break it between characters */
        while (n > 0) {
            int k = MIN(font_fit_chars(f, s, avail), n);
            if (k <= 0) { const char *q = s; utf8_decode(&q); k = q - s; }
            int kw = font_text_width_n(f, s, k);
            add_text_frag(F, st, f, s, k, kw, 0);
            note_min(F, MIN(kw, avail));
            s += k; n -= k;
            if (n > 0) flush_line(F, false);
        }
        return;
    }
    add_text_frag(F, st, f, s, n, w, gap);
    note_min(F, w);
}

static void flow_text(struct flow *F, const struct style *st, const char *s, int n) {
    struct font *f = style_font(st);
    int i = 0;
    while (i < n) {
        if (isspace(s[i])) { F->space = true; F->space_w = font_char_width(f, ' '); i++; continue; }
        int j = i;
        while (j < n && !isspace(s[j])) j++;
        add_word(F, st, f, s + i, j - i);
        i = j;
    }
}

static void flow_pre(struct flow *F, const struct style *st, const char *s, int n) {
    struct font *f = style_font(st);
    int sw = font_char_width(f, ' ');
    int i = 0;
    while (i < n) {
        if (s[i] == '\n') { flush_line(F, true); i++; continue; }
        if (s[i] == '\r') { i++; continue; }
        if (s[i] == '\t') { int col = F->line_w / (sw ? sw : 1); F->line_w = (col / 8 + 1) * 8 * sw; i++; note_min(F, F->line_w); continue; }
        int j = i;
        while (j < n && s[j] != '\n' && s[j] != '\t' && s[j] != '\r') j++;
        int w = font_text_width_n(f, s + i, j - i);
        add_text_frag(F, st, f, s + i, j - i, w, 0);
        note_min(F, F->line_w);
        i = j;
    }
}

static void add_box(struct flow *F, const struct style *st, int type, struct html_node *node, int w, int h, int baseline_drop) {
    int gap = F->space && F->nfrags ? F->space_w : 0;
    F->space = false;
    int avail = F->right - F->left;
    if (F->nfrags && !st->nowrap && F->line_w + gap + w > avail) { flush_line(F, false); gap = 0; }
    struct frag fr = { 0 };
    fr.kind = FR_BOX; fr.item_type = type; fr.node = node;
    fr.x = F->line_w + gap; fr.w = w; fr.h = h;
    fr.ascent = h - baseline_drop; fr.descent = baseline_drop;
    fr.link = st->link; fr.color = st->color;
    push_frag(F, &fr);
    F->line_w += gap + w;
    note_min(F, w);
}

/* a block boundary: finish the line and collapse the margin */
static void block_break(struct flow *F, int margin) {
    flush_line(F, false);
    F->margin = MAX(F->margin, margin);
    F->space = false;
}

/* ---- styles ---- */
static int parse_length(const char *v, int avail) {
    if (!v || !isdigit(*v)) return -1;
    int n = atoi(v);
    const char *p = v; while (isdigit(*p)) p++;
    if (*p == '.') { p++; while (isdigit(*p)) p++; }
    if (*p == '%') return avail * n / 100;
    return n;
}

struct css { color_t bg; bool has_bg; bool hidden; int align; };

static void apply_inline_css(const char *s, struct style *st, struct css *css) {
    if (!s) return;
    while (*s) {
        while (isspace(*s) || *s == ';') s++;
        const char *ns = s;
        while (*s && *s != ':' && *s != ';') s++;
        if (*s != ':') break;
        char prop[32]; size_t pl = MIN((size_t)(s - ns), sizeof prop - 1);
        for (size_t i = 0; i < pl; i++) prop[i] = tolower(ns[i]);
        while (pl && isspace(prop[pl - 1])) pl--;
        prop[pl] = 0;
        s++;
        while (isspace(*s)) s++;
        char val[96]; size_t vl = 0;
        while (*s && *s != ';' && vl < sizeof val - 1) val[vl++] = tolower(*s++);
        while (vl && isspace(val[vl - 1])) vl--;
        val[vl] = 0;
        if (!strcmp(prop, "color")) html_parse_color(val, &st->color);
        else if (!strcmp(prop, "background-color") || !strcmp(prop, "background")) { if (html_parse_color(val, &css->bg)) css->has_bg = true; }
        else if (!strcmp(prop, "display") && !strcmp(val, "none")) css->hidden = true;
        else if (!strcmp(prop, "visibility") && !strcmp(val, "hidden")) css->hidden = true;
        else if (!strcmp(prop, "font-weight")) st->bold = !strcmp(val, "bold") || !strcmp(val, "bolder") || atoi(val) >= 600;
        else if (!strcmp(prop, "font-style")) st->italic = !strcmp(val, "italic") || !strcmp(val, "oblique");
        else if (!strcmp(prop, "text-decoration") || !strcmp(prop, "text-decoration-line")) { st->underline = strstr(val, "underline") != NULL; st->strike = strstr(val, "line-through") != NULL; }
        else if (!strcmp(prop, "text-align")) css->align = !strcmp(val, "center") ? 1 : !strcmp(val, "right") ? 2 : 0;
        else if (!strcmp(prop, "white-space")) { if (!strncmp(val, "pre", 3)) st->pre = true; else if (!strcmp(val, "nowrap")) st->nowrap = true; }
        else if (!strcmp(prop, "font-family") && (strstr(val, "mono") || strstr(val, "courier"))) st->family = 1;
    }
}

static int font_size_attr(const char *v, int cur) {
    if (!v || !*v) return cur;
    int n;
    if (*v == '+' || *v == '-') n = 3 + atoi(v); else n = atoi(v);
    n = CLAMP(n, 1, 7);
    static const int map[8] = { 2, 0, 1, 2, 3, 4, 5, 5 };
    return map[n];
}

static bool is_hidden_tag(int t) {
    switch (t) {
    case T_HEAD: case T_SCRIPT: case T_STYLE: case T_TITLE: case T_META: case T_LINK: case T_BASE: case T_TEMPLATE:
    case T_SVG: case T_MATH: case T_VIDEO: case T_AUDIO: case T_CANVAS: case T_OBJECT: case T_EMBED: case T_PARAM:
    case T_MAP: case T_AREA: case T_SOURCE: case T_TRACK: case T_OPTION: case T_OPTGROUP: case T_COL: case T_COLGROUP:
    case T_DIALOG: return true;
    default: return false;
    }
}

static bool is_block_tag(int t) {
    switch (t) {
    case T_P: case T_DIV: case T_UL: case T_OL: case T_LI: case T_DL: case T_DT: case T_DD: case T_MENU: case T_DIR:
    case T_BLOCKQUOTE: case T_CENTER: case T_PRE: case T_XMP: case T_LISTING: case T_PLAINTEXT:
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_FORM: case T_HEADER: case T_FOOTER:
    case T_NAV: case T_SECTION: case T_ARTICLE: case T_ASIDE: case T_MAIN: case T_ADDRESS: case T_FIGURE:
    case T_FIGCAPTION: case T_FIELDSET: case T_LEGEND: case T_DETAILS: case T_SUMMARY: case T_HGROUP: case T_CAPTION:
    case T_BODY: case T_FRAMESET: case T_NOFRAMES: case T_TABLE: case T_TR: case T_TD: case T_TH: case T_THEAD:
    case T_TBODY: case T_TFOOT: return true;
    default: return false;
    }
}

static int align_attr(const struct html_node *n, int dflt) {
    const char *a = html_attr(n, "align");
    if (!a) return dflt;
    if (!strcasecmp(a, "center") || !strcasecmp(a, "middle")) return 1;
    if (!strcasecmp(a, "right")) return 2;
    if (!strcasecmp(a, "left")) return 0;
    return dflt;
}

static void flow_node(struct flow *F, struct html_node *n, const struct style *st_in);
static void flow_flat(struct flow *F, struct html_node *n, const struct style *st);
static void flow_children(struct flow *F, struct html_node *n, const struct style *st) {
    for (struct html_node *c = n->first; c; c = c->next) flow_node(F, c, st);
}

static void collect_text(struct html_node *n, char *out, size_t outsz) {
    size_t o = strlen(out);
    for (struct html_node *c = n->first; c && o < outsz - 1; c = c->next) {
        if (c->type == HN_TEXT) {
            for (int i = 0; i < c->text_len && o < outsz - 1; i++) {
                char ch = c->text[i];
                if (isspace(ch)) { if (o && out[o - 1] != ' ') out[o++] = ' '; }
                else out[o++] = ch;
            }
            out[o] = 0;
        } else if (!is_hidden_tag(c->tag)) { collect_text(c, out, outsz); o = strlen(out); }
    }
}

/* text of the n-th <option> of a select (or "") */
const char *select_option_text(struct html_node *sel, int index, char *out, size_t outsz, const char **value) {
    int idx = 0;
    out[0] = 0;
    if (value) *value = NULL;
    struct html_node *stack[16]; int sp = 0;
    struct html_node *c = sel->first;
    while (c || sp) {
        if (!c) { c = stack[--sp]->next; continue; }
        if (c->type == HN_ELEMENT && c->tag == T_OPTION) {
            if (idx++ == index) {
                collect_text(c, out, outsz);
                size_t l = strlen(out); while (l && out[l - 1] == ' ') out[--l] = 0;
                if (value) *value = html_attr(c, "value");
                return out;
            }
            c = c->next; continue;
        }
        if (c->type == HN_ELEMENT && c->first && sp < 16) { stack[sp++] = c; c = c->first; continue; }
        c = c->next;
    }
    return out;
}

static __attribute__((noinline)) void flow_field(struct flow *F, struct html_node *n, const struct style *st) {
    struct form_field *f = n->field;
    if (!f || f->kind == FF_HIDDEN) return;
    struct font *uf = font_ui;
    int h = 24, w;
    const char *sz = html_attr(n, "size");
    switch (f->kind) {
    case FF_CHECKBOX: case FF_RADIO: w = 14; h = 14; add_box(F, st, DI_FIELD, n, w, h, 1); return;
    case FF_SUBMIT: case FF_RESET: case FF_BUTTON: case FF_IMAGE: {
        char label[128] = "";
        if (n->tag == T_BUTTON) collect_text(n, label, sizeof label);
        else strlcpy(label, f->kind == FF_IMAGE ? (html_attr(n, "alt") ? html_attr(n, "alt") : "Submit") : f->value, sizeof label);
        if (n->tag == T_BUTTON) strlcpy(f->value, html_attr(n, "value") ? html_attr(n, "value") : "", sizeof f->value);
        w = font_text_width(uf, label) + 24;
        break; }
    case FF_SELECT: {
        /* one walk over the options (select_option_text per index would be quadratic) */
        int maxw = 40; char buf[128];
        struct html_node *stack[16]; int sp = 0;
        for (struct html_node *c = n->first; c || sp; ) {
            if (!c) { c = stack[--sp]->next; continue; }
            if (c->type == HN_ELEMENT && c->tag == T_OPTION) {
                buf[0] = 0; collect_text(c, buf, sizeof buf);
                size_t l = strlen(buf); while (l && buf[l - 1] == ' ') buf[--l] = 0;
                maxw = MAX(maxw, font_text_width(uf, buf));
                c = c->next; continue;
            }
            if (c->type == HN_ELEMENT && c->first && sp < 16) { stack[sp++] = c; c = c->first; continue; }
            c = c->next;
        }
        w = MIN(maxw, 400) + 34;
        break; }
    case FF_TEXTAREA: {
        int cols = html_attr(n, "cols") ? atoi(html_attr(n, "cols")) : 40, rows = html_attr(n, "rows") ? atoi(html_attr(n, "rows")) : 3;
        w = CLAMP(cols, 4, 120) * 7 + 16; h = CLAMP(rows, 1, 40) * 16 + 10;
        break; }
    default: w = CLAMP(sz ? atoi(sz) : 20, 2, 100) * 7 + 18; break;
    }
    w = MIN(w, MAX(40, F->right - F->left));
    add_box(F, st, DI_FIELD, n, w, h, 6);
}

#define MAX_IMG_SIDE 4096
static __attribute__((noinline)) void flow_image(struct flow *F, struct html_node *n, const struct style *st) {
    struct html_image *im = n->image;
    int avail = MAX(1, F->right - F->left);
    const char *wa = html_attr(n, "width"), *ha = html_attr(n, "height");
    int w = parse_length(wa, avail), h = parse_length(ha, avail);
    /* measuring (avail is a huge placeholder): percentages resolve later against the real width */
    bool pct_w = !F->emit && wa && strchr(wa, '%');
    if (pct_w) w = -1;
    if (!F->emit && ha && strchr(ha, '%')) h = -1;
    w = MIN(w, MAX_IMG_SIDE); h = MIN(h, MAX_IMG_SIDE);
    struct canvas *img = im ? im->img : NULL;
    if (img) {
        if (w <= 0 && h <= 0) { w = img->w; h = img->h; }
        else if (w <= 0) w = img->w * h / MAX(1, img->h);
        else if (h <= 0) h = img->h * w / MAX(1, img->w);
    } else if ((!im || im->failed || w <= 0 || h <= 0)) {
        /* nothing to show (yet): fall back to the alternative text */
        const char *alt = html_attr(n, "alt");
        if (alt && *alt && (!im || im->failed)) {
            struct style s2 = *st; s2.color = RGB(0x80, 0x80, 0x80);
            if (!s2.underline) s2.italic = true;
            flow_text(F, &s2, alt, strlen(alt));
        }
        return;
    }
    if (w <= 0 || h <= 0) return;
    if (w > avail) { h = (int)((int64_t)h * avail / w); w = avail; }
    w = CLAMP(w, 1, MAX_IMG_SIDE); h = CLAMP(h, 1, MAX_IMG_SIDE);
    int min_w = F->min_w;
    add_box(F, st, DI_IMAGE, n, w, h, 0);
    if (pct_w) F->min_w = min_w;   /* it shrinks with the cell */
}

/* ---- tables ---- */
struct tcell { struct html_node *node; int col, span; int minw, maxw, pct; int h; int first_item, last_item, bg_item; int valign; };
struct trow { struct html_node *node; int first_cell, ncells; int h; };
struct table {
    struct tcell *cells; int ncells, capc;
    struct trow *rows; int nrows, capr;
    int ncols;
};

static void table_add_row(struct table *t, struct html_node *tr) {
    if (t->nrows >= MAX_ROWS) return;
    if (t->nrows == t->capr) { t->capr = t->capr ? t->capr * 2 : 16; t->rows = krealloc(t->rows, t->capr * sizeof *t->rows); }
    struct trow *r = &t->rows[t->nrows++];
    r->node = tr; r->first_cell = t->ncells; r->ncells = 0; r->h = 0;
    int col = 0;
    for (struct html_node *c = tr->first; c; c = c->next) {
        if (c->type != HN_ELEMENT || (c->tag != T_TD && c->tag != T_TH)) continue;
        if (col >= MAX_COLS) break;
        if (t->ncells == t->capc) { t->capc = t->capc ? t->capc * 2 : 32; t->cells = krealloc(t->cells, t->capc * sizeof *t->cells); }
        struct tcell *cell = &t->cells[t->ncells++];
        memset(cell, 0, sizeof *cell);
        cell->node = c; cell->col = col;
        const char *cs = html_attr(c, "colspan");
        cell->span = CLAMP(cs ? atoi(cs) : 1, 1, MAX_COLS - col);
        col += cell->span;
        r->ncells++;
    }
    t->ncols = MAX(t->ncols, col);
}

static void table_collect(struct table *t, struct html_node *n, int depth) {
    for (struct html_node *c = n->first; c; c = c->next) {
        if (c->type != HN_ELEMENT) continue;
        if (c->tag == T_TR) table_add_row(t, c);
        else if ((c->tag == T_THEAD || c->tag == T_TBODY || c->tag == T_TFOOT || c->tag == T_FORM) && depth < 3) table_collect(t, c, depth + 1);
    }
}

struct tcols { int min[MAX_COLS], max[MAX_COLS], pct[MAX_COLS], w[MAX_COLS], x[MAX_COLS + 1]; };

static __attribute__((noinline)) void flow_table(struct flow *F, struct html_node *tn, const struct style *st) {
    struct lay *lay = F->lay;
    block_break(F, 0);
    F->y += F->margin; F->margin = 0;
    struct table t = { 0 };
    table_collect(&t, tn, 0);
    /* caption */
    for (struct html_node *c = tn->first; c; c = c->next)
        if (c->type == HN_ELEMENT && c->tag == T_CAPTION) {
            int sa = F->align; F->align = 1;
            flow_children(F, c, st); flush_line(F, false);
            F->align = sa;
        }
    if (!t.nrows || !t.ncols) { kfree(t.cells); kfree(t.rows); return; }

    const char *ba = html_attr(tn, "border");
    int border = ba ? (*ba ? CLAMP(atoi(ba), 0, 8) : 1) : 0;
    const char *csa = html_attr(tn, "cellspacing"), *cpa = html_attr(tn, "cellpadding");
    int spacing = csa ? CLAMP(atoi(csa), 0, 40) : 2;
    int pad = cpa ? CLAMP(atoi(cpa), 0, 40) : 1;
    int avail = MAX(20, F->right - F->left);
    int fixed_w = parse_length(html_attr(tn, "width"), avail);
    struct css tcss = { 0 };
    struct style tst = *st;
    apply_inline_css(html_attr(tn, "style"), &tst, &tcss);
    color_t tbg;
    bool has_tbg = html_parse_color(html_attr(tn, "bgcolor"), &tbg) || (tcss.has_bg && (tbg = tcss.bg, true));

    bool depth_ok = lay->depth < MAX_LAYOUT_DEPTH;
    /* measure every cell: minimum (longest unbreakable run) and preferred (unwrapped) widths */
    struct tcols *tc = kzalloc(sizeof *tc);
    int *colmin = tc->min, *colmax = tc->max, *colpct = tc->pct, *colw = tc->w, *colx = tc->x;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < t.ncells; i++) {
            struct tcell *c = &t.cells[i];
            if ((pass == 0) != (c->span == 1)) continue;
            if (pass == 0) {
                struct flow M = { 0 };
                M.lay = lay; M.origin = M.left = 0; M.right = 100000; M.emit = false; M.strut = F->strut;
                struct style cst = tst;
                if (c->node->tag == T_TH) cst.bold = true;
                if (html_attr(c->node, "nowrap")) cst.nowrap = true;
                if (depth_ok) { lay->depth++; flow_children(&M, c->node, &cst); lay->depth--; } else flow_flat(&M, c->node, &cst);
                flush_line(&M, false);
                kfree(M.frags);
                c->maxw = MAX(M.max_right, M.min_w) + 2 * pad;
                c->minw = M.min_w + 2 * pad;
                if (cst.nowrap) c->minw = c->maxw;
                const char *wa = html_attr(c->node, "width");
                if (wa && strchr(wa, '%')) c->pct = CLAMP(atoi(wa), 1, 100);
                else if (wa) { int px = parse_length(wa, avail); if (px > 0) { c->minw = MAX(c->minw, MIN(px, avail)); c->maxw = MAX(c->minw, px); } }
                colmin[c->col] = MAX(colmin[c->col], c->minw);
                colmax[c->col] = MAX(colmax[c->col], c->maxw);
                colpct[c->col] = MAX(colpct[c->col], c->pct);
            } else {
                /* spanning cells: grow the spanned columns evenly if they are too narrow */
                struct flow M = { 0 };
                M.lay = lay; M.right = 100000; M.emit = false; M.strut = F->strut;
                struct style cst = tst;
                if (depth_ok) { lay->depth++; flow_children(&M, c->node, &cst); lay->depth--; } else flow_flat(&M, c->node, &cst);
                flush_line(&M, false);
                kfree(M.frags);
                int mn = M.min_w + 2 * pad, mx = MAX(M.max_right, M.min_w) + 2 * pad;
                int smin = 0, smax = 0;
                for (int k = c->col; k < c->col + c->span; k++) { smin += colmin[k]; smax += colmax[k]; }
                smin += (c->span - 1) * spacing; smax += (c->span - 1) * spacing;
                if (mn > smin) { int d = (mn - smin) / c->span + 1; for (int k = c->col; k < c->col + c->span; k++) colmin[k] += d; }
                if (mx > smax) { int d = (mx - smax) / c->span + 1; for (int k = c->col; k < c->col + c->span; k++) colmax[k] += d; }
            }
        }
    }
    int extra = (t.ncols + 1) * spacing + 2 * border;
    int summin = 0, summax = 0;
    for (int k = 0; k < t.ncols; k++) { colmax[k] = MAX(colmax[k], colmin[k]); summin += colmin[k]; summax += colmax[k]; }
    int target;
    if (fixed_w > 0) target = MAX(fixed_w - extra, summin);
    else target = MIN(summax, avail - extra);
    target = MAX(target, summin);
    if (target >= summax) {
        /* spare room (fixed-width table): percentage columns first, then proportionally */
        int spare = target - summax, weight = 0;
        for (int k = 0; k < t.ncols; k++) { colw[k] = colmax[k]; weight += colmax[k]; }
        int pct_total = 0;
        for (int k = 0; k < t.ncols; k++) if (colpct[k]) { int want = target * colpct[k] / 100; if (want > colw[k]) { int d = MIN(want - colw[k], spare); colw[k] += d; spare -= d; } pct_total++; }
        if (spare > 0) {
            int nonpct = 0; for (int k = 0; k < t.ncols; k++) if (!colpct[k]) nonpct++;
            for (int k = 0; k < t.ncols && spare > 0; k++) {
                if (pct_total && nonpct && colpct[k]) continue;
                int d = weight ? (int)((int64_t)spare * colmax[k] / MAX(1, weight)) : spare / t.ncols;
                if (nonpct && pct_total) d = spare / nonpct;
                colw[k] += d;
            }
        }
    } else {
        int range = summax - summin;
        for (int k = 0; k < t.ncols; k++)
            colw[k] = colmin[k] + (range ? (int)((int64_t)(colmax[k] - colmin[k]) * (target - summin) / range) : 0);
    }
    int tw = extra;
    for (int k = 0; k < t.ncols; k++) tw += colw[k];
    int talign = align_attr(tn, F->align == 1 ? 1 : 0);
    int tx = F->left + (talign == 1 ? MAX(0, (avail - tw) / 2) : talign == 2 ? MAX(0, avail - tw) : 0);
    colx[0] = tx + border + spacing;
    for (int k = 0; k < t.ncols; k++) colx[k + 1] = colx[k] + colw[k] + spacing;
    if (!F->emit) {
        /* measuring an enclosing table: the widths are all it needs, and laying the
           cells out here too would make nested tables cost 2^depth */
        F->max_right = MAX(F->max_right, tx + tw);
        note_min(F, summin + extra);
        kfree(tc); kfree(t.cells); kfree(t.rows);
        return;
    }

    int ty = F->y;
    int tbg_item = -1;
    if (has_tbg) { struct display_item it = { .type = DI_RECT, .x = tx, .y = ty, .w = tw, .h = 0, .color = tbg, .link = -1 }; tbg_item = add_item(F, &it); }
    int y = ty + border + spacing;
    for (int r = 0; r < t.nrows; r++) {
        struct trow *row = &t.rows[r];
        struct css rcss = { 0 };
        struct style rst = tst;
        apply_inline_css(html_attr(row->node, "style"), &rst, &rcss);
        color_t rbg; bool has_rbg = html_parse_color(html_attr(row->node, "bgcolor"), &rbg) || (rcss.has_bg && (rbg = rcss.bg, true));
        int ralign = align_attr(row->node, -1);
        const char *rva = html_attr(row->node, "valign");
        int row_h = 0;
        for (int i = 0; i < row->ncells; i++) {
            struct tcell *c = &t.cells[row->first_cell + i];
            int x = colx[c->col], cw = colx[c->col + c->span] - spacing - x;
            struct css ccss = { 0 };
            struct style cst = rst;
            if (c->node->tag == T_TH) cst.bold = true;
            if (html_attr(c->node, "nowrap")) cst.nowrap = true;
            apply_inline_css(html_attr(c->node, "style"), &cst, &ccss);
            color_t cbg; bool has_cbg = html_parse_color(html_attr(c->node, "bgcolor"), &cbg) || (ccss.has_bg && (cbg = ccss.bg, true));
            if (!has_cbg && has_rbg) { cbg = rbg; has_cbg = true; }
            c->bg_item = -1;
            if (has_cbg) { struct display_item it = { .type = DI_RECT, .x = x, .y = y, .w = cw, .h = 0, .color = cbg, .link = -1 }; c->bg_item = add_item(F, &it); }
            const char *va = html_attr(c->node, "valign"); if (!va) va = rva;
            c->valign = !va ? 1 : !strcasecmp(va, "top") ? 0 : !strcasecmp(va, "bottom") ? 2 : 1;
            struct flow C = { 0 };
            C.lay = lay; C.origin = C.left = x + pad; C.right = x + cw - pad; C.y = y + pad; C.emit = F->emit; C.strut = F->strut;
            C.align = align_attr(c->node, ralign >= 0 ? ralign : c->node->tag == T_TH ? 1 : 0);
            if (ccss.align) C.align = ccss.align;
            c->first_item = lay->L->nitems;
            if (depth_ok) { lay->depth++; flow_children(&C, c->node, &cst); lay->depth--; }
            else flow_flat(&C, c->node, &cst);
            flush_line(&C, false);
            kfree(C.frags);
            c->last_item = lay->L->nitems;
            c->h = C.y - y + pad;
            if (html_attr(c->node, "height")) c->h = MAX(c->h, parse_length(html_attr(c->node, "height"), 10000));
            row_h = MAX(row_h, c->h);
            F->max_right = MAX(F->max_right, C.max_right + pad);
        }
        if (html_attr(row->node, "height")) row_h = MAX(row_h, parse_length(html_attr(row->node, "height"), 10000));
        for (int i = 0; i < row->ncells && F->emit; i++) {
            struct tcell *c = &t.cells[row->first_cell + i];
            int dy = c->valign == 1 ? (row_h - c->h) / 2 : c->valign == 2 ? row_h - c->h : 0;
            for (int k = c->first_item; k < c->last_item; k++) if (k != c->bg_item) lay->L->items[k].y += dy;
            if (c->bg_item >= 0) lay->L->items[c->bg_item].h = row_h;
            if (border) {
                int x = colx[c->col], cw = colx[c->col + c->span] - spacing - x;
                struct display_item it = { .type = DI_BORDER, .x = x, .y = y, .w = cw, .h = row_h, .color = RGB(0xA0, 0xA0, 0xA0), .link = -1 };
                add_item(F, &it);
            }
        }
        y += row_h + spacing;
    }
    int th = y - ty + border;
    if (tbg_item >= 0) lay->L->items[tbg_item].h = th;
    if (border) {
        for (int b = 0; b < border; b++) {
            struct display_item it = { .type = DI_BORDER, .x = tx + b, .y = ty + b, .w = tw - 2 * b, .h = th - 2 * b, .color = RGB(0x80, 0x80, 0x80), .link = -1 };
            add_item(F, &it);
        }
    }
    F->y = ty + th;
    F->max_right = MAX(F->max_right, tx + tw);
    note_min(F, summin + extra);
    kfree(tc); kfree(t.cells); kfree(t.rows);
}

/* ---- blocks ---- */
static void list_marker(struct flow *F, struct html_node *li, const struct style *st) {
    if (F->has_marker) flush_line(F, false);   /* an earlier marker still waiting for text */
    struct html_node *list = li->parent;
    bool ordered = list && list->tag == T_OL;
    const char *type = html_attr(li, "type");
    if (!type && list) type = html_attr(list, "type");
    if (ordered) {
        int start = list && html_attr(list, "start") ? atoi(html_attr(list, "start")) : 1;
        int idx = start;
        for (struct html_node *s = list->first; s && s != li; s = s->next) if (s->type == HN_ELEMENT && s->tag == T_LI) idx++;
        if (html_attr(li, "value")) idx = atoi(html_attr(li, "value"));
        if (type && (*type == 'a' || *type == 'A') && idx >= 1 && idx <= 26) snprintf(F->marker, sizeof F->marker, "%c.", (*type == 'a' ? 'a' : 'A') + idx - 1);
        else if (type && (*type == 'i' || *type == 'I') && idx >= 1 && idx < 40) {
            static const char *rl[] = { "", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix" };
            static const char *rt[] = { "", "x", "xx", "xxx" };
            char r[16]; snprintf(r, sizeof r, "%s%s", rt[idx / 10], rl[idx % 10]);
            if (*type == 'I') for (char *p = r; *p; p++) *p = toupper(*p);
            snprintf(F->marker, sizeof F->marker, "%s.", r);
        } else snprintf(F->marker, sizeof F->marker, "%d.", idx);
    } else {
        if (type && !strcasecmp(type, "none")) return;
        const char *m = st->list_depth <= 1 ? "•" : st->list_depth == 2 ? "◦" : "▪";
        if (type && !strcasecmp(type, "circle")) m = "◦";
        if (type && !strcasecmp(type, "square")) m = "▪";
        strlcpy(F->marker, m, sizeof F->marker);
        /* fall back to glyphs the UI fonts have */
        if (!strcmp(m, "◦")) strlcpy(F->marker, "○", sizeof F->marker);
        if (!strcmp(m, "▪")) strlcpy(F->marker, "–", sizeof F->marker);
    }
    F->marker_font = style_font(st);
    F->marker_color = st->color;
    F->has_marker = true;
}

/* <a>: link style and named anchor (kept out of flow_node's recursive frame) */
static __attribute__((noinline)) void flow_anchor(struct flow *F, struct html_node *n, struct style *st) {
    struct html_doc *doc = F->lay->doc;
    const char *href = html_attr(n, "href");
    if (href && *href && !starts_with_ci(href, "javascript:")) {
        char abs[1024];
        url_resolve(doc->base, href, abs, sizeof abs);
        st->link = add_link(F->lay->L, abs, n);
        st->color = doc->link;
        st->underline = true;
    }
    const char *name = html_attr(n, "name");
    if (name) add_anchor(F, name);
}

static __attribute__((noinline)) void flow_frame(struct flow *F, struct html_node *n, const struct style *st) {
    struct lay *lay = F->lay;
    const char *src = html_attr(n, "src");
    if (!src || !*src) return;
    char abs[1024], label[1100];
    url_resolve(lay->doc->base, src, abs, sizeof abs);
    const char *nm = html_attr(n, "name");
    snprintf(label, sizeof label, "[%s: %s]", n->tag == T_FRAME ? "frame" : "embedded page", nm && *nm ? nm : src);
    if (n->tag == T_FRAME) block_break(F, 4);
    struct style ls = *st; ls.link = add_link(lay->L, abs, n); ls.color = lay->doc->link; ls.underline = true;
    const char *keep = keep_string(lay->L, label);
    flow_text(F, &ls, keep, strlen(keep));
    if (n->tag == T_FRAME) block_break(F, 4);
}

/* content nested deeper than MAX_LAYOUT_DEPTH: flow its text without recursing */
static __attribute__((noinline)) void flow_flat(struct flow *F, struct html_node *n, const struct style *st) {
    if (n->tag == T_BR) { flush_line(F, false); return; }
    bool blk = is_block_tag(n->tag);
    if (blk) block_break(F, 0);
    struct html_node *c = n->first;
    while (c) {
        if (c->type == HN_TEXT) { if (st->pre) flow_pre(F, st, c->text, c->text_len); else flow_text(F, st, c->text, c->text_len); }
        else if (c->tag == T_BR) flush_line(F, false);
        else if (!is_hidden_tag(c->tag) && !html_attr(c, "hidden")) {
            if (is_block_tag(c->tag)) block_break(F, 0);
            if (c->first) { c = c->first; continue; }
        }
        while (c != n && !c->next) { c = c->parent; if (c != n && is_block_tag(c->tag)) block_break(F, 0); }
        if (c == n) break;
        c = c->next;
    }
    if (blk) block_break(F, 0);
}

static void flow_node(struct flow *F, struct html_node *n, const struct style *st_in) {
    struct lay *lay = F->lay;
    if (n->type == HN_TEXT) {
        if (st_in->pre) flow_pre(F, st_in, n->text, n->text_len);
        else flow_text(F, st_in, n->text, n->text_len);
        return;
    }
    int tag = n->tag;
    if (is_hidden_tag(tag) || html_attr(n, "hidden")) return;
    if (lay->depth >= MAX_LAYOUT_DEPTH) { flow_flat(F, n, st_in); return; }
    struct style st = *st_in;
    struct css css = { 0 };
    switch (tag) {
    case T_B: case T_STRONG: case T_DT: st.bold = true; break;
    case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN: case T_ADDRESS: st.italic = true; break;
    case T_U: case T_INS: st.underline = true; break;
    case T_S: case T_STRIKE: case T_DEL: st.strike = true; break;
    case T_CODE: case T_TT: case T_KBD: case T_SAMP: st.family = 1; break;
    case T_PRE: case T_XMP: case T_LISTING: case T_PLAINTEXT: st.family = 1; st.pre = true; break;
    case T_SMALL: case T_SUB: case T_SUP: st.size = MAX(0, st.size - 1); break;
    case T_BIG: st.size = MIN(5, st.size + 1); break;
    case T_NOBR: st.nowrap = true; break;
    case T_H1: st.bold = true; st.size = 5; break;
    case T_H2: st.bold = true; st.size = 4; break;
    case T_H3: st.bold = true; st.size = 3; break;
    case T_H4: st.bold = true; st.size = 2; break;
    case T_H5: st.bold = true; st.size = 1; break;
    case T_H6: st.bold = true; st.size = 0; break;
    case T_TH: st.bold = true; break;
    case T_LEGEND: case T_SUMMARY: st.bold = true; break;
    case T_FONT: {
        html_parse_color(html_attr(n, "color"), &st.color);
        st.size = font_size_attr(html_attr(n, "size"), st.size);
        const char *face = html_attr(n, "face");
        if (face && (strstr(face, "ourier") || strstr(face, "ono"))) st.family = 1;
        break; }
    case T_A: flow_anchor(F, n, &st); break;
    }
    apply_inline_css(html_attr(n, "style"), &st, &css);
    if (css.hidden) return;
    const char *id = html_attr(n, "id");
    if (id) add_anchor(F, id);

    switch (tag) {
    case T_BR:
        if (!F->nfrags) { F->strut = style_font(&st); flush_line(F, true); }
        else flush_line(F, false);
        return;
    case T_IMG: flow_image(F, n, &st); return;
    case T_INPUT: case T_SELECT: case T_TEXTAREA: case T_BUTTON: flow_field(F, n, &st); return;
    case T_HR: {
        block_break(F, 8);
        F->y += F->margin; F->margin = 0;
        int avail = F->right - F->left;
        int w = parse_length(html_attr(n, "width"), avail);
        if (w <= 0 || w > avail) w = avail;
        int x = F->left + (align_attr(n, 1) == 1 ? (avail - w) / 2 : align_attr(n, 1) == 2 ? avail - w : 0);
        struct display_item it = { .type = DI_HLINE, .x = x, .y = F->y, .w = w, .h = 2, .link = -1 };
        add_item(F, &it);
        F->y += 2;
        F->margin = 8;
        return; }
    case T_TABLE: lay->depth++; flow_table(F, n, &st); lay->depth--; return;
    case T_FRAME: case T_IFRAME: flow_frame(F, n, &st); return;
    }

    if (!is_block_tag(tag)) {
        lay->depth++;
        flow_children(F, n, &st);
        lay->depth--;
        return;
    }

    /* block element */
    int mt = 0, mb = 0, indent_l = 0, indent_r = 0;
    switch (tag) {
    case T_P: mt = mb = 12; break;
    case T_H1: mt = 18; mb = 12; break;
    case T_H2: mt = 16; mb = 10; break;
    case T_H3: mt = 14; mb = 8; break;
    case T_H4: case T_H5: case T_H6: mt = 12; mb = 8; break;
    case T_UL: case T_OL: case T_MENU: case T_DIR:
        if (st.list_depth == 0) mt = mb = 12;
        indent_l = 30; st.list_depth++;
        break;
    case T_DL: if (st.list_depth == 0) mt = mb = 12; break;
    case T_DD: indent_l = 32; break;
    case T_BLOCKQUOTE: mt = mb = 12; indent_l = 32; indent_r = 24; break;
    case T_FIGURE: mt = mb = 12; indent_l = indent_r = 32; break;
    case T_PRE: case T_XMP: case T_LISTING: case T_PLAINTEXT: mt = mb = 12; break;
    case T_FIELDSET: mt = mb = 8; indent_l = indent_r = 10; break;
    case T_DETAILS: case T_SUMMARY: mt = mb = 4; break;
    case T_ADDRESS: break;
    case T_FORM: mb = 0; break;
    }
    if (st.list_depth > 0 && (tag == T_UL || tag == T_OL) && st.list_depth > 1) mt = mb = 0;
    /* <li><p>…: keep a pending marker for the block's first line instead of a line of its own */
    bool keep_marker = F->has_marker && !F->nfrags && !indent_l;
    if (keep_marker) F->has_marker = false;
    block_break(F, mt);
    if (keep_marker) F->has_marker = true;
    int save_l = F->left, save_r = F->right, save_align = F->align;
    struct font *save_strut = F->strut;
    F->left += indent_l; F->right -= indent_r;
    if (F->right - F->left < 40) F->right = F->left + 40;
    F->strut = style_font(&st);
    if (tag == T_CENTER) F->align = 1;
    F->align = align_attr(n, F->align);
    if (css.align) F->align = css.align;
    int bg_item = -1;
    if (!css.has_bg && tag != T_BODY && html_parse_color(html_attr(n, "bgcolor"), &css.bg)) css.has_bg = true;
    if (css.has_bg) {
        struct display_item it = { .type = DI_RECT, .x = F->left, .y = F->y + F->margin, .w = F->right - F->left, .h = 0, .color = css.bg, .link = -1 };
        bg_item = add_item(F, &it);
    }
    int y0 = F->y + F->margin;
    if (tag == T_LI) list_marker(F, n, &st);
    lay->depth++;
    flow_children(F, n, &st);
    lay->depth--;
    flush_line(F, false);
    if (bg_item >= 0) lay->L->items[bg_item].h = F->y - y0;
    F->left = save_l; F->right = save_r; F->align = save_align; F->strut = save_strut;
    F->margin = MAX(F->margin, mb);
}

/* ---- public API ---- */
struct layout *html_layout(struct html_doc *doc, int width) {
    load_fonts();
    struct layout *L = kzalloc(sizeof *L);
    L->bg = doc->bg; L->link_color = doc->link; L->vlink_color = doc->vlink;
    struct lay lay = { .L = L, .doc = doc };
    struct flow F = { 0 };
    F.lay = &lay; F.emit = true;
    F.origin = F.left = PAGE_MARGIN; F.right = MAX(width - PAGE_MARGIN, PAGE_MARGIN + 100);
    F.y = PAGE_MARGIN;
    struct style st = { 0 };
    st.size = 2; st.color = doc->text; st.link = -1;
    F.strut = style_font(&st);
    flow_children(&F, doc->root, &st);
    flush_line(&F, false);
    kfree(F.frags);
    L->height = F.y + PAGE_MARGIN;
    L->width = MAX(width, F.max_right + PAGE_MARGIN);
    return L;
}

void layout_free(struct layout *L) {
    if (!L) return;
    for (int i = 0; i < L->nlinks; i++) kfree(L->links[i]);
    for (int i = 0; i < L->nanchors; i++) kfree(L->anchors[i].name);
    for (int i = 0; i < L->nstrs; i++) kfree(L->strs[i]);
    kfree(L->strs);
    kfree(L->links); kfree(L->link_nodes); kfree(L->anchors); kfree(L->items); kfree(L);
}

int layout_link_at(struct layout *L, int x, int y) {
    for (int i = L->nitems - 1; i >= 0; i--) {
        struct display_item *it = &L->items[i];
        if (it->link < 0) continue;
        int w = it->type == DI_TEXT ? MAX(it->w, it->ul_w) : it->w;
        if (x >= it->x && x < it->x + w && y >= it->y - 2 && y < it->y + it->h + 2) return it->link;
    }
    return -1;
}

struct display_item *layout_field_at(struct layout *L, int x, int y) {
    for (int i = L->nitems - 1; i >= 0; i--) {
        struct display_item *it = &L->items[i];
        if (it->type == DI_FIELD && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it;
    }
    return NULL;
}

int layout_anchor_y(struct layout *L, const char *name) {
    for (int i = 0; i < L->nanchors; i++) if (!strcmp(L->anchors[i].name, name)) return L->anchors[i].y;
    return -1;
}

static void paint_field(struct canvas *c, struct display_item *it, int y, struct form_field *focus) {
    struct form_field *f = it->node->field;
    if (!f) return;
    struct rect r = rect_make(it->x, y, it->w, it->h);
    color_t border = RGB(0x9A, 0x9A, 0xA0), fill = RGB(255, 255, 255), text = RGB(0x1D, 0x1D, 0x1F);
    bool focused = f == focus;
    switch (f->kind) {
    case FF_CHECKBOX:
        gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 3, f->checked ? theme.accent : fill);
        gfx_draw_rounded_rect(c, r.x, r.y, r.w, r.h, 3, f->checked ? theme.accent : border);
        if (f->checked) { gfx_line(c, r.x + 3, r.y + 7, r.x + 6, r.y + 10, RGB(255, 255, 255)); gfx_line(c, r.x + 6, r.y + 10, r.x + 11, r.y + 3, RGB(255, 255, 255)); gfx_line(c, r.x + 3, r.y + 8, r.x + 6, r.y + 11, RGB(255, 255, 255)); }
        return;
    case FF_RADIO:
        gfx_fill_circle(c, r.x + 7, r.y + 7, 7, f->checked ? theme.accent : border);
        gfx_fill_circle(c, r.x + 7, r.y + 7, 6, f->checked ? theme.accent : fill);
        if (f->checked) gfx_fill_circle(c, r.x + 7, r.y + 7, 3, RGB(255, 255, 255));
        return;
    case FF_SUBMIT: case FF_RESET: case FF_BUTTON: case FF_IMAGE: {
        char label[128] = "";
        if (it->node->tag == T_BUTTON) collect_text(it->node, label, sizeof label);
        else strlcpy(label, f->kind == FF_IMAGE ? (html_attr(it->node, "alt") ? html_attr(it->node, "alt") : "Submit") : f->value, sizeof label);
        gfx_vgradient(c, r.x, r.y, r.w, r.h, RGB(0xFB, 0xFB, 0xFC), RGB(0xEC, 0xEC, 0xEF));
        gfx_draw_rounded_rect(c, r.x, r.y, r.w, r.h, 5, border);
        font_draw_centered(c, font_ui, r, text, label);
        return; }
    case FF_SELECT: {
        char buf[128];
        select_option_text(it->node, f->selected, buf, sizeof buf, NULL);
        gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 5, fill);
        gfx_draw_rounded_rect(c, r.x, r.y, r.w, r.h, 5, border);
        font_draw_ellipsized(c, font_ui, r.x + 8, r.y + (r.h - font_height(font_ui)) / 2, r.w - 34, text, buf);
        font_draw_aligned(c, font_ui_small, rect_make(r.x, r.y, r.w - 8, r.h), RGB(0x60, 0x60, 0x66), "▲▼", 2);
        return; }
    default: {
        gfx_fill_rect(c, r.x, r.y, r.w, r.h, fill);
        gfx_draw_rect(c, r.x, r.y, r.w, r.h, focused ? theme.accent : border);
        if (focused) gfx_draw_rect(c, r.x - 1, r.y - 1, r.w + 2, r.h + 2, color_with_alpha(theme.accent, 0x70));
        struct canvas sub = canvas_sub(c, rect_make(r.x + 5, r.y + 2, r.w - 10, r.h - 4));
        char shown[512];
        if (f->kind == FF_PASSWORD) { size_t l = 0; for (const char *p = f->value; *p && l < sizeof shown - 1; p++) if ((*p & 0xC0) != 0x80) shown[l++] = '*'; shown[l] = 0; }
        else strlcpy(shown, f->value, sizeof shown);
        const char *ph = html_attr(it->node, "placeholder");
        int tw = font_text_width(font_ui, shown);
        int sx = tw > sub.w - 4 && focused ? sub.w - 4 - tw : 0;
        if (f->kind == FF_TEXTAREA) {
            /* wrap lines naively at the box width */
            int ly = 2; const char *p = shown;
            while (*p && ly < sub.h) {
                const char *nl = strchr(p, '\n');
                int len = nl ? (int)(nl - p) : (int)strlen(p);
                int fit = font_fit_chars(font_ui, p, sub.w - 2);
                if (fit < len && fit > 0) len = fit;
                font_draw_n(&sub, font_ui, 0, ly, text, p, len);
                p += len; if (*p == '\n') p++;
                ly += 16;
            }
        } else if (!shown[0] && ph) font_draw(&sub, font_ui, 0, (sub.h - font_height(font_ui)) / 2, RGB(0x9A, 0x9A, 0xA0), ph);
        else font_draw(&sub, font_ui, sx, (sub.h - font_height(font_ui)) / 2, text, shown);
        if (focused && ((system_ticks / 500) & 1) == 0 && f->kind != FF_TEXTAREA) gfx_fill_rect(&sub, sx + tw + 1, 3, 1, sub.h - 6, text);
        return; }
    }
}

void layout_paint(struct layout *L, struct canvas *c, int scroll_y, struct form_field *focus, bool (*visited)(void *ctx, const char *url), void *ctx) {
    gfx_fill(c, L->bg);
    int top = scroll_y, bottom = scroll_y + c->h;
    for (int i = 0; i < L->nitems; i++) {
        struct display_item *it = &L->items[i];
        if (it->y + it->h < top || it->y > bottom) continue;
        int y = it->y - scroll_y;
        switch (it->type) {
        case DI_TEXT: {
            color_t col = it->color;
            if (it->link >= 0 && col == L->link_color && visited && visited(ctx, L->links[it->link])) col = L->vlink_color;
            if (it->flags & DIF_ITALIC) font_draw_oblique_n(c, it->font, it->x, y, col, it->text, it->len);
            else font_draw_n(c, it->font, it->x, y, col, it->text, it->len);
            int base = y + font_ascent(it->font);
            if (it->flags & DIF_UNDERLINE) gfx_hline(c, it->x, base + 2, it->ul_w, col);
            if (it->flags & DIF_STRIKE) gfx_hline(c, it->x, base - font_ascent(it->font) * 3 / 10, it->w, col);
            break; }
        case DI_RECT: gfx_fill_rect(c, it->x, y, it->w, it->h, it->color); break;
        case DI_BORDER: gfx_draw_rect(c, it->x, y, it->w, it->h, it->color); break;
        case DI_HLINE: gfx_hline(c, it->x, y, it->w, RGB(0xB8, 0xB8, 0xBC)); gfx_hline(c, it->x, y + 1, it->w, RGB(0xEE, 0xEE, 0xF0)); break;
        case DI_IMAGE: {
            struct html_image *im = it->node->image;
            if (!im || !im->img) { gfx_draw_rect(c, it->x, y, it->w, it->h, RGB(0xC8, 0xC8, 0xCC)); break; }
            if (im->img->w == it->w && im->img->h == it->h) { gfx_blit(c, it->x, y, im->img); }
            else if ((int64_t)it->w * it->h > (4 << 20)) {
                /* too big to cache a scaled copy: scale the visible part on every paint */
                canvas_destroy(im->scaled); im->scaled = NULL;
                gfx_blit_scaled(c, it->x, y, it->w, it->h, im->img);
            } else {
                if (!im->scaled || im->scaled->w != it->w || im->scaled->h != it->h) {
                    canvas_destroy(im->scaled);
                    im->scaled = canvas_create(it->w, it->h);
                    gfx_blit_scaled(im->scaled, 0, 0, it->w, it->h, im->img);
                }
                gfx_blit(c, it->x, y, im->scaled);
            }
            if (it->link >= 0) gfx_draw_rect(c, it->x, y, it->w, it->h, color_with_alpha(L->link_color, 0x60));
            break; }
        case DI_FIELD: paint_field(c, it, y, focus); break;
        }
    }
}
