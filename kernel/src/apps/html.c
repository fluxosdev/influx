/*
 * HTML parser for the web browser: a tolerant tokenizer and tree builder.
 * It knows the implied end tags that real pages depend on (p, li, dt/dd,
 * tr/td/th, option, headings), skips scripts and style sheets, decodes
 * character references and converts legacy windows-1252 text to UTF-8.
 */
#include "html.h"
#include "mm.h"
#include "string.h"
#include "net.h"

static const char *tag_names[T_COUNT] = {
    [T_UNKNOWN] = "", [T_HTML] = "html", [T_HEAD] = "head", [T_BODY] = "body", [T_TITLE] = "title", [T_META] = "meta",
    [T_LINK] = "link", [T_SCRIPT] = "script", [T_STYLE] = "style", [T_BASE] = "base", [T_P] = "p", [T_DIV] = "div",
    [T_SPAN] = "span", [T_A] = "a", [T_B] = "b", [T_STRONG] = "strong", [T_I] = "i", [T_EM] = "em", [T_U] = "u",
    [T_S] = "s", [T_STRIKE] = "strike", [T_DEL] = "del", [T_INS] = "ins", [T_CODE] = "code", [T_TT] = "tt",
    [T_KBD] = "kbd", [T_SAMP] = "samp", [T_VAR] = "var", [T_PRE] = "pre", [T_XMP] = "xmp", [T_LISTING] = "listing",
    [T_PLAINTEXT] = "plaintext", [T_BR] = "br", [T_HR] = "hr", [T_H1] = "h1", [T_H2] = "h2", [T_H3] = "h3",
    [T_H4] = "h4", [T_H5] = "h5", [T_H6] = "h6", [T_UL] = "ul", [T_OL] = "ol", [T_LI] = "li", [T_DL] = "dl",
    [T_DT] = "dt", [T_DD] = "dd", [T_MENU] = "menu", [T_DIR] = "dir", [T_BLOCKQUOTE] = "blockquote",
    [T_CENTER] = "center", [T_TABLE] = "table", [T_CAPTION] = "caption", [T_THEAD] = "thead", [T_TBODY] = "tbody",
    [T_TFOOT] = "tfoot", [T_TR] = "tr", [T_TD] = "td", [T_TH] = "th", [T_COL] = "col", [T_COLGROUP] = "colgroup",
    [T_IMG] = "img", [T_FORM] = "form", [T_INPUT] = "input", [T_BUTTON] = "button", [T_SELECT] = "select",
    [T_OPTION] = "option", [T_OPTGROUP] = "optgroup", [T_TEXTAREA] = "textarea", [T_LABEL] = "label",
    [T_FIELDSET] = "fieldset", [T_LEGEND] = "legend", [T_FONT] = "font", [T_SMALL] = "small", [T_BIG] = "big",
    [T_SUB] = "sub", [T_SUP] = "sup", [T_HEADER] = "header", [T_FOOTER] = "footer", [T_NAV] = "nav",
    [T_SECTION] = "section", [T_ARTICLE] = "article", [T_ASIDE] = "aside", [T_MAIN] = "main", [T_ADDRESS] = "address",
    [T_FIGURE] = "figure", [T_FIGCAPTION] = "figcaption", [T_NOSCRIPT] = "noscript", [T_IFRAME] = "iframe",
    [T_SVG] = "svg", [T_MATH] = "math", [T_NOBR] = "nobr", [T_WBR] = "wbr", [T_ABBR] = "abbr", [T_ACRONYM] = "acronym",
    [T_CITE] = "cite", [T_Q] = "q", [T_MARK] = "mark", [T_DFN] = "dfn", [T_DETAILS] = "details", [T_SUMMARY] = "summary",
    [T_AREA] = "area", [T_MAP] = "map", [T_VIDEO] = "video", [T_AUDIO] = "audio", [T_SOURCE] = "source",
    [T_TRACK] = "track", [T_CANVAS] = "canvas", [T_OBJECT] = "object", [T_EMBED] = "embed", [T_PARAM] = "param",
    [T_TEMPLATE] = "template", [T_HGROUP] = "hgroup", [T_DIALOG] = "dialog", [T_PICTURE] = "picture",
    [T_TIME] = "time", [T_DATA] = "data", [T_OUTPUT] = "output", [T_METER] = "meter", [T_PROGRESS] = "progress",
    [T_BLINK] = "blink", [T_MARQUEE] = "marquee", [T_FRAMESET] = "frameset", [T_FRAME] = "frame",
    [T_NOFRAMES] = "noframes",
};

