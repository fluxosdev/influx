#include "gfx.h"
#include "mm.h"
#include "string.h"
#include "simd.h"

struct rect rect_intersect(struct rect a, struct rect b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0; if (r.h < 0) r.h = 0;
    return r;
}
struct rect rect_union(struct rect a, struct rect b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

int isqrt64(uint64_t v) {
    if (v == 0) return 0;
    uint64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int)x;
}

struct canvas *canvas_create(int w, int h) {
    w = w < 0 ? 0 : MIN(w, 16384); h = h < 0 ? 0 : MIN(h, 16384);   /* callers don't check for NULL: clamp */
    struct canvas *c = kzalloc(sizeof *c);
    c->px = kzalloc((size_t)w * h * 4);
    c->w = w; c->h = h; c->stride = w; c->owns_px = true;
    canvas_reset_clip(c);
    return c;
}
void canvas_init(struct canvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride; c->owns_px = false;
    canvas_reset_clip(c);
}
void canvas_destroy(struct canvas *c) { if (!c) return; if (c->owns_px) kfree(c->px); kfree(c); }
void canvas_set_clip(struct canvas *c, struct rect r) { c->clip = rect_intersect(r, rect_make(0, 0, c->w, c->h)); }
void canvas_reset_clip(struct canvas *c) { c->clip = rect_make(0, 0, c->w, c->h); }
struct canvas canvas_sub(struct canvas *c, struct rect r) {
    struct canvas s;
    struct rect cr = rect_intersect(r, rect_make(0, 0, c->w, c->h));
    s.px = c->px + cr.y * c->stride + cr.x;
    s.w = cr.w; s.h = cr.h; s.stride = c->stride; s.owns_px = false;
    /* clip: intersection of parent's clip translated */
    struct rect pc = rect_make(c->clip.x - cr.x, c->clip.y - cr.y, c->clip.w, c->clip.h);
    s.clip = rect_intersect(pc, rect_make(0, 0, s.w, s.h));
    return s;
}

color_t color_blend(color_t dst, color_t src) { return blend_px(dst, src, ALPHA(src)); }
color_t color_mix(color_t a, color_t b, int t) {
    if (t <= 0) return a; if (t >= 256) return b;
    int it = 256 - t;
    return RGBA((RED(a) * it + RED(b) * t) >> 8, (GREEN(a) * it + GREEN(b) * t) >> 8, (BLUE(a) * it + BLUE(b) * t) >> 8, (ALPHA(a) * it + ALPHA(b) * t) >> 8);
}
color_t color_lighten(color_t c, int amount) { return RGBA(CLAMP((int)RED(c) + amount, 0, 255), CLAMP((int)GREEN(c) + amount, 0, 255), CLAMP((int)BLUE(c) + amount, 0, 255), ALPHA(c)); }
color_t color_darken(color_t c, int amount) { return RGBA(CLAMP((int)RED(c) - amount, 0, 255), CLAMP((int)GREEN(c) - amount, 0, 255), CLAMP((int)BLUE(c) - amount, 0, 255), ALPHA(c)); }
color_t color_with_alpha(color_t c, int a) { return WITH_ALPHA(c, CLAMP(a, 0, 255)); }

static inline void put_px(struct canvas *c, int x, int y, color_t col) {
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h) return;
    uint32_t *p = c->px + y * c->stride + x;
    *p = blend_px(*p, col, ALPHA(col));
}
static inline void put_px_cov(struct canvas *c, int x, int y, color_t col, int cov256) {
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h) return;
    uint32_t a = (ALPHA(col) * (uint32_t)CLAMP(cov256, 0, 256)) >> 8;
    uint32_t *p = c->px + y * c->stride + x;
    *p = blend_px(*p, col, a);
}

void gfx_fill(struct canvas *c, color_t col) { gfx_fill_rect(c, 0, 0, c->w, c->h, col); }

