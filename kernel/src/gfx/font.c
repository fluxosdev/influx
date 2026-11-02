#include "gfx.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

#pragma pack(push, 1)
struct fnt_header { char magic[4]; uint16_t size; int16_t ascent, descent; uint16_t line_height; uint32_t glyph_count, data_size; };
struct fnt_glyph  { uint32_t cp; int16_t x0, y0; uint16_t w, h, advance_q8, pad; uint32_t off; };
#pragma pack(pop)

struct font {
    struct fnt_header hdr;
    const struct fnt_glyph *glyphs;
    const uint8_t *data;
    void *blob;
    int16_t direct[256];   /* glyph index for cp < 256, -1 if none */
};

struct font *font_ui, *font_ui_small, *font_ui_medium, *font_ui_medium_small, *font_ui_bold, *font_ui_semibold;
struct font *font_ui_large, *font_ui_title, *font_ui_semibold_large, *font_ui_semibold_xl, *font_ui_display, *font_ui_huge;
struct font *font_mono, *font_mono_bold;

uint32_t utf8_decode(const char **sp) {
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t cp; int extra;
    if (s[0] < 0x80) { cp = s[0]; extra = 0; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
    else { *sp += 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) { *sp += i; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *sp += extra + 1;
    return cp;
}
int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = cp; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3; }
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); return 4;
}

struct font *font_from_memory(const void *data, size_t size) {
    if (size < sizeof(struct fnt_header) || memcmp(data, "IFNT", 4)) return NULL;
    struct font *f = kzalloc(sizeof *f);
    memcpy(&f->hdr, data, sizeof f->hdr);
    f->blob = (void *)data;
    f->glyphs = (const struct fnt_glyph *)((const uint8_t *)data + sizeof(struct fnt_header));
    f->data = (const uint8_t *)(f->glyphs + f->hdr.glyph_count);
    for (int i = 0; i < 256; i++) f->direct[i] = -1;
    for (uint32_t i = 0; i < f->hdr.glyph_count; i++) if (f->glyphs[i].cp < 256) f->direct[f->glyphs[i].cp] = (int16_t)i;
    return f;
}
struct font *font_load(const char *path) {
    void *data; size_t size;
    if (vfs_read_file(path, &data, &size) < 0) { kprintf("font: cannot load %s\n", path); return NULL; }
    struct font *f = font_from_memory(data, size);
    if (!f) kfree(data);
    return f;
}
static const struct fnt_glyph *glyph(const struct font *f, uint32_t cp) {
    if (cp < 256) { int i = f->direct[cp]; return i >= 0 ? &f->glyphs[i] : NULL; }
    for (uint32_t i = 0; i < f->hdr.glyph_count; i++) if (f->glyphs[i].cp == cp) return &f->glyphs[i];
    return NULL;
}
int font_height(const struct font *f) { return f->hdr.line_height; }
int font_ascent(const struct font *f) { return f->hdr.ascent; }
int font_size(const struct font *f) { return f->hdr.size; }
int font_char_width(const struct font *f, uint32_t cp) { const struct fnt_glyph *g = glyph(f, cp); if (!g) g = glyph(f, '?'); return g ? (g->advance_q8 + 128) >> 8 : 0; }

int font_text_width_n(const struct font *f, const char *s, size_t n) {
    const char *end = s + n; int pen = 0;
    while (s < end && *s) {
        uint32_t cp = utf8_decode(&s);
        const struct fnt_glyph *g = glyph(f, cp); if (!g) g = glyph(f, '?');
        if (g) pen += g->advance_q8;
    }
    return (pen + 128) >> 8;
}
int font_text_width(const struct font *f, const char *s) { return font_text_width_n(f, s, strlen(s)); }