static int tag_lookup(const char *name, size_t n) {
    for (int i = 1; i < T_COUNT; i++) {
        const char *t = tag_names[i];
        if (t && strlen(t) == n) {
            size_t k = 0;
            while (k < n && tolower(name[k]) == t[k]) k++;
            if (k == n) return i;
        }
    }
    return T_UNKNOWN;
}

static bool is_void(int t) {
    switch (t) {
    case T_BR: case T_HR: case T_IMG: case T_INPUT: case T_META: case T_LINK: case T_BASE: case T_COL: case T_AREA:
    case T_WBR: case T_SOURCE: case T_TRACK: case T_PARAM: case T_EMBED: case T_FRAME: return true;
    default: return false;
    }
}
/* start tags that implicitly close an open <p> */
static bool closes_p(int t) {
    switch (t) {
    case T_P: case T_DIV: case T_UL: case T_OL: case T_DL: case T_PRE: case T_XMP: case T_LISTING: case T_TABLE:
    case T_BLOCKQUOTE: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_HR: case T_FORM:
    case T_CENTER: case T_HEADER: case T_FOOTER: case T_NAV: case T_SECTION: case T_ARTICLE: case T_ASIDE:
    case T_MAIN: case T_ADDRESS: case T_FIGURE: case T_FIELDSET: case T_MENU: case T_DIR: case T_DETAILS:
    case T_PLAINTEXT: case T_HGROUP: case T_DIALOG: case T_LI: case T_DT: case T_DD: return true;
    default: return false;
    }
}

/* ---- character references ---- */
static const struct { const char *name; uint32_t cp; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "sbquo", 0x201A }, { "bdquo", 0x201E }, { "bull", 0x2022 },
    { "middot", 0xB7 }, { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "times", 0xD7 }, { "divide", 0xF7 },
    { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 }, { "para", 0xB6 },
    { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "shy", 0xAD }, { "frac12", 0xBD }, { "frac14", 0xBC }, { "frac34", 0xBE },
    { "sup1", 0xB9 }, { "sup2", 0xB2 }, { "sup3", 0xB3 }, { "micro", 0xB5 }, { "ordf", 0xAA }, { "ordm", 0xBA },
    { "not", 0xAC }, { "macr", 0xAF }, { "acute", 0xB4 }, { "cedil", 0xB8 }, { "uml", 0xA8 }, { "curren", 0xA4 },
    { "brvbar", 0xA6 }, { "larr", 0x2190 }, { "uarr", 0x2191 }, { "rarr", 0x2192 }, { "darr", 0x2193 },
    { "harr", 0x2194 }, { "minus", 0x2212 }, { "ne", 0x2260 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "pi", 0x3C0 },
    { "radic", 0x221A }, { "infin", 0x221E }, { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "permil", 0x2030 },
    { "lsaquo", 0x2039 }, { "rsaquo", 0x203A }, { "prime", 0x2032 }, { "ensp", ' ' }, { "emsp", ' ' }, { "thinsp", ' ' },
    { "zwnj", 0 }, { "zwj", 0 }, { "lrm", 0 }, { "rlm", 0 }, { "hearts", 0x2665 }, { "check", 0x2713 }, { "star", 0x2606 },
    { "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Acirc", 0xC2 }, { "Atilde", 0xC3 }, { "Auml", 0xC4 }, { "Aring", 0xC5 },
    { "AElig", 0xC6 }, { "Ccedil", 0xC7 }, { "Egrave", 0xC8 }, { "Eacute", 0xC9 }, { "Ecirc", 0xCA }, { "Euml", 0xCB },
    { "Igrave", 0xCC }, { "Iacute", 0xCD }, { "Icirc", 0xCE }, { "Iuml", 0xCF }, { "ETH", 0xD0 }, { "Ntilde", 0xD1 },
    { "Ograve", 0xD2 }, { "Oacute", 0xD3 }, { "Ocirc", 0xD4 }, { "Otilde", 0xD5 }, { "Ouml", 0xD6 }, { "Oslash", 0xD8 },
    { "Ugrave", 0xD9 }, { "Uacute", 0xDA }, { "Ucirc", 0xDB }, { "Uuml", 0xDC }, { "Yacute", 0xDD }, { "THORN", 0xDE },
    { "szlig", 0xDF }, { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acirc", 0xE2 }, { "atilde", 0xE3 }, { "auml", 0xE4 },
    { "aring", 0xE5 }, { "aelig", 0xE6 }, { "ccedil", 0xE7 }, { "egrave", 0xE8 }, { "eacute", 0xE9 }, { "ecirc", 0xEA },
    { "euml", 0xEB }, { "igrave", 0xEC }, { "iacute", 0xED }, { "icirc", 0xEE }, { "iuml", 0xEF }, { "eth", 0xF0 },
    { "ntilde", 0xF1 }, { "ograve", 0xF2 }, { "oacute", 0xF3 }, { "ocirc", 0xF4 }, { "otilde", 0xF5 }, { "ouml", 0xF6 },
    { "oslash", 0xF8 }, { "ugrave", 0xF9 }, { "uacute", 0xFA }, { "ucirc", 0xFB }, { "uuml", 0xFC }, { "yacute", 0xFD },
    { "thorn", 0xFE }, { "yuml", 0xFF },
};