void gfx_fill_rect(struct canvas *c, int x, int y, int w, int h, color_t col) {
    struct rect r = rect_intersect(rect_make(x, y, w, h), c->clip);
    if (rect_empty(r)) return;
    uint32_t a = ALPHA(col);
    if (a == 0) return;
    bool simd = a != 255 && simd_ok();
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        uint32_t *p = c->px + yy * c->stride + r.x;
        if (a == 255) memset32(p, col, r.w);
        else if (simd) simd_blend_solid(p, col, a, r.w);
        else for (int i = 0; i < r.w; i++) p[i] = blend_px(p[i], col, a);
    }
}
void gfx_fill_rect_r(struct canvas *c, struct rect r, color_t col) { gfx_fill_rect(c, r.x, r.y, r.w, r.h, col); }
void gfx_draw_rect(struct canvas *c, int x, int y, int w, int h, color_t col) {
    gfx_hline(c, x, y, w, col); gfx_hline(c, x, y + h - 1, w, col);
    gfx_vline(c, x, y + 1, h - 2, col); gfx_vline(c, x + w - 1, y + 1, h - 2, col);
}
void gfx_hline(struct canvas *c, int x, int y, int w, color_t col) { gfx_fill_rect(c, x, y, w, 1, col); }
void gfx_vline(struct canvas *c, int x, int y, int h, color_t col) { gfx_fill_rect(c, x, y, 1, h, col); }

void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, color_t col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        put_px(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/* coverage (0..256) of pixel (px,py) for a circle of radius r centred at (cx,cy), all in pixel units with
 * pixel centres at +0.5; uses 1/16 sub-pixel fixed point */
static inline int corner_cov(int px, int py, int cx, int cy, int r) {
    int dx = (px * 16 + 8) - cx * 16, dy = (py * 16 + 8) - cy * 16;
    int d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
    int cov = r * 16 + 8 - d;   /* >16 inside, <0 outside */
    if (cov <= 0) return 0; if (cov >= 16) return 256;
    return cov * 16;
}

void gfx_fill_rounded_rect_corners(struct canvas *c, int x, int y, int w, int h, int r, color_t col, int mask) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    if (r <= 0 || mask == 0) { gfx_fill_rect(c, x, y, w, h, col); return; }
    /* middle band + top/bottom strips without corners */
    gfx_fill_rect(c, x, y + r, w, h - 2 * r, col);
    gfx_fill_rect(c, x + r, y, w - 2 * r, r, col);
    gfx_fill_rect(c, x + r, y + h - r, w - 2 * r, r, col);
    /* corners */
    for (int cy = 0; cy < r; cy++) for (int cx = 0; cx < r; cx++) {
        int tlx = x + cx, tly = y + cy;
        int cov_tl = corner_cov(tlx, tly, x + r, y + r, r);
        int trx = x + w - 1 - cx, brx = trx, bly = y + h - 1 - cy;
        if (mask & 1) put_px_cov(c, tlx, tly, col, cov_tl); else put_px(c, tlx, tly, col);
        if (mask & 2) put_px_cov(c, trx, tly, col, cov_tl); else put_px(c, trx, tly, col);
        if (mask & 4) put_px_cov(c, tlx, bly, col, cov_tl); else put_px(c, tlx, bly, col);
        if (mask & 8) put_px_cov(c, brx, bly, col, cov_tl); else put_px(c, brx, bly, col);
    }
}
void gfx_fill_rounded_rect(struct canvas *c, int x, int y, int w, int h, int r, color_t col) { gfx_fill_rounded_rect_corners(c, x, y, w, h, r, col, 15); }

void gfx_draw_rounded_rect(struct canvas *c, int x, int y, int w, int h, int r, color_t col) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    gfx_hline(c, x + r, y, w - 2 * r, col); gfx_hline(c, x + r, y + h - 1, w - 2 * r, col);
    gfx_vline(c, x, y + r, h - 2 * r, col); gfx_vline(c, x + w - 1, y + r, h - 2 * r, col);
    for (int cy = 0; cy < r; cy++) for (int cx = 0; cx < r; cx++) {
        int outer = corner_cov(x + cx, y + cy, x + r, y + r, r);
        int inner = corner_cov(x + cx, y + cy, x + r, y + r, r - 1);
        int cov = outer - inner; if (cov <= 0) continue;
        put_px_cov(c, x + cx, y + cy, col, cov);
        put_px_cov(c, x + w - 1 - cx, y + cy, col, cov);
        put_px_cov(c, x + cx, y + h - 1 - cy, col, cov);
        put_px_cov(c, x + w - 1 - cx, y + h - 1 - cy, col, cov);
    }
}