int font_draw_n(struct canvas *c, const struct font *f, int x, int y, color_t col, const char *s, size_t n) {
    if (!f) return 0;
    const char *end = s + n;
    int pen = x << 8;   /* 24.8 */
    int base = y + f->hdr.ascent;
    uint32_t ca = ALPHA(col);
    while (s < end && *s) {
        uint32_t cp = utf8_decode(&s);
        const struct fnt_glyph *g = glyph(f, cp); if (!g) g = glyph(f, '?');
        if (!g) continue;
        if (g->w && g->h) {
            int gx = ((pen + 128) >> 8) + g->x0, gy = base + g->y0;
            struct rect dr = rect_intersect(rect_make(gx, gy, g->w, g->h), c->clip);
            for (int yy = dr.y; yy < dr.y + dr.h; yy++) {
                const uint8_t *src = f->data + g->off + (yy - gy) * g->w + (dr.x - gx);
                uint32_t *d = c->px + yy * c->stride + dr.x;
                for (int i = 0; i < dr.w; i++) {
                    uint32_t a = src[i];
                    if (!a) continue;
                    a = (a * ca) / 255;
                    d[i] = color_blend(d[i], WITH_ALPHA(col, a));
                }
            }
        }
        pen += g->advance_q8;
    }
    return ((pen + 128) >> 8) - x;
}
int font_draw(struct canvas *c, const struct font *f, int x, int y, color_t col, const char *s) { return font_draw_n(c, f, x, y, col, s, strlen(s)); }

int font_fit_chars(const struct font *f, const char *s, int maxw) {
    const char *p = s; int pen = 0;
    while (*p) {
        const char *q = p;
        uint32_t cp = utf8_decode(&q);
        const struct fnt_glyph *g = glyph(f, cp); if (!g) g = glyph(f, '?');
        int adv = g ? g->advance_q8 : 0;
        if (((pen + adv + 128) >> 8) > maxw) break;
        pen += adv; p = q;
    }
    return (int)(p - s);
}

int font_draw_ellipsized(struct canvas *c, const struct font *f, int x, int y, int maxw, color_t col, const char *s) {
    if (font_text_width(f, s) <= maxw) return font_draw(c, f, x, y, col, s);
    int ew = font_text_width(f, "…");
    int n = font_fit_chars(f, s, maxw - ew);
    int w = font_draw_n(c, f, x, y, col, s, n);
    return w + font_draw(c, f, x + w, y, col, "…");
}
void font_draw_aligned(struct canvas *c, const struct font *f, struct rect r, color_t col, const char *s, int align) {
    int w = font_text_width(f, s), h = font_height(f);
    int x = align == 1 ? r.x + (r.w - w) / 2 : align == 2 ? r.x + r.w - w : r.x;
    int y = r.y + (r.h - h) / 2;
    font_draw(c, f, x, y, col, s);
}
void font_draw_centered(struct canvas *c, const struct font *f, struct rect r, color_t col, const char *s) { font_draw_aligned(c, f, r, col, s, 1); }

void fonts_init(void) {
    font_ui = font_load("/sys/fonts/inter-regular-13.fnt");
    font_ui_small = font_load("/sys/fonts/inter-regular-11.fnt");
    font_ui_medium = font_load("/sys/fonts/inter-medium-13.fnt");
    font_ui_medium_small = font_load("/sys/fonts/inter-medium-11.fnt");
    font_ui_semibold = font_load("/sys/fonts/inter-semibold-13.fnt");
    font_ui_bold = font_load("/sys/fonts/inter-bold-13.fnt");
    font_ui_large = font_load("/sys/fonts/inter-regular-15.fnt");
    font_ui_semibold_large = font_load("/sys/fonts/inter-semibold-15.fnt");
    font_ui_semibold_xl = font_load("/sys/fonts/inter-semibold-20.fnt");
    font_ui_title = font_load("/sys/fonts/inter-bold-22.fnt");
    font_ui_display = font_load("/sys/fonts/inter-regular-24.fnt");
    font_ui_huge = font_load("/sys/fonts/inter-regular-34.fnt");
    font_mono = font_load("/sys/fonts/scp-regular-14.fnt");
    font_mono_bold = font_load("/sys/fonts/scp-bold-14.fnt");
    if (!font_ui || !font_mono) panic("fonts: UI fonts missing from initrd");
    kprintf("fonts: Inter %dpx (line %d, ascent %d), Source Code Pro %dpx loaded\n", font_size(font_ui), font_height(font_ui), font_ascent(font_ui), font_size(font_mono));
}