/* windows-1252 0x80-0x9F */
static const uint16_t cp1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

/* decodes &...; references in [s, s+n) into out (which must hold n bytes; decoding never grows text) */
static int decode_entities(const char *s, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n;) {
        if (s[i] != '&') { out[o++] = s[i++]; continue; }
        int j = i + 1;
        uint32_t cp = 0; bool ok = false;
        if (j < n && s[j] == '#') {
            j++;
            bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
            if (hex) j++;
            int start = j;
            while (j < n && (hex ? isxdigit(s[j]) : isdigit(s[j])) && j - start < 8) {
                cp = cp * (hex ? 16 : 10) + (isdigit(s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10));
                j++;
            }
            if (j > start) {
                ok = true;
                if (j < n && s[j] == ';') j++;
                if (cp >= 0x80 && cp < 0xA0) cp = cp1252[cp - 0x80];
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
            }
        } else {
            int start = j;
            while (j < n && isalnum(s[j]) && j - start < 10) j++;
            int len = j - start;
            for (size_t k = 0; k < ARRAY_LEN(entities) && len; k++) {
                if ((int)strlen(entities[k].name) == len && !strncmp(entities[k].name, s + start, len)) {
                    bool semi = j < n && s[j] == ';';
                    /* without a semicolon only the classic ones are recognised */
                    if (!semi && k > 5 && entities[k].cp != 0xA9) break;
                    cp = entities[k].cp; ok = true;
                    if (semi) j++;
                    break;
                }
            }
        }
        if (!ok) { out[o++] = s[i++]; continue; }
        char enc[4]; int el = cp ? utf8_encode(cp, enc) : 0;
        if (el > j - i) el = 0;   /* cannot happen for real entities; never overflow */
        memcpy(out + o, enc, el); o += el;
        i = j;
    }
    return o;
}

char *html_to_utf8(const char *data, size_t len, const char *charset, size_t *out_len) {
    bool latin = false;
    if (charset && (strstr(charset, "8859") || strstr(charset, "1252") || strstr(charset, "latin") || strstr(charset, "LATIN") || strstr(charset, "ascii") || strstr(charset, "ASCII")))
        latin = true;
    char *out = kmalloc(len * 3 + 1);
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)data;
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; len -= 3; latin = false; }
    for (size_t i = 0; i < len;) {
        unsigned char c = p[i];
        if (c < 0x80) { out[o++] = c ? c : ' '; i++; continue; }
        if (!latin) {
            int need = (c >= 0xC2 && c < 0xE0) ? 1 : (c >= 0xE0 && c < 0xF0) ? 2 : (c >= 0xF0 && c < 0xF5) ? 3 : 0;
            if (need && i + need < len) {
                bool valid = true;
                for (int k = 1; k <= need; k++) if ((p[i + k] & 0xC0) != 0x80) valid = false;
                if (valid) { memcpy(out + o, p + i, need + 1); o += need + 1; i += need + 1; continue; }
            }
        }
        uint32_t cp = c >= 0x80 && c < 0xA0 ? cp1252[c - 0x80] : c;
        o += utf8_encode(cp, out + o);
        i++;
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

/* ---- colours ---- */
static const struct { const char *name; uint32_t rgb; } color_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
    { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
    { "purple", 0x800080 }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "lime", 0x00FF00 }, { "olive", 0x808000 },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "orange", 0xFFA500 },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "darkred", 0x8B0000 }, { "darkgreen", 0x006400 },
    { "darkblue", 0x00008B }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 },
    { "lightgrey", 0xD3D3D3 }, { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 }, { "lightyellow", 0xFFFFE0 },
    { "gold", 0xFFD700 }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 },
    { "khaki", 0xF0E68C }, { "lavender", 0xE6E6FA }, { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 }, { "tan", 0xD2B48C },
    { "crimson", 0xDC143C }, { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "steelblue", 0x4682B4 },
    { "darkorange", 0xFF8C00 }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "midnightblue", 0x191970 },
    { "royalblue", 0x4169E1 }, { "skyblue", 0x87CEEB }, { "slategray", 0x708090 }, { "firebrick", 0xB22222 },
    { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 }, { "chocolate", 0xD2691E }, { "tomato", 0xFF6347 },
    { "orangered", 0xFF4500 }, { "linen", 0xFAF0E6 }, { "snow", 0xFFFAFA }, { "honeydew", 0xF0FFF0 },
    { "aliceblue", 0xF0F8FF }, { "azure", 0xF0FFFF }, { "mintcream", 0xF5FFFA }, { "wheat", 0xF5DEB3 },
};