void gfx_fill_circle(struct canvas *c, int cx, int cy, int r, color_t col) {
    for (int y = cy - r - 1; y <= cy + r + 1; y++) for (int x = cx - r - 1; x <= cx + r + 1; x++) {
        int dx = (x * 16 + 8) - cx * 16, dy = (y * 16 + 8) - cy * 16;
        int d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
        int cov = r * 16 - d + 8;
        if (cov <= 0) continue;
        put_px_cov(c, x, y, col, cov >= 16 ? 256 : cov * 16);
    }
}
void gfx_draw_circle(struct canvas *c, int cx, int cy, int r, color_t col) {
    for (int y = cy - r - 1; y <= cy + r + 1; y++) for (int x = cx - r - 1; x <= cx + r + 1; x++) {
        int dx = (x * 16 + 8) - cx * 16, dy = (y * 16 + 8) - cy * 16;
        int d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
        int outer = r * 16 - d + 8, inner = (r - 1) * 16 - d + 8;
        outer = CLAMP(outer, 0, 16); inner = CLAMP(inner, 0, 16);
        int cov = outer - inner;
        if (cov > 0) put_px_cov(c, x, y, col, cov * 16);
    }
}

void gfx_vgradient(struct canvas *c, int x, int y, int w, int h, color_t top, color_t bottom) {
    for (int i = 0; i < h; i++) gfx_fill_rect(c, x, y + i, w, 1, color_mix(top, bottom, h > 1 ? i * 256 / (h - 1) : 0));
}
void gfx_hgradient(struct canvas *c, int x, int y, int w, int h, color_t left, color_t right) {
    for (int i = 0; i < w; i++) gfx_fill_rect(c, x + i, y, 1, h, color_mix(left, right, w > 1 ? i * 256 / (w - 1) : 0));
}

void gfx_blit_rect(struct canvas *dst, int x, int y, const struct canvas *src, struct rect sr, bool blend) {
    sr = rect_intersect(sr, rect_make(0, 0, src->w, src->h));
    struct rect dr = rect_intersect(rect_make(x, y, sr.w, sr.h), dst->clip);
    if (rect_empty(dr)) return;
    int ox = dr.x - x, oy = dr.y - y;
    bool simd = blend && simd_ok();
    for (int yy = 0; yy < dr.h; yy++) {
        const uint32_t *s = src->px + (sr.y + oy + yy) * src->stride + sr.x + ox;
        uint32_t *d = dst->px + (dr.y + yy) * dst->stride + dr.x;
        if (!blend) memcpy(d, s, dr.w * 4);
        else if (simd) simd_blend_span(d, s, dr.w);
        else for (int i = 0; i < dr.w; i++) d[i] = blend_px(d[i], s[i], ALPHA(s[i]));
    }
}
void gfx_blit(struct canvas *dst, int x, int y, const struct canvas *src) { gfx_blit_rect(dst, x, y, src, rect_make(0, 0, src->w, src->h), true); }
void gfx_blit_opaque(struct canvas *dst, int x, int y, const struct canvas *src) { gfx_blit_rect(dst, x, y, src, rect_make(0, 0, src->w, src->h), false); }
void gfx_copy_rect(struct canvas *dst, int dx, int dy, const struct canvas *src, int sx, int sy, int w, int h) { gfx_blit_rect(dst, dx, dy, src, rect_make(sx, sy, w, h), false); }

