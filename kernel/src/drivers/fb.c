#include "gfx.h"
#include "mm.h"
#include "string.h"
#include <limine.h>
#include "gpu.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
static struct limine_framebuffer *fb;
static uint8_t *fb_mem;
static int fbw, fbh, fbpitch;
struct canvas *screen;

void fb_init(void) {
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count == 0) panic("no framebuffer");
    fb = framebuffer_request.response->framebuffers[0];
    fb_mem = fb->address;
    fbw = fb->width; fbh = fb->height; fbpitch = fb->pitch;
    if (fb->bpp != 32) panic("framebuffer: unsupported bpp %d", fb->bpp);
    screen = canvas_create(fbw, fbh);
    kprintf("fb: %dx%d, pitch %d, bpp %d at %p\n", fbw, fbh, fbpitch, fb->bpp, fb_mem);
}
int fb_width(void) { return fbw; }
/* switch to a new scanout buffer (after a GPU mode set) */
void fb_set_mode(uint64_t phys, int w, int h, int pitch) {
    fb_mem = vmm_map_wc(phys, (size_t)pitch * h);
    fbw = w; fbh = h; fbpitch = pitch;
    canvas_destroy(screen);
    screen = canvas_create(w, h);
    extern void fb_reset_flip(void);
    fb_reset_flip();
}
int fb_height(void) { return fbh; }

/* With a page-flipping GPU, damage goes to the VRAM back buffer; the other
 * buffer's stale regions are remembered and refreshed after the flip. */
static struct rect stale[2][32]; static int nstale[2];
static int flip_side;              /* which 'stale' list belongs to the current back buffer */
static bool flip_pending;
void fb_reset_flip(void) { nstale[0] = nstale[1] = 0; flip_pending = false; }
static void stale_add(int side, struct rect r) {
    if (nstale[side] < 32) stale[side][nstale[side]++] = r;
    else { struct rect u = stale[side][0]; for (int i = 1; i < nstale[side]; i++) u = rect_union(u, stale[side][i]); stale[side][0] = rect_union(u, r); nstale[side] = 1; }
}
static void copy_rect_to(uint8_t *dst, int pitch, struct rect r) {
    for (int y = r.y; y < r.y + r.h; y++) memcpy(dst + y * pitch + r.x * 4, screen->px + y * screen->stride + r.x, r.w * 4);
}
void fb_flush(struct rect r) {
    r = rect_intersect(r, rect_make(0, 0, fbw, fbh));
    if (rect_empty(r)) return;
    struct gpu_device *g = gpu_primary();
    if (g && g->page_flip) {
        int pitch; uint8_t *back = g->back_buffer(g, &pitch);
        copy_rect_to(back, pitch, r);
        stale_add(flip_side ^ 1, r);     /* the front buffer will miss this update */
        flip_pending = true;
        return;
    }
    copy_rect_to(fb_mem, fbpitch, r);
}
void fb_present(void) {
    struct gpu_device *g = gpu_primary();
    if (!g || !g->page_flip || !flip_pending) return;
    g->flip(g);
    flip_pending = false;
    flip_side ^= 1;
    /* bring the new back buffer up to date */
    int pitch; uint8_t *back = g->back_buffer(g, &pitch);
    for (int i = 0; i < nstale[flip_side]; i++) copy_rect_to(back, pitch, rect_intersect(stale[flip_side][i], rect_make(0, 0, fbw, fbh)));
    nstale[flip_side] = 0;
}
void fb_flush_all(void) { fb_flush(rect_make(0, 0, fbw, fbh)); fb_present(); }

/* Locate a file inside the raw initrd tar (fonts/images are needed before the VFS exists) */
static const void *tar_find(const char *name, size_t *size) {
    const uint8_t *p = boot_info.initrd, *end = p + boot_info.initrd_size;
    while (p + 512 <= end && p[0]) {
        const char *n = (const char *)p;
        if (n[0] == '.' && n[1] == '/') n += 2;
        uint64_t sz = 0; for (int i = 0; i < 11; i++) sz = sz * 8 + (p[124 + i] - '0');
        if (!strcmp(n, name)) { *size = sz; return p + 512; }
        p += 512 + ALIGN_UP(sz, 512);
    }
    return NULL;
}

static struct canvas *splash_logo;
static struct font *splash_font;
static int splash_y;

void fb_boot_splash(void) {
    gfx_fill(screen, RGB(0x12, 0x12, 0x14));
    size_t sz; const void *d;
    if ((d = tar_find("sys/icons/logo-256.img", &sz))) splash_logo = image_from_memory(d, sz);
    if ((d = tar_find("sys/fonts/inter-regular-13.fnt", &sz))) splash_font = font_from_memory(d, sz);
    int lw = 120;
    if (splash_logo) gfx_blit_scaled_tinted(screen, (fbw - lw) / 2, fbh / 2 - lw / 2 - 40, lw, lw, splash_logo, RGB(255, 255, 255));
    splash_y = fbh / 2 + lw / 2 - 20;
    /* progress track */
    gfx_fill_rounded_rect(screen, fbw / 2 - 90, splash_y, 180, 6, 3, RGB(0x3a, 0x3a, 0x40));
    fb_flush_all();
}

/* replaces the progress bar with an error, for failures that stop the boot */
void fb_boot_error(const char *msg) {
    gfx_fill_rect(screen, 0, splash_y - 2, fbw, 40, RGB(0x12, 0x12, 0x14));
    if (splash_font) font_draw_centered(screen, splash_font, rect_make(0, splash_y, fbw, 20), RGB(0xFF, 0x6B, 0x60), msg);
    fb_flush(rect_make(0, splash_y - 2, fbw, 40));
    fb_present();   /* page-flipping GPUs (i915, radeon) only show the back buffer after a flip */
}

static int splash_step;
void fb_boot_progress(const char *msg) {
    if (!splash_font) return;
    splash_step++;
    int total = 8, w = 180 * MIN(splash_step, total) / total;
    gfx_fill_rounded_rect(screen, fbw / 2 - 90, splash_y, w, 6, 3, RGB(0xE0, 0xE0, 0xE4));
    gfx_fill_rect(screen, 0, splash_y + 16, fbw, 20, RGB(0x12, 0x12, 0x14));
    font_draw_centered(screen, splash_font, rect_make(0, splash_y + 16, fbw, 20), RGB(0x9a, 0x9a, 0xa0), msg);
    fb_flush(rect_make(0, splash_y - 2, fbw, 40));
    fb_present();   /* page-flipping GPUs (i915, radeon) only show the back buffer after a flip */
}