bool html_parse_color(const char *s, color_t *out) {
    if (!s) return false;
    while (isspace(*s)) s++;
    char buf[32]; size_t n = 0;
    while (s[n] && !isspace(s[n]) && s[n] != ';' && n < sizeof buf - 1) { buf[n] = tolower(s[n]); n++; }
    buf[n] = 0;
    if (!n) return false;
    if (!strncmp(buf, "rgb", 3)) {
        const char *p = strchr(s, '(');
        if (!p) return false;
        int v[3] = { 0 };
        for (int i = 0; i < 3 && *p; i++) {
            p++; while (isspace(*p)) p++;
            v[i] = CLAMP(atoi(p), 0, 255);
            while (*p && *p != ',' && *p != ')' && *p != ' ') p++;
            while (*p == ' ') p++;
            if (*p == ')') break;
        }
        *out = RGB(v[0], v[1], v[2]);
        return true;
    }
    for (size_t i = 0; i < ARRAY_LEN(color_names); i++)
        if (!strcmp(buf, color_names[i].name)) { *out = 0xFF000000u | color_names[i].rgb; return true; }
    const char *h = buf[0] == '#' ? buf + 1 : buf;
    size_t hl = strlen(h);
    for (size_t i = 0; i < hl; i++) if (!isxdigit(h[i])) return false;
    uint32_t v = strtoul(h, NULL, 16);
    if (hl == 3) { *out = RGB(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17); return true; }
    if (hl == 6) { *out = 0xFF000000u | v; return true; }
    return false;
}

/* ---- tree ---- */
#define MAX_DEPTH 200

struct parser {
    struct html_doc *doc;
    struct html_node *stack[MAX_DEPTH];
    int depth;
    struct html_node *form;   /* the open <form> (forms do not nest) */
    struct form_field **field_tail;
    struct html_image **image_tail;
    struct html_image *image_hash[256];
};

static struct html_node *node_new(int type, int tag) {
    struct html_node *n = kzalloc(sizeof *n);
    n->type = type; n->tag = tag;
    return n;
}
static void append_child(struct html_node *parent, struct html_node *n) {
    n->parent = parent;
    if (parent->last) parent->last->next = n; else parent->first = n;
    parent->last = n;
}
static struct html_node *current(struct parser *p) { return p->stack[p->depth - 1]; }