static inline uint32_t sample_bilinear(const struct canvas *src, int fx, int fy) {
    /* fx, fy in 16.16 fixed point, pixel-centre convention */
    int x0 = fx >> 16, y0 = fy >> 16;
    int ax = (fx >> 8) & 0xFF, ay = (fy >> 8) & 0xFF;
    int x1 = MIN(x0 + 1, src->w - 1), y1 = MIN(y0 + 1, src->h - 1);
    x0 = CLAMP(x0, 0, src->w - 1); y0 = CLAMP(y0, 0, src->h - 1);
    uint32_t p00 = src->px[y0 * src->stride + x0], p10 = src->px[y0 * src->stride + x1];
    uint32_t p01 = src->px[y1 * src->stride + x0], p11 = src->px[y1 * src->stride + x1];
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        /* premultiply by alpha to avoid dark fringes */
        int a00 = ALPHA(p00), a10 = ALPHA(p10), a01 = ALPHA(p01), a11 = ALPHA(p11);
        int c00 = (p00 >> sh) & 0xFF, c10 = (p10 >> sh) & 0xFF, c01 = (p01 >> sh) & 0xFF, c11 = (p11 >> sh) & 0xFF;
        if (sh != 24) { c00 = c00 * a00 / 255; c10 = c10 * a10 / 255; c01 = c01 * a01 / 255; c11 = c11 * a11 / 255; }
        int top = c00 * (256 - ax) + c10 * ax, bot = c01 * (256 - ax) + c11 * ax;
        int v = (top * (256 - ay) + bot * ay) >> 16;
        out |= (uint32_t)v << sh;
    }
    int a = ALPHA(out);
    if (a > 0 && a < 255) {
        uint32_t r = RED(out) * 255 / a, g = GREEN(out) * 255 / a, b = BLUE(out) * 255 / a;
        out = RGBA(MIN(r, 255), MIN(g, 255), MIN(b, 255), a);
    }
    return out;
}

void gfx_blit_scaled(struct canvas *dst, int x, int y, int w, int h, const struct canvas *src) {
    if (w <= 0 || h <= 0) return;
    if (w == src->w && h == src->h) { gfx_blit(dst, x, y, src); return; }
    struct rect dr = rect_intersect(rect_make(x, y, w, h), dst->clip);
    if (rect_empty(dr)) return;
    int sx_step = (src->w << 16) / w, sy_step = (src->h << 16) / h;
    for (int yy = dr.y; yy < dr.y + dr.h; yy++) {
        int fy = (yy - y) * sy_step + sy_step / 2 - (1 << 15);
        uint32_t *d = dst->px + yy * dst->stride;
        for (int xx = dr.x; xx < dr.x + dr.w; xx++) {
            int fx = (xx - x) * sx_step + sx_step / 2 - (1 << 15);
            uint32_t s = sample_bilinear(src, fx < 0 ? 0 : fx, fy < 0 ? 0 : fy);
            d[xx] = blend_px(d[xx], s, ALPHA(s));
        }
    }
}

void gfx_blit_tinted(struct canvas *dst, int x, int y, const struct canvas *src, color_t tint) {
    struct rect dr = rect_intersect(rect_make(x, y, src->w, src->h), dst->clip);
    if (rect_empty(dr)) return;
    uint32_t ta = ALPHA(tint);
    bool simd = simd_ok();
    for (int yy = 0; yy < dr.h; yy++) {
        const uint32_t *s = src->px + (dr.y - y + yy) * src->stride + (dr.x - x);
        uint32_t *d = dst->px + (dr.y + yy) * dst->stride + dr.x;
        if (simd) simd_blend_tint(d, s, tint, ta, dr.w);
        else for (int i = 0; i < dr.w; i++) d[i] = blend_px(d[i], tint, ALPHA(s[i]) * ta / 255);
    }
}
void gfx_blit_scaled_tinted(struct canvas *dst, int x, int y, int w, int h, const struct canvas *src, color_t tint) {
    struct rect dr = rect_intersect(rect_make(x, y, w, h), dst->clip);
    if (rect_empty(dr)) return;
    int sx_step = (src->w << 16) / w, sy_step = (src->h << 16) / h;
    uint32_t ta = ALPHA(tint);
    for (int yy = dr.y; yy < dr.y + dr.h; yy++) {
        int fy = (yy - y) * sy_step + sy_step / 2 - (1 << 15);
        uint32_t *d = dst->px + yy * dst->stride;
        for (int xx = dr.x; xx < dr.x + dr.w; xx++) {
            int fx = (xx - x) * sx_step + sx_step / 2 - (1 << 15);
            uint32_t s = sample_bilinear(src, fx < 0 ? 0 : fx, fy < 0 ? 0 : fy);
            d[xx] = blend_px(d[xx], tint, ALPHA(s) * ta / 255);
        }
    }
}

