#pragma once
#include "kernel.h"

typedef uint32_t color_t;   /* 0xAARRGGBB, straight alpha */
#define RGB(r, g, b)     (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define ALPHA(c) ((c) >> 24)
#define RED(c)   (((c) >> 16) & 0xFF)
#define GREEN(c) (((c) >> 8) & 0xFF)
#define BLUE(c)  ((c) & 0xFF)
#define WITH_ALPHA(c, a) (((c) & 0x00FFFFFF) | ((uint32_t)(a) << 24))

struct rect { int x, y, w, h; };
static inline struct rect rect_make(int x, int y, int w, int h) { struct rect r = { x, y, w, h }; return r; }
static inline bool rect_contains(struct rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
static inline bool rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_overlaps(struct rect a, struct rect b) { return !rect_empty(rect_intersect(a, b)); }

struct canvas {
    uint32_t *px;
    int w, h, stride;      /* stride in pixels */
    struct rect clip;
    bool owns_px;
};

struct canvas *canvas_create(int w, int h);
void canvas_init(struct canvas *c, uint32_t *px, int w, int h, int stride);
void canvas_destroy(struct canvas *c);
void canvas_set_clip(struct canvas *c, struct rect r);
void canvas_reset_clip(struct canvas *c);
struct canvas canvas_sub(struct canvas *c, struct rect r);   /* view into a region; clip = region */

color_t color_blend(color_t dst, color_t src);               /* src over dst */
color_t color_mix(color_t a, color_t b, int t256);           /* lerp */
color_t color_lighten(color_t c, int amount);
color_t color_darken(color_t c, int amount);
color_t color_with_alpha(color_t c, int a);

void gfx_fill(struct canvas *c, color_t col);
void gfx_fill_rect(struct canvas *c, int x, int y, int w, int h, color_t col);
void gfx_fill_rect_r(struct canvas *c, struct rect r, color_t col);
void gfx_draw_rect(struct canvas *c, int x, int y, int w, int h, color_t col);  /* 1px outline */
void gfx_fill_rounded_rect(struct canvas *c, int x, int y, int w, int h, int radius, color_t col);
void gfx_draw_rounded_rect(struct canvas *c, int x, int y, int w, int h, int radius, color_t col);
void gfx_fill_rounded_rect_corners(struct canvas *c, int x, int y, int w, int h, int radius, color_t col, int corner_mask); /* 1=TL 2=TR 4=BL 8=BR */
void gfx_fill_circle(struct canvas *c, int cx, int cy, int r, color_t col);
void gfx_draw_circle(struct canvas *c, int cx, int cy, int r, color_t col);
void gfx_hline(struct canvas *c, int x, int y, int w, color_t col);
void gfx_vline(struct canvas *c, int x, int y, int h, color_t col);
void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, color_t col);
void gfx_vgradient(struct canvas *c, int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_hgradient(struct canvas *c, int x, int y, int w, int h, color_t left, color_t right);
void gfx_blit(struct canvas *dst, int x, int y, const struct canvas *src);                   /* alpha blended */
void gfx_blit_opaque(struct canvas *dst, int x, int y, const struct canvas *src);            /* straight copy */
void gfx_blit_rect(struct canvas *dst, int x, int y, const struct canvas *src, struct rect sr, bool blend);
void gfx_blit_scaled(struct canvas *dst, int x, int y, int w, int h, const struct canvas *src);  /* bilinear */
void gfx_blit_tinted(struct canvas *dst, int x, int y, const struct canvas *src, color_t tint); /* use src alpha as mask */
void gfx_blit_scaled_tinted(struct canvas *dst, int x, int y, int w, int h, const struct canvas *src, color_t tint);
void gfx_blit_rounded(struct canvas *dst, int x, int y, const struct canvas *src, int radius, int corner_mask);
void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int radius, int spread, int max_alpha);
void gfx_shadow_ex(struct canvas *c, int x, int y, int w, int h, int radius, int spread, int max_alpha, struct rect skip);
void gfx_copy_rect(struct canvas *dst, int dx, int dy, const struct canvas *src, int sx, int sy, int w, int h);
void gfx_blur_rect(struct canvas *c, struct rect r, int radius);

/* fonts */
struct font;
struct font *font_load(const char *path);
struct font *font_from_memory(const void *data, size_t size);
int font_height(const struct font *f);        /* line height */
int font_ascent(const struct font *f);
int font_size(const struct font *f);
int font_char_width(const struct font *f, uint32_t cp);
int font_text_width(const struct font *f, const char *s);
int font_text_width_n(const struct font *f, const char *s, size_t n);
int font_draw(struct canvas *c, const struct font *f, int x, int y, color_t col, const char *s);   /* y = top of line box; returns width */
int font_draw_n(struct canvas *c, const struct font *f, int x, int y, color_t col, const char *s, size_t n);
int font_draw_ellipsized(struct canvas *c, const struct font *f, int x, int y, int maxw, color_t col, const char *s);
void font_draw_centered(struct canvas *c, const struct font *f, struct rect r, color_t col, const char *s);
void font_draw_aligned(struct canvas *c, const struct font *f, struct rect r, color_t col, const char *s, int align); /* 0 left,1 center,2 right; vertically centered */
int font_fit_chars(const struct font *f, const char *s, int maxw);   /* bytes that fit */
uint32_t utf8_decode(const char **s);
int utf8_encode(uint32_t cp, char *out);

/* standard UI fonts, loaded at desktop start */
extern struct font *font_ui, *font_ui_small, *font_ui_medium, *font_ui_medium_small, *font_ui_bold, *font_ui_semibold;
extern struct font *font_ui_large, *font_ui_title, *font_ui_semibold_large, *font_ui_semibold_xl, *font_ui_display, *font_ui_huge;
extern struct font *font_mono, *font_mono_bold;
void fonts_init(void);

/* images */
struct canvas *image_load(const char *path);
struct canvas *image_from_memory(const void *data, size_t size);

/* framebuffer */
extern struct canvas *screen;    /* back buffer */
void fb_init(void);
void fb_flush(struct rect r);    /* copy back buffer to the physical framebuffer */
void fb_flush_all(void);
void fb_present(void);           /* page flip (no-op on the software path) */
void fb_set_mode(uint64_t phys, int w, int h, int pitch);
int fb_width(void);
int fb_height(void);
void fb_boot_splash(void);
void fb_boot_progress(const char *msg);

int isqrt64(uint64_t v);