const char *html_attr(const struct html_node *n, const char *name) {
    if (!n) return NULL;
    for (int i = 0; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

static size_t text_cap(size_t n) { size_t c = 32; while (c < n) c <<= 1; return c; }
static void add_text(struct parser *p, const char *s, int n) {
    if (n <= 0) return;
    struct html_node *cur = current(p);
    struct html_node *last = cur->last;
    if (last && last->type == HN_TEXT) {
        /* grow geometrically: pages can append to the same node many times */
        if (last->text_len + n + 1 > text_cap(last->text_len + 1)) last->text = krealloc(last->text, text_cap(last->text_len + n + 1));
        memcpy(last->text + last->text_len, s, n);
        last->text_len += n; last->text[last->text_len] = 0;
        return;
    }
    struct html_node *t = node_new(HN_TEXT, 0);
    t->text = kmalloc(text_cap(n + 1));
    memcpy(t->text, s, n); t->text[n] = 0; t->text_len = n;
    append_child(cur, t);
}
static void add_text_decoded(struct parser *p, const char *s, int n) {
    if (n <= 0) return;
    char *tmp = kmalloc(n + 1);
    int m = decode_entities(s, n, tmp);
    add_text(p, tmp, m);
    kfree(tmp);
}

/* index of the innermost open element with this tag, stopping at a boundary tag (-1 if none) */
static int find_open(struct parser *p, int tag, const int *bounds) {
    for (int i = p->depth - 1; i > 0; i--) {
        int t = p->stack[i]->tag;
        if (t == tag) return i;
        for (const int *b = bounds; b && *b; b++) if (t == *b) return -1;
    }
    return -1;
}
static void pop_to(struct parser *p, int idx) {
    if (idx <= 0) return;
    for (int i = p->depth - 1; i >= idx; i--) if (p->stack[i]->tag == T_FORM) p->form = NULL;
    p->depth = idx;
}
static void close_if_open(struct parser *p, int tag, const int *bounds) { int i = find_open(p, tag, bounds); if (i > 0) pop_to(p, i); }

static const int scope_bounds[] = { T_TABLE, T_TD, T_TH, T_BUTTON, T_HTML, T_CAPTION, 0 };
static const int list_bounds[] = { T_UL, T_OL, T_MENU, T_DIR, T_TABLE, T_TD, T_TH, 0 };
static const int dl_bounds[] = { T_DL, T_TABLE, T_TD, T_TH, 0 };
static const int table_bounds[] = { T_TABLE, 0 };
static const int row_bounds[] = { T_TR, T_TABLE, 0 };
static const int select_bounds[] = { T_SELECT, 0 };

static void register_field(struct parser *p, struct html_node *n) {
    struct form_field *f = kzalloc(sizeof *f);
    f->node = n; f->form = p->form;
    const char *type = html_attr(n, "type");
    if (n->tag == T_SELECT) f->kind = FF_SELECT;
    else if (n->tag == T_TEXTAREA) f->kind = FF_TEXTAREA;
    else if (n->tag == T_BUTTON) f->kind = (type && !strcasecmp(type, "button")) ? FF_BUTTON : (type && !strcasecmp(type, "reset")) ? FF_RESET : FF_SUBMIT;
    else if (!type) f->kind = FF_TEXT;
    else if (!strcasecmp(type, "password")) f->kind = FF_PASSWORD;
    else if (!strcasecmp(type, "submit")) f->kind = FF_SUBMIT;
    else if (!strcasecmp(type, "reset")) f->kind = FF_RESET;
    else if (!strcasecmp(type, "button")) f->kind = FF_BUTTON;
    else if (!strcasecmp(type, "checkbox")) f->kind = FF_CHECKBOX;
    else if (!strcasecmp(type, "radio")) f->kind = FF_RADIO;
    else if (!strcasecmp(type, "hidden")) f->kind = FF_HIDDEN;
    else if (!strcasecmp(type, "image")) f->kind = FF_IMAGE;
    else f->kind = FF_TEXT;   /* text, search, email, url, number, tel, ... */
    const char *name = html_attr(n, "name"), *value = html_attr(n, "value");
    if (name) strlcpy(f->name, name, sizeof f->name);
    if (value) strlcpy(f->value, value, sizeof f->value);
    else if (f->kind == FF_SUBMIT && n->tag == T_INPUT) strcpy(f->value, "Submit");
    else if (f->kind == FF_RESET) strcpy(f->value, "Reset");
    else if (f->kind == FF_CHECKBOX || f->kind == FF_RADIO) strcpy(f->value, "on");
    f->checked = html_attr(n, "checked") != NULL;
    n->field = f;
    /* keep document order */
    if (!p->field_tail) p->field_tail = &p->doc->fields;
    *p->field_tail = f; p->field_tail = &f->next;
}

static void start_tag(struct parser *p, int tag, struct html_attr *attrs, int nattrs, bool self_closing) {
    struct html_doc *d = p->doc;
    if (tag == T_HTML) {
        for (int i = 0; i < nattrs; i++) { kfree(attrs[i].name); kfree(attrs[i].value); }
        kfree(attrs);
        return;
    }
    if (tag == T_BODY) {
        const char *bg = NULL, *text = NULL, *link = NULL, *vlink = NULL;
        for (int i = 0; i < nattrs; i++) {
            if (!strcmp(attrs[i].name, "bgcolor")) bg = attrs[i].value;
            else if (!strcmp(attrs[i].name, "text")) text = attrs[i].value;
            else if (!strcmp(attrs[i].name, "link")) link = attrs[i].value;
            else if (!strcmp(attrs[i].name, "vlink")) vlink = attrs[i].value;
        }
        if (bg && html_parse_color(bg, &d->bg)) d->has_bg = true;
        if (text) html_parse_color(text, &d->text);
        if (link) html_parse_color(link, &d->link);
        if (vlink) html_parse_color(vlink, &d->vlink);
    }
    if (tag == T_BASE) {
        for (int i = 0; i < nattrs; i++) if (!strcmp(attrs[i].name, "href")) url_resolve(d->url, attrs[i].value, d->base, sizeof d->base);
    }
    if (tag == T_META) {
        /* <meta http-equiv=refresh> and charset are handled by the caller; nothing to keep */
    }
    if (closes_p(tag)) close_if_open(p, T_P, scope_bounds);
    switch (tag) {
    case T_LI: close_if_open(p, T_LI, list_bounds); break;
    case T_DT: case T_DD: close_if_open(p, T_DT, dl_bounds); close_if_open(p, T_DD, dl_bounds); break;
    case T_TR: close_if_open(p, T_TR, table_bounds); break;
    case T_TD: case T_TH: close_if_open(p, T_TD, row_bounds); close_if_open(p, T_TH, row_bounds); break;
    case T_THEAD: case T_TBODY: case T_TFOOT: close_if_open(p, T_THEAD, table_bounds); close_if_open(p, T_TBODY, table_bounds); close_if_open(p, T_TFOOT, table_bounds); break;
    case T_OPTION: close_if_open(p, T_OPTION, select_bounds); break;
    case T_OPTGROUP: close_if_open(p, T_OPTION, select_bounds); close_if_open(p, T_OPTGROUP, select_bounds); break;
    case T_A: close_if_open(p, T_A, NULL); break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
        int t = current(p)->tag;
        if (t >= T_H1 && t <= T_H6) pop_to(p, p->depth - 1);
        break; }
    case T_FORM:
        if (p->form) { for (int i = 0; i < nattrs; i++) { kfree(attrs[i].name); kfree(attrs[i].value); } kfree(attrs); return; }
        break;
    }
    struct html_node *n = node_new(HN_ELEMENT, tag);
    n->attrs = attrs; n->nattrs = nattrs;
    append_child(current(p), n);
    if (tag == T_FORM) p->form = n;
    if (tag == T_INPUT || tag == T_SELECT || tag == T_TEXTAREA || tag == T_BUTTON) register_field(p, n);
    if (tag == T_IMG) {
        const char *src = html_attr(n, "src");
        if (!src || !*src) src = html_attr(n, "data-src");
        if (src && *src && strncmp(src, "data:", 5)) {
            char abs[1024];
            url_resolve(d->base, src, abs, sizeof abs);
            struct html_image *im = NULL;
            uint32_t h = 5381;
            for (const char *q = abs; *q; q++) h = h * 33 + (unsigned char)*q;
            for (struct html_image *x = p->image_hash[h & 255]; x; x = x->hnext) if (!strcmp(x->url, abs)) { im = x; break; }
            if (!im) {
                im = kzalloc(sizeof *im);
                im->url = kstrdup(abs);
                im->hnext = p->image_hash[h & 255]; p->image_hash[h & 255] = im;
                if (!p->image_tail) p->image_tail = &d->images;
                *p->image_tail = im; p->image_tail = &im->next;
            }
            n->image = im;
        }
    }
    if (!is_void(tag) && !self_closing && p->depth < MAX_DEPTH) p->stack[p->depth++] = n;
}

static void end_tag(struct parser *p, int tag) {
    if (tag == T_BR) { start_tag(p, T_BR, NULL, 0, true); return; }
    if (tag == T_HTML || tag == T_BODY || tag == T_UNKNOWN) return;
    const int *bounds = NULL;
    if (tag == T_TD || tag == T_TH || tag == T_TR || tag == T_THEAD || tag == T_TBODY || tag == T_TFOOT) bounds = table_bounds;
    if (tag == T_LI) bounds = list_bounds;
    if (tag == T_P) bounds = scope_bounds;
    int i = find_open(p, tag, bounds);
    if (i > 0) pop_to(p, i);
}

/* text of a raw-text element: [s, end) up to "</name" */
static const char *find_close(const char *s, const char *end, const char *name) {
    size_t nl = strlen(name);
    for (const char *q = s; q + 2 + nl <= end; q++) {
        if (q[0] != '<' || q[1] != '/') continue;
        size_t k = 0;
        while (k < nl && tolower(q[2 + k]) == name[k]) k++;
        if (k == nl) return q;
    }
    return end;
}

static void collapse_into(char *dst, size_t dstsz, const char *s, int n) {
    size_t o = 0; bool sp = true;
    for (int i = 0; i < n && o < dstsz - 1; i++) {
        if (isspace(s[i])) { if (!sp) { dst[o++] = ' '; sp = true; } }
        else { dst[o++] = s[i]; sp = false; }
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

static void parse_tokens(struct parser *p, const char *s, size_t len) {
    const char *end = s + len, *q = s;
    while (q < end) {
        if (*q != '<') {
            const char *lt = memchr(q, '<', end - q);
            if (!lt) lt = end;
            add_text_decoded(p, q, lt - q);
            q = lt;
            continue;
        }
        if (end - q >= 4 && !strncmp(q, "<!--", 4)) {
            const char *c = q + 4;
            while (c + 3 <= end && strncmp(c, "-->", 3)) c++;
            q = c + 3 <= end ? c + 3 : end;
            continue;
        }
        if (q + 1 < end && (q[1] == '!' || q[1] == '?')) {
            const char *gt = memchr(q, '>', end - q);
            q = gt ? gt + 1 : end;
            continue;
        }
        bool closing = q + 1 < end && q[1] == '/';
        const char *nstart = q + (closing ? 2 : 1);
        if (nstart >= end || !isalpha(*nstart)) { add_text(p, "<", 1); q++; continue; }
        const char *nend = nstart;
        while (nend < end && (isalnum(*nend) || *nend == '-' || *nend == ':')) nend++;
        int tag = tag_lookup(nstart, nend - nstart);
        /* attributes */
        struct html_attr *attrs = NULL; int nattrs = 0, cap = 0;
        const char *a = nend;
        bool self_closing = false;
        while (a < end && *a != '>') {
            while (a < end && (isspace(*a) || *a == '/')) { if (*a == '/' && a + 1 < end && a[1] == '>') self_closing = true; a++; }
            if (a >= end || *a == '>') break;
            const char *an = a;
            while (a < end && !isspace(*a) && *a != '=' && *a != '>' && !(*a == '/' && a + 1 < end && a[1] == '>')) a++;
            const char *ane = a;
            while (a < end && isspace(*a)) a++;
            const char *vs = NULL, *ve = NULL;
            if (a < end && *a == '=') {
                a++;
                while (a < end && isspace(*a)) a++;
                if (a < end && (*a == '"' || *a == '\'')) {
                    char quote = *a++;
                    vs = a;
                    while (a < end && *a != quote) a++;
                    ve = a;
                    if (a < end) a++;
                } else {
                    vs = a;
                    while (a < end && !isspace(*a) && *a != '>') a++;
                    ve = a;
                }
            }
            if (closing || ane == an) continue;
            if (nattrs == cap) { cap = cap ? cap * 2 : 4; attrs = krealloc(attrs, cap * sizeof *attrs); }
            char *name = kmalloc(ane - an + 1);
            for (int i = 0; i < ane - an; i++) name[i] = tolower(an[i]);
            name[ane - an] = 0;
            char *value;
            if (vs) { value = kmalloc(ve - vs + 1); int m = decode_entities(vs, ve - vs, value); value[m] = 0; }
            else value = kstrdup("");
            attrs[nattrs].name = name; attrs[nattrs].value = value; nattrs++;
        }
        q = a < end ? a + 1 : end;
        if (closing) { end_tag(p, tag); continue; }
        if (tag == T_UNKNOWN) {
            for (int i = 0; i < nattrs; i++) { kfree(attrs[i].name); kfree(attrs[i].value); }
            kfree(attrs);
            continue;
        }
        switch (tag) {
        case T_SCRIPT: case T_STYLE: case T_TEMPLATE: {
            for (int i = 0; i < nattrs; i++) { kfree(attrs[i].name); kfree(attrs[i].value); }
            kfree(attrs);
            const char *c = find_close(q, end, tag_names[tag]);
            const char *gt = c < end ? memchr(c, '>', end - c) : NULL;
            q = gt ? gt + 1 : end;
            continue; }
        case T_TITLE: {
            for (int i = 0; i < nattrs; i++) { kfree(attrs[i].name); kfree(attrs[i].value); }
            kfree(attrs);
            const char *c = find_close(q, end, "title");
            if (!p->doc->title[0]) {
                char *tmp = kmalloc(c - q + 1);
                int m = decode_entities(q, c - q, tmp);
                collapse_into(p->doc->title, sizeof p->doc->title, tmp, m);
                kfree(tmp);
            }
            const char *gt = c < end ? memchr(c, '>', end - c) : NULL;
            q = gt ? gt + 1 : end;
            continue; }
        case T_TEXTAREA: case T_XMP: case T_PLAINTEXT: {
            start_tag(p, tag, attrs, nattrs, false);
            const char *c = tag == T_PLAINTEXT ? end : find_close(q, end, tag_names[tag]);
            const char *body = q;
            if (body < c && *body == '\n') body++;
            if (tag == T_TEXTAREA) add_text_decoded(p, body, c - body); else add_text(p, body, c - body);
            if (tag == T_TEXTAREA && current(p)->field) {
                struct form_field *f = current(p)->field;
                char *tmp = kmalloc(c - body + 1);
                int m = decode_entities(body, c - body, tmp);
                tmp[m] = 0; strlcpy(f->value, tmp, sizeof f->value); kfree(tmp);
            }
            end_tag(p, tag);
            const char *gt = c < end ? memchr(c, '>', end - c) : NULL;
            q = gt ? gt + 1 : end;
            continue; }
        }
        start_tag(p, tag, attrs, nattrs, self_closing);
    }
}

/* select boxes: count options and pick the initially selected one */
static void finish_fields(struct html_doc *d) {
    for (struct form_field *f = d->fields; f; f = f->next) {
        if (f->kind != FF_SELECT) continue;
        int idx = 0;
        f->selected = 0;
        struct html_node *stack[32]; int sp = 0;
        struct html_node *n = f->node->first;
        while (n || sp) {
            if (!n) { n = stack[--sp]->next; continue; }
            if (n->type == HN_ELEMENT && n->tag == T_OPTION) { if (html_attr(n, "selected")) f->selected = idx; idx++; n = n->next; continue; }
            if (n->type == HN_ELEMENT && n->first && sp < 32) { stack[sp++] = n; n = n->first; continue; }
            n = n->next;
        }
        f->noptions = idx;
    }
}

static const char *sniff_charset(const char *data, size_t len) {
    static char cs[32];
    size_t lim = MIN(len, (size_t)2048);
    for (size_t i = 0; i + 8 < lim; i++) {
        if (strncmp(data + i, "charset=", 8) && strncmp(data + i, "CHARSET=", 8)) continue;
        const char *v = data + i + 8;
        while (*v == '"' || *v == '\'' || *v == ' ') v++;
        size_t n = 0;
        while (v + n < data + lim && n < sizeof cs - 1 && (isalnum(v[n]) || v[n] == '-' || v[n] == '_')) { cs[n] = tolower(v[n]); n++; }
        cs[n] = 0;
        return n ? cs : NULL;
    }
    return NULL;
}

static struct html_doc *doc_new(const char *url) {
    struct html_doc *d = kzalloc(sizeof *d);
    strlcpy(d->url, url ? url : "about:blank", sizeof d->url);
    strlcpy(d->base, d->url, sizeof d->base);
    d->bg = RGB(255, 255, 255);
    d->text = RGB(0x1D, 0x1D, 0x1F);
    d->link = RGB(0x0B, 0x57, 0xD0);
    d->vlink = RGB(0x68, 0x1D, 0xA8);
    d->root = node_new(HN_ELEMENT, T_HTML);
    return d;
}

struct html_doc *html_parse(const char *data, size_t len, const char *charset, const char *url) {
    struct html_doc *d = doc_new(url);
    if (!charset) charset = sniff_charset(data, len);
    size_t ulen;
    char *u = html_to_utf8(data, len, charset, &ulen);
    d->source = u; d->source_len = ulen;
    struct parser *p = kzalloc(sizeof *p);
    p->doc = d;
    p->stack[0] = d->root; p->depth = 1;
    parse_tokens(p, u, ulen);
    kfree(p);
    finish_fields(d);
    return d;
}

struct html_doc *html_from_text(const char *data, size_t len, const char *url) {
    struct html_doc *d = doc_new(url);
    size_t ulen;
    char *u = html_to_utf8(data, len, NULL, &ulen);
    d->source = u; d->source_len = ulen;
    struct html_node *pre = node_new(HN_ELEMENT, T_PRE);
    append_child(d->root, pre);
    struct html_node *t = node_new(HN_TEXT, 0);
    t->text = kmalloc(ulen + 1); memcpy(t->text, u, ulen + 1); t->text_len = ulen;
    append_child(pre, t);
    const char *slash = strrchr(d->url, '/');
    strlcpy(d->title, slash && slash[1] ? slash + 1 : d->url, sizeof d->title);
    return d;
}

static void free_node(struct html_node *n) {
    /* iterative over siblings, recursive over children (depth is capped by the parser) */
    while (n) {
        struct html_node *next = n->next;
        free_node(n->first);
        for (int i = 0; i < n->nattrs; i++) { kfree(n->attrs[i].name); kfree(n->attrs[i].value); }
        kfree(n->attrs); kfree(n->text); kfree(n);
        n = next;
    }
}

void html_free(struct html_doc *d) {
    if (!d) return;
    free_node(d->root);
    for (struct html_image *im = d->images, *nx; im; im = nx) { nx = im->next; kfree(im->url); canvas_destroy(im->img); canvas_destroy(im->scaled); kfree(im); }
    for (struct form_field *f = d->fields, *nx; f; f = nx) { nx = f->next; kfree(f); }
    kfree(d->source);
    kfree(d);
}