/* Blit with rounded corners cut out of the source (used for window bodies). */
void gfx_blit_rounded(struct canvas *dst, int x, int y, const struct canvas *src, int r, int mask) {
    gfx_blit(dst, x, y, src);
    if (r <= 0) return;
    /* re-blend the corner pixels: we can't "un-draw", so callers composite in order:
     * background first, then this. We handle corners by drawing src pixels with coverage. */
}

/* Soft drop shadow of a rounded rect. Pixels inside 'skip' (the area the window
 * itself will cover, minus its rounded corners) are left untouched for speed. */
void gfx_shadow_ex(struct canvas *c, int x, int y, int w, int h, int radius, int spread, int max_alpha, struct rect skip) {
    struct rect band = rect_intersect(rect_make(x - spread, y - spread, w + 2 * spread, h + 2 * spread), c->clip);
    if (rect_empty(band)) return;
    int hw = w / 2, hh = h / 2;
    int cx16 = (x + hw) * 16 + (w & 1 ? 8 : 0), cy16 = (y + hh) * 16 + (h & 1 ? 8 : 0);
    int ex = hw * 16 - radius * 16, ey = hh * 16 - radius * 16;
    int sx0 = skip.x, sx1 = skip.x + skip.w, sy0 = skip.y, sy1 = skip.y + skip.h;
    for (int yy = band.y; yy < band.y + band.h; yy++) {
        uint32_t *row = c->px + yy * c->stride;
        int dy = (yy * 16 + 8) - cy16; if (dy < 0) dy = -dy; dy -= ey; if (dy < 0) dy = 0;
        bool mid_row = yy >= sy0 + radius && yy < sy1 - radius;
        for (int xx = band.x; xx < band.x + band.w; xx++) {
            if (skip.w > 0 && xx >= sx0 && xx < sx1 && yy >= sy0 && yy < sy1) {
                if (mid_row) { xx = sx1 - 1; continue; }
                if (xx >= sx0 + radius && xx < sx1 - radius) { xx = sx1 - radius - 1; continue; }
            }
            int dx = (xx * 16 + 8) - cx16; if (dx < 0) dx = -dx; dx -= ex; if (dx < 0) dx = 0;
            int d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy)) - radius * 16;
            if (d < 0) d = 0;
            int t = spread * 16 - d;
            if (t <= 0) continue;
            int a = max_alpha * t / (spread * 16);
            a = a * t / (spread * 16);
            row[xx] = blend_px(row[xx], 0xFF000000, a);
        }
    }
}
/* coverage (0..256) of pixel (px,py) by a rounded rectangle, anti-aliased */
static int rrect_cov(int px, int py, struct rect r, int rad) {
    int hx = r.w * 8, hy = r.h * 8;                        /* half extents, 1/16 px */
    int dx = (px * 16 + 8) - (r.x * 16 + hx), dy = (py * 16 + 8) - (r.y * 16 + hy);
    if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    int qx = dx - (hx - rad * 16), qy = dy - (hy - rad * 16);
    int ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    int outside = isqrt64((uint64_t)((int64_t)ox * ox + (int64_t)oy * oy)) + MIN(MAX(qx, qy), 0) - rad * 16;   /* signed distance */
    int cov = 8 - outside;                                  /* half-pixel ramp */
    return cov <= 0 ? 0 : cov >= 16 ? 256 : cov * 16;
}

/* Drop shadow of a rounded rect (offset by dy) that is only visible outside the shape itself,
 * for translucent surfaces whose body must not be darkened. */
void gfx_shadow_outside(struct canvas *c, struct rect r, int radius, int spread, int max_alpha, int dy) {
    struct rect band = rect_intersect(rect_make(r.x - spread, r.y + dy - spread, r.w + 2 * spread, r.h + 2 * spread), c->clip);
    struct rect sh = rect_make(r.x, r.y + dy, r.w, r.h);
    for (int yy = band.y; yy < band.y + band.h; yy++) {
        uint32_t *row = c->px + yy * c->stride;
        for (int xx = band.x; xx < band.x + band.w; xx++) {
            int inside = rrect_cov(xx, yy, r, radius);
            if (inside >= 256) continue;
            /* distance outside the shifted shape */
            int hx = sh.w * 8, hy = sh.h * 8;
            int ddx = (xx * 16 + 8) - (sh.x * 16 + hx), ddy = (yy * 16 + 8) - (sh.y * 16 + hy);
            if (ddx < 0) ddx = -ddx; if (ddy < 0) ddy = -ddy;
            int qx = ddx - (hx - radius * 16), qy = ddy - (hy - radius * 16);
            int ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            int d = isqrt64((uint64_t)((int64_t)ox * ox + (int64_t)oy * oy)) + MIN(MAX(qx, qy), 0) - radius * 16;
            if (d < 0) d = 0;
            int t = spread * 16 - d;
            if (t <= 0) continue;
            int a = max_alpha * t / (spread * 16);
            a = a * t / (spread * 16);
            a = a * (256 - inside) / 256;
            row[xx] = blend_px(row[xx], 0xFF000000, a);
        }
    }
}

void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int radius, int spread, int max_alpha) {
    gfx_shadow_ex(c, x, y, w, h, radius, spread, max_alpha, rect_make(0, 0, 0, 0));
}

/* Blur r but keep the pixels outside a rounded rectangle (corner radius cr) untouched,
 * blending with anti-aliased coverage along the corner arcs. */
void gfx_blur_rect_rounded(struct canvas *c, struct rect r, int radius, int cr) {
    r = rect_intersect(r, rect_make(0, 0, c->w, c->h));
    if (rect_empty(r)) return;
    if (cr * 2 > r.w) cr = r.w / 2; if (cr * 2 > r.h) cr = r.h / 2;
    /* save the four corner squares */
    uint32_t *save = kmalloc((size_t)4 * cr * cr * 4 + 4);
    int cx0[4] = { r.x, r.x + r.w - cr, r.x, r.x + r.w - cr }, cy0[4] = { r.y, r.y, r.y + r.h - cr, r.y + r.h - cr };
    for (int k = 0; k < 4; k++) for (int y = 0; y < cr; y++) memcpy(save + (k * cr + y) * cr, c->px + (cy0[k] + y) * c->stride + cx0[k], cr * 4);
    gfx_blur_rect(c, r, radius);
    /* restore outside the arcs */
    for (int k = 0; k < 4; k++) for (int y = 0; y < cr; y++) for (int x = 0; x < cr; x++) {
        int px = cx0[k] + x, py = cy0[k] + y;
        int ccx = (k & 1) ? r.x + r.w - cr : r.x + cr, ccy = (k & 2) ? r.y + r.h - cr : r.y + cr;
        int cov = corner_cov(px, py, ccx, ccy, cr);
        /* corner_cov measures distance from the arc centre; pixels on the inner side of the centre are fully inside */
        if (((k & 1) && px < ccx) || (!(k & 1) && px >= ccx) || ((k & 2) && py < ccy) || (!(k & 2) && py >= ccy)) cov = 256;
        uint32_t orig = save[(k * cr + y) * cr + x];
        uint32_t *d = c->px + py * c->stride + px;
        *d = cov >= 256 ? *d : cov <= 0 ? orig : color_mix(orig, *d, cov);
    }
    kfree(save);
}

/* The SIMD passes split across CPUs: rows are independent in the horizontal
 * pass and columns in the vertical one (bands of 8 keep the AVX2 path whole). */
struct blur_job { uint32_t *px; int stride; uint32_t *tmp; uint8_t *sums; int w, h, radius, bands; };
static void blur_h_band(void *ctx, int i) {
    struct blur_job *j = ctx;
    int y0 = j->h * i / j->bands, y1 = j->h * (i + 1) / j->bands;
    simd_blur_h(j->px + (size_t)y0 * j->stride, j->stride, j->tmp + (size_t)y0 * j->w, j->w, j->w, y1 - y0, j->radius);
}
static void blur_v_band(void *ctx, int i) {
    struct blur_job *j = ctx;
    int x0 = (j->w * i / j->bands) & ~7, x1 = i == j->bands - 1 ? j->w : (j->w * (i + 1) / j->bands) & ~7;
    if (x1 > x0) simd_blur_v(j->tmp + x0, j->w, j->px + x0, j->stride, x1 - x0, j->h, j->radius, j->sums + (size_t)x0 * 16);
}
#define BLUR_PARALLEL_MIN_PX (32 * 1024)

/* Cheap box blur (2 passes) within r, used for translucent surfaces */
void gfx_blur_rect(struct canvas *c, struct rect r, int radius) {
    r = rect_intersect(r, rect_make(0, 0, c->w, c->h));
    if (rect_empty(r) || radius <= 0) return;
    uint32_t *tmp = kmalloc((size_t)r.w * r.h * 4);
    if (radius <= SIMD_BLUR_MAX_RADIUS && simd_ok()) {
        void *sums = kmalloc((size_t)r.w * 16);
        struct blur_job j = { c->px + r.y * c->stride + r.x, c->stride, tmp, sums, r.w, r.h, radius, 1 };
        if (r.w * r.h >= BLUR_PARALLEL_MIN_PX) j.bands = MAX(1, MIN(smp_workers(), MIN(r.h, r.w / 8)));
        for (int pass = 0; pass < 2; pass++) {
            smp_parallel_for(j.bands, blur_h_band, &j);
            smp_parallel_for(j.bands, blur_v_band, &j);
        }
        kfree(sums); kfree(tmp);
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        /* horizontal */
        for (int yy = 0; yy < r.h; yy++) {
            uint32_t *row = c->px + (r.y + yy) * c->stride + r.x;
            uint32_t sr = 0, sg = 0, sb = 0; int n = 0;
            for (int i = -radius; i <= radius; i++) { int xi = CLAMP(i, 0, r.w - 1); sr += RED(row[xi]); sg += GREEN(row[xi]); sb += BLUE(row[xi]); n++; }
            for (int xx = 0; xx < r.w; xx++) {
                tmp[yy * r.w + xx] = RGB(sr / n, sg / n, sb / n);
                int xo = CLAMP(xx - radius, 0, r.w - 1), xi = CLAMP(xx + radius + 1, 0, r.w - 1);
                sr += RED(row[xi]) - RED(row[xo]); sg += GREEN(row[xi]) - GREEN(row[xo]); sb += BLUE(row[xi]) - BLUE(row[xo]);
            }
        }
        /* vertical */
        for (int xx = 0; xx < r.w; xx++) {
            uint32_t sr = 0, sg = 0, sb = 0; int n = 0;
            for (int i = -radius; i <= radius; i++) { int yi = CLAMP(i, 0, r.h - 1); uint32_t p = tmp[yi * r.w + xx]; sr += RED(p); sg += GREEN(p); sb += BLUE(p); n++; }
            for (int yy = 0; yy < r.h; yy++) {
                c->px[(r.y + yy) * c->stride + r.x + xx] = RGB(sr / n, sg / n, sb / n);
                int yo = CLAMP(yy - radius, 0, r.h - 1), yi = CLAMP(yy + radius + 1, 0, r.h - 1);
                uint32_t pi = tmp[yi * r.w + xx], po = tmp[yo * r.w + xx];
                sr += RED(pi) - RED(po); sg += GREEN(pi) - GREEN(po); sb += BLUE(pi) - BLUE(po);
            }
        }
    }
    kfree(tmp);
}
