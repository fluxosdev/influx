/* System Settings: appearance, wallpaper, displays, date & time, storage, about. */
#include "ui.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "gpu.h"
#include "installer.h"
#include "simd.h"
#include "net.h"

#define SIDEBAR_W 200
enum { PANE_GENERAL, PANE_APPEARANCE, PANE_WALLPAPER, PANE_DISPLAYS, PANE_DATETIME, PANE_STORAGE, PANE_ABOUT, NPANES };
static const char *pane_names[NPANES] = { "General", "Appearance", "Wallpaper", "Displays", "Date & Time", "Storage", "About" };
static const color_t pane_colors[NPANES] = { 0xFF8E8E93, 0xFF1D1D1F, 0xFF0A84FF, 0xFF32ADE6, 0xFF5E5CE6, 0xFF8E8E93, 0xFF30B0C7 };

struct wp_preset { const char *name; color_t top, bottom; };
static const struct wp_preset wallpapers[] = {
    { "Steel Blue", 0xFF5894C4, 0xFF366A98 }, { "Midnight", 0xFF24365A, 0xFF0C1526 }, { "Forest", 0xFF3F8F60, 0xFF1E5236 },
    { "Sunset", 0xFFF09A5E, 0xFF8E3B6B }, { "Graphite", 0xFF5C5C62, 0xFF2A2A2E }, { "Lavender", 0xFF9484CC, 0xFF5B4A9A },
    { "Rose", 0xFFE08A9A, 0xFF9A3E5B }, { "Ocean", 0xFF2C8FB0, 0xFF114A66 },
};

struct settings {
    struct window *w;
    int pane;
    struct textfield hostname;
    int pressed;      /* pressed control id */
    int wp_index;
    /* Displays pane */
    bool confirming; uint64_t confirm_deadline; int prev_w, prev_h, new_w, new_h;
    char disp_status[160]; bool disp_status_error;
    bool dd_open; int dd_hover, dd_first;
};

/* ---- Displays pane: one layout shared by drawing and hit testing ---- */
#define MAX_MODES 16
enum { DPRESS_KEEP = 10, DPRESS_REVERT };
struct disp_layout {
    int cx, cy, cw;
    struct rect preview, panel;
    struct display_mode modes[MAX_MODES]; int nmodes;
    int current;                                         /* preset index matching the screen, or -1 */
    struct rect dropdown, list; int list_visible;
    struct rect note, banner, keep, revert;
};
static bool mode_matches(const struct display_mode *m, int w, int h) { return m->h == h && m->w >= w && m->w - w < 8; }
static void displays_layout(struct settings *s, struct disp_layout *L) {
    int W = s->w->frame.w, H = s->w->frame.h;
    L->cx = SIDEBAR_W + 30; L->cy = TITLEBAR_H + 54; L->cw = W - L->cx - 30;
    L->preview = rect_make(L->cx, L->cy, 150, 110);
    int py = L->cy + 132;
    L->nmodes = gpu_list_modes(L->modes, MAX_MODES);
    L->current = -1;
    for (int i = 0; i < L->nmodes; i++) if (mode_matches(&L->modes[i], wm_screen_w(), wm_screen_h())) L->current = i;
    L->dropdown = rect_make(L->cx + 110, py + 12, 220, 28);
    int ry = py + 52;
    L->note = rect_make(L->cx + 16, ry, L->cw - 32, 16);
    L->panel = rect_make(L->cx, py, L->cw, ry + 26 - py);
    L->banner = rect_make(L->cx, L->panel.y + L->panel.h + 10, L->cw, 44);
    L->keep = rect_make(L->banner.x + L->banner.w - 12 - 80, L->banner.y + 9, 80, 26);
    L->revert = rect_make(L->keep.x - 8 - 80, L->banner.y + 9, 80, 26);
    /* popup list, macOS style: the checked item sits over the control; clamp to the window, scroll only if it cannot fit */
    int n = L->nmodes, want = n * DROPDOWN_ITEM_H + 8;
    int top = TITLEBAR_H + 4, bottom = H - 8;
    int h = MIN(want, bottom - top);
    h = ((h - 8) / DROPDOWN_ITEM_H) * DROPDOWN_ITEM_H + 8;
    int sel = MAX(L->current, 0);
    int y = L->dropdown.y + (L->dropdown.h - DROPDOWN_ITEM_H) / 2 - 4 - sel * DROPDOWN_ITEM_H;
    y = CLAMP(y, top, bottom - h);
    L->list = rect_make(L->dropdown.x, y, L->dropdown.w, h);
    L->list_visible = (h - 8) / DROPDOWN_ITEM_H;
}

static void disp_set_status(struct settings *s, bool error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void disp_set_status(struct settings *s, bool error, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(s->disp_status, sizeof s->disp_status, fmt, ap); va_end(ap);
    s->disp_status_error = error;
}
static void disp_apply(struct settings *s, int w, int h) {
    if (s->confirming) return;
    struct gpu_device *g = gpu_primary();
    if (g && g->modeset) {
        int pw = wm_screen_w(), ph = wm_screen_h();
        if (w == pw && h == ph) { disp_set_status(s, false, "%d \u00d7 %d is already in use.", w, h); return; }
        if (!wm_set_resolution(w, h)) { disp_set_status(s, true, "The display does not support %d \u00d7 %d.", w, h); return; }
        s->confirming = true; s->confirm_deadline = system_ticks + 15000;
        s->prev_w = pw; s->prev_h = ph; s->new_w = w; s->new_h = h;
        s->disp_status[0] = 0;
    } else {
        int r = display_persist_resolution(w, h);
        if (r == 0) disp_set_status(s, false, "Saved. %d \u00d7 %d will be used from the next boot.", w, h);
        else disp_set_status(s, true, "This display only changes resolution at boot. Install inFlux to a disk to save a boot resolution.");
    }
}
static void disp_keep(struct settings *s) {
    if (!s->confirming) return;
    s->confirming = false;
    int r = display_persist_resolution(s->new_w, s->new_h);
    if (r == 0) disp_set_status(s, false, "Using %d \u00d7 %d. Saved as the boot resolution.", wm_screen_w(), wm_screen_h());
    else disp_set_status(s, false, "Using %d \u00d7 %d for this session.", wm_screen_w(), wm_screen_h());
}
static void disp_revert(struct settings *s, bool timed_out) {
    if (!s->confirming) return;
    s->confirming = false;
    wm_set_resolution(s->prev_w, s->prev_h);
    disp_set_status(s, false, "%s Restored %d \u00d7 %d.", timed_out ? "No confirmation received." : "Reverted.", wm_screen_w(), wm_screen_h());
}
static void dd_open(struct settings *s, struct disp_layout *L) {
    s->dd_open = true;
    s->dd_hover = L->current;
    s->dd_first = 0;
    if (s->dd_hover >= L->list_visible) s->dd_first = s->dd_hover - L->list_visible + 1;
}
static void dd_choose(struct settings *s, struct disp_layout *L, int i) {
    s->dd_open = false;
    if (i < 0 || i >= L->nmodes) return;
    disp_apply(s, L->modes[i].w, L->modes[i].h);
}
static void dd_scroll_to(struct settings *s, struct disp_layout *L) {
    if (s->dd_hover < s->dd_first) s->dd_first = s->dd_hover;
    if (s->dd_hover >= s->dd_first + L->list_visible) s->dd_first = s->dd_hover - L->list_visible + 1;
}
static bool dd_key(struct settings *s, struct event *ev) {
    struct disp_layout L; displays_layout(s, &L);
    int n = L.nmodes;
    if (!s->dd_open) return false;
    if (ev->key == KEY_ESC) { s->dd_open = false; return true; }
    if (ev->key == KEY_DOWN) { s->dd_hover = s->dd_hover < 0 ? 0 : MIN(n - 1, s->dd_hover + 1); dd_scroll_to(s, &L); return true; }
    if (ev->key == KEY_UP) { s->dd_hover = s->dd_hover < 0 ? n - 1 : MAX(0, s->dd_hover - 1); dd_scroll_to(s, &L); return true; }
    if (ev->key == KEY_HOME) { s->dd_hover = 0; dd_scroll_to(s, &L); return true; }
    if (ev->key == KEY_END) { s->dd_hover = n - 1; dd_scroll_to(s, &L); return true; }
    if (ev->key == KEY_ENTER || ev->key == KEY_SPACE) { dd_choose(s, &L, s->dd_hover); return true; }
    return true;   /* swallow everything else while open */
}
enum { SCMD_PANE = CMD_APP + 1 };
static const struct menu_item app_items[] = { MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit System Settings", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item view_items[] = { MENU("General", SCMD_PANE + 0, '1', MOD_CMD), MENU("Appearance", SCMD_PANE + 1, '2', MOD_CMD), MENU("Wallpaper", SCMD_PANE + 2, '3', MOD_CMD), MENU("Displays", SCMD_PANE + 3, '4', MOD_CMD), MENU("Date & Time", SCMD_PANE + 4, '5', MOD_CMD), MENU("Storage", SCMD_PANE + 5, '6', MOD_CMD), MENU("About", SCMD_PANE + 6, '7', MOD_CMD) };
static const struct menu menus[] = { { "File", app_items, ARRAY_LEN(app_items) }, { "View", view_items, ARRAY_LEN(view_items) } };
static const struct app_menus settings_menus = { menus, ARRAY_LEN(menus) };

static void draw_pane_icon(struct canvas *c, int pane, int x, int y, int sz) {
    gfx_fill_rounded_rect(c, x, y, sz, sz, sz / 4, pane_colors[pane]);
    color_t w = RGB(255, 255, 255);
    int cx = x + sz / 2, cy = y + sz / 2;
    switch (pane) {
    case PANE_GENERAL: gfx_draw_circle(c, cx, cy, sz / 3, w); gfx_fill_circle(c, cx, cy, sz / 8, w); break;
    case PANE_APPEARANCE: gfx_fill_circle(c, cx, cy, sz / 3, w); gfx_fill_rect(c, cx, cy - sz / 3, sz / 3 + 1, sz * 2 / 3 + 1, pane_colors[pane]); gfx_draw_circle(c, cx, cy, sz / 3, w); break;
    case PANE_WALLPAPER: gfx_fill_rounded_rect(c, x + 4, y + 5, sz - 8, sz - 10, 2, w); gfx_fill_rect(c, x + 6, y + 7, sz - 12, sz - 14, RGB(0x5A, 0x9A, 0xD8)); break;
    case PANE_DISPLAYS: gfx_fill_rounded_rect(c, x + 3, y + 4, sz - 6, sz - 10, 2, w); gfx_fill_rect(c, cx - 3, y + sz - 6, 6, 3, w); break;
    case PANE_DATETIME: gfx_draw_circle(c, cx, cy, sz / 3, w); gfx_line(c, cx, cy, cx, cy - sz / 4, w); gfx_line(c, cx, cy, cx + sz / 5, cy, w); break;
    case PANE_STORAGE: gfx_fill_rounded_rect(c, x + 3, cy - 4, sz - 6, 8, 2, w); gfx_fill_circle(c, x + sz - 7, cy, 1, pane_colors[pane]); break;
    case PANE_ABOUT: { struct canvas *logo = icon_get("logo", 64); if (logo) gfx_blit_scaled_tinted(c, x + 4, y + 4, sz - 8, sz - 8, logo, w); break; }
    }
}

static void format_size(uint64_t sz, char *out, size_t n) {
    if (sz < 1024ULL * 1024) snprintf(out, n, "%lu KB", sz / 1024);
    else if (sz < 1024ULL * 1024 * 1024) snprintf(out, n, "%lu MB", sz >> 20);
    else snprintf(out, n, "%lu.%lu GB", sz >> 30, ((sz >> 20) % 1024) / 103);
}

static void draw_preview_card(struct canvas *c, int x, int y, bool dark, bool selected) {
    int w = 120, h = 80;
    gfx_fill_rounded_rect(c, x, y, w, h, 8, dark ? RGB(0x2A, 0x3A, 0x55) : RGB(0x6B, 0xA6, 0xD6));
    gfx_fill_rounded_rect(c, x + 16, y + 20, w - 32, h - 28, 5, dark ? RGB(0x2A, 0x2A, 0x2E) : RGB(0xFF, 0xFF, 0xFF));
    gfx_fill_rounded_rect_corners(c, x + 16, y + 20, w - 32, 10, 5, dark ? RGB(0x3A, 0x3A, 0x3E) : RGB(0xE8, 0xE8, 0xEA), 3);
    gfx_fill_circle(c, x + 22, y + 25, 2, 0xFFFF5F57); gfx_fill_circle(c, x + 29, y + 25, 2, 0xFFFEBC2E); gfx_fill_circle(c, x + 36, y + 25, 2, 0xFF28C840);
    gfx_fill_rect(c, x, y, w, 6, dark ? RGBA(0, 0, 0, 0x80) : RGBA(255, 255, 255, 0xB0));
    if (selected) gfx_draw_rounded_rect(c, x - 2, y - 2, w + 4, h + 4, 10, theme.accent), gfx_draw_rounded_rect(c, x - 3, y - 3, w + 6, h + 6, 11, theme.accent);
}

static void draw(struct settings *s) {
    struct canvas *c = window_begin_draw(s->w);
    int W = c->w, H = c->h;
    gfx_fill(c, theme.bg);
    gfx_fill_rect(c, 0, 0, SIDEBAR_W, H, theme.sidebar_bg);
    gfx_vline(c, SIDEBAR_W - 1, 0, H, theme.separator);
    int y = TITLEBAR_H + 12;
    for (int i = 0; i < NPANES; i++) {
        bool sel = s->pane == i;
        if (sel) gfx_fill_rounded_rect(c, 8, y, SIDEBAR_W - 16, 30, 6, theme.accent);
        draw_pane_icon(c, i, 18, y + 5, 20);
        font_draw(c, font_ui, 48, y + (30 - font_height(font_ui)) / 2, sel ? theme.accent_text : theme.text, pane_names[i]);
        y += 32;
    }
    /* content */
    int cx = SIDEBAR_W + 30, cy = TITLEBAR_H + 10;
    font_draw(c, font_ui_title, cx, cy, theme.text, pane_names[s->pane]);
    cy += 44;
    int cw = W - cx - 30;
    char buf[160];
    switch (s->pane) {
    case PANE_GENERAL: {
        ui_panel(c, rect_make(cx, cy, cw, 130), theme.bg_secondary);
        font_draw(c, font_ui_semibold, cx + 16, cy + 14, theme.text, "Computer name");
        textfield_draw(c, &s->hostname, rect_make(cx + 16, cy + 36, cw - 32, 26));
        font_draw(c, font_ui_small, cx + 16, cy + 68, theme.text_secondary, "Other computers can see this name. It is also used in the shell prompt.");
        ui_button(c, rect_make(cx + cw - 16 - 90, cy + 92, 90, 26), "Apply", BTN_DEFAULT, s->pressed == 1, true);
        break; }
    case PANE_APPEARANCE: {
        font_draw(c, font_ui_semibold, cx, cy, theme.text, "Appearance");
        draw_preview_card(c, cx, cy + 24, false, !theme.dark);
        font_draw_centered(c, font_ui, rect_make(cx, cy + 108, 120, 18), theme.text_secondary, "Light");
        draw_preview_card(c, cx + 140, cy + 24, true, theme.dark);
        font_draw_centered(c, font_ui, rect_make(cx + 140, cy + 108, 120, 18), theme.text_secondary, "Dark");
        font_draw(c, font_ui_semibold, cx, cy + 148, theme.text, "Accent color");
        for (int i = 0; i < 8; i++) {
            int ax = cx + i * 30, ay = cy + 172;
            gfx_fill_circle(c, ax + 10, ay + 10, 9, accent_colors[i]);
            if (theme.accent_index == i) gfx_fill_circle(c, ax + 10, ay + 10, 3, RGB(255, 255, 255));
        }
        font_draw(c, font_ui, cx + 250, cy + 175, theme.text_secondary, accent_names[theme.accent_index]);
        font_draw(c, font_ui_semibold, cx, cy + 214, theme.text, "Menu bar clock");
        ui_switch(c, cx + 200, cy + 210, clock_24h);
        font_draw(c, font_ui, cx + 246, cy + 214, theme.text_secondary, "Use 24-hour time");
        break; }
    case PANE_WALLPAPER: {
        font_draw(c, font_ui, cx, cy, theme.text_secondary, "Choose a desktop gradient.");
        for (size_t i = 0; i < ARRAY_LEN(wallpapers); i++) {
            int wx = cx + (i % 4) * 130, wy = cy + 30 + (i / 4) * 110;
            bool sel = theme.wallpaper_top == wallpapers[i].top;
            gfx_vgradient(c, wx + 1, wy + 1, 118, 78, wallpapers[i].top, wallpapers[i].bottom);
            gfx_draw_rounded_rect(c, wx, wy, 120, 80, 6, sel ? theme.accent : theme.border);
            if (sel) gfx_draw_rounded_rect(c, wx - 2, wy - 2, 124, 84, 8, theme.accent);
            font_draw_centered(c, font_ui_small, rect_make(wx, wy + 84, 120, 16), theme.text_secondary, wallpapers[i].name);
        }
        break; }
    case PANE_DISPLAYS: {
        struct disp_layout L; displays_layout(s, &L);
        struct gpu_device *g = gpu_primary();
        /* monitor preview with the current aspect ratio */
        int mw = L.preview.w, mh = mw * wm_screen_h() / MAX(1, wm_screen_w()); if (mh > 90) { mh = 90; mw = mh * wm_screen_w() / MAX(1, wm_screen_h()); }
        int mx = L.preview.x + (L.preview.w - mw) / 2, my = L.preview.y;
        gfx_fill_rounded_rect(c, mx, my, mw, mh, 6, RGB(0x1C, 0x1C, 0x1E));
        gfx_vgradient(c, mx + 4, my + 4, mw - 8, mh - 8, theme.wallpaper_top, theme.wallpaper_bottom);
        gfx_fill_rect(c, mx + mw / 2 - 8, my + mh, 16, 8, RGB(0x8E, 0x8E, 0x93));
        gfx_fill_rect(c, mx + mw / 2 - 24, my + mh + 8, 48, 3, RGB(0x8E, 0x8E, 0x93));
        snprintf(buf, sizeof buf, "%d \u00d7 %d", wm_screen_w(), wm_screen_h());
        font_draw_centered(c, font_ui_semibold, rect_make(L.preview.x, my + mh + 12, L.preview.w, 18), theme.text, buf);
        /* adapter details */
        char vram[48], drv[96], clk[64];
        if (g && g->vram_bytes) snprintf(vram, sizeof vram, "%lu MB (%s)", g->vram_bytes >> 20, g->chip); else strcpy(vram, "unknown");
        snprintf(drv, sizeof drv, "%s%s%s", g ? g->driver : "software", g && g->page_flip ? ", page flipping" : "", g && g->hw_cursor ? ", hardware cursor" : "");
        if (g && g->engine_clock_khz && g->memory_clock_khz) snprintf(clk, sizeof clk, "%u MHz core, %u MHz memory", g->engine_clock_khz / 1000, g->memory_clock_khz / 1000);
        else if (g && g->engine_clock_khz) snprintf(clk, sizeof clk, "%u MHz core", g->engine_clock_khz / 1000);
        else strcpy(clk, "n/a");
        const char *labels[] = { "Graphics", "Driver", "VRAM", "Clocks", "Modes", "OpenGL" };
        const char *vals[] = { g ? g->name : "framebuffer", drv, vram, clk, g && g->modeset ? "Changeable now" : "Changeable at next boot", "1.1 software rasterizer" };
        int ix = L.cx + L.preview.w + 20, iw = L.cx + L.cw - ix;
        for (int i = 0; i < 6; i++) {
            font_draw_aligned(c, font_ui_semibold, rect_make(ix, L.cy + i * 19, 64, 18), theme.text, labels[i], 2);
            font_draw_ellipsized(c, font_ui, ix + 74, L.cy + i * 19 + 2, iw - 74, theme.text_secondary, vals[i]);
        }
        /* resolution panel */
        ui_panel(c, L.panel, theme.bg_secondary);
        font_draw(c, font_ui_semibold, L.cx + 16, L.dropdown.y + (L.dropdown.h - font_height(font_ui_semibold)) / 2, theme.text, "Resolution");
        char ddlabel[48];
        if (L.current >= 0) snprintf(ddlabel, sizeof ddlabel, "%d \u00d7 %d", L.modes[L.current].w, L.modes[L.current].h);
        else snprintf(ddlabel, sizeof ddlabel, "%d \u00d7 %d", wm_screen_w(), wm_screen_h());
        ui_dropdown(c, L.dropdown, ddlabel, s->dd_open, !s->confirming);
        const char *note = g && g->modeset ? "Changes apply immediately. Kept resolutions are also used at boot on an installed system."
                                           : "This display changes resolution at boot. The choice is saved to the installed system.";
        if (s->disp_status[0]) font_draw_ellipsized(c, font_ui_small, L.note.x, L.note.y, L.note.w, s->disp_status_error ? RGB(0xFF, 0x3B, 0x30) : theme.text_secondary, s->disp_status);
        else font_draw_ellipsized(c, font_ui_small, L.note.x, L.note.y, L.note.w, theme.text_tertiary, note);
        /* confirmation banner */
        if (s->confirming) {
            int left = (int)(((int64_t)s->confirm_deadline - (int64_t)system_ticks + 999) / 1000); if (left < 0) left = 0;
            struct rect b = L.banner;
            gfx_fill_rounded_rect(c, b.x, b.y, b.w, b.h, 8, color_with_alpha(theme.accent, 0x26));
            gfx_draw_rounded_rect(c, b.x, b.y, b.w, b.h, 8, color_with_alpha(theme.accent, 0x80));
            font_draw(c, font_ui_semibold, b.x + 14, b.y + 6, theme.text, "Keep this resolution?");
            snprintf(buf, sizeof buf, "Reverting to %d \u00d7 %d in %d second%s.", s->prev_w, s->prev_h, left, left == 1 ? "" : "s");
            font_draw(c, font_ui_small, b.x + 14, b.y + 24, theme.text_secondary, buf);
            ui_button(c, L.revert, "Revert", BTN_NORMAL, s->pressed == DPRESS_REVERT, true);
            ui_button(c, L.keep, "Keep", BTN_DEFAULT, s->pressed == DPRESS_KEEP, true);
        }
        if (s->dd_open) {
            const char *items[MAX_MODES]; char names[MAX_MODES][24];
            for (int i = 0; i < L.nmodes; i++) { snprintf(names[i], sizeof names[i], "%d \u00d7 %d", L.modes[i].w, L.modes[i].h); items[i] = names[i]; }
            ui_dropdown_list(c, L.list, items, L.nmodes, s->dd_first, s->dd_hover, L.current);
        }
        break; }
    case PANE_DATETIME: {
        struct rtc_time t; unix_to_datetime(time_unix() + tz_offset_minutes * 60, &t);
        static const char *wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        static const char *mon[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", t.hour, t.minute, t.second);
        font_draw(c, font_ui_huge, cx, cy, theme.text, buf);
        snprintf(buf, sizeof buf, "%s, %s %d, %d", wd[day_of_week(t.year, t.month, t.day)], mon[(t.month - 1) % 12], t.day, t.year);
        font_draw(c, font_ui_large, cx, cy + 44, theme.text_secondary, buf);
        ui_panel(c, rect_make(cx, cy + 84, cw, 60), theme.bg_secondary);
        font_draw(c, font_ui_semibold, cx + 16, cy + 104, theme.text, "Time zone offset");
        ui_button(c, rect_make(cx + cw - 16 - 180, cy + 100, 30, 26), "−", BTN_NORMAL, s->pressed == 2, true);
        int64_t off = tz_offset_minutes; snprintf(buf, sizeof buf, "UTC%s%02ld:%02ld", off < 0 ? "−" : "+", (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
        font_draw_centered(c, font_ui, rect_make(cx + cw - 16 - 146, cy + 100, 110, 26), theme.text, buf);
        ui_button(c, rect_make(cx + cw - 16 - 30, cy + 100, 30, 26), "+", BTN_NORMAL, s->pressed == 3, true);
        ui_panel(c, rect_make(cx, cy + 156, cw, 60), theme.bg_secondary);
        font_draw(c, font_ui_semibold, cx + 16, cy + 166, theme.text, "Set time automatically");
        if (!sntp_enabled) snprintf(buf, sizeof buf, "Off: the clock keeps the hardware clock's time.");
        else if (!sntp_last_sync) snprintf(buf, sizeof buf, "Waiting for a network connection to reach a time server.");
        else {
            struct rtc_time ls; unix_to_datetime(sntp_last_sync + tz_offset_minutes * 60, &ls);
            snprintf(buf, sizeof buf, "Last synchronised with %s at %02d:%02d.", sntp_last_server, ls.hour, ls.minute);
        }
        font_draw(c, font_ui_small, cx + 16, cy + 188, theme.text_secondary, buf);
        ui_switch(c, cx + cw - 16 - 38, cy + 175, sntp_enabled);
        font_draw(c, font_ui_small, cx, cy + 228, theme.text_secondary, "The hardware clock is kept in UTC; it is set whenever the time is synchronised.");
        break; }
    case PANE_STORAGE: {
        int sy = cy;
        uint64_t total = pmm_total_bytes(), used = pmm_used_bytes();
        for (int i = -1; i < vfs_mount_count(); i++) {
            char name[64], mp[PATH_MAX]; uint64_t t, u; const char *fs; const char *bus = "Memory";
            if (i < 0) { strcpy(name, "System (RAM disk)"); t = total; u = used; fs = "tmpfs"; strcpy(mp, "/"); }
            else { struct vnode *r = vfs_mount_at(i, mp, sizeof mp); if (!r) continue; strlcpy(name, r->name[0] ? r->name : vfs_basename(mp), sizeof name); t = r->fs_total; u = t > r->fs_free ? t - r->fs_free : 0; fs = r->fs_name ? r->fs_name : "?"; bus = "Disk"; }
            ui_panel(c, rect_make(cx, sy, cw, 70), theme.bg_secondary);
            struct canvas *ic = icon_get(i < 0 ? "computer" : "drive", 64);
            if (ic) gfx_blit_scaled(c, cx + 14, sy + 13, 44, 44, ic);
            font_draw(c, font_ui_semibold, cx + 70, sy + 12, theme.text, name);
            char a[32], b[32]; format_size(u, a, sizeof a); format_size(t, b, sizeof b);
            snprintf(buf, sizeof buf, "%s of %s used  ·  %s  ·  mounted at %s", a, b, fs, mp);
            font_draw(c, font_ui_small, cx + 70, sy + 30, theme.text_secondary, buf);
            int bw = cw - 90;
            gfx_fill_rounded_rect(c, cx + 70, sy + 48, bw, 10, 5, theme.dark ? RGB(0x3A, 0x3A, 0x3E) : RGB(0xDD, 0xDD, 0xE0));
            if (t) gfx_fill_rounded_rect(c, cx + 70, sy + 48, MAX(10, (int)(bw * u / t)), 10, 5, i < 0 ? theme.accent : RGB(0x34, 0xC7, 0x59));
            UNUSED(bus);
            sy += 80;
        }
        break; }
    case PANE_ABOUT: {
        struct canvas *logo = icon_get("logo", 128);
        if (logo) gfx_blit_scaled_tinted(c, cx, cy, 72, 72, logo, theme.text);
        font_draw(c, font_ui_title, cx + 90, cy + 8, theme.text, "inFlux");
        snprintf(buf, sizeof buf, "Version %s", INFLUX_VERSION); font_draw(c, font_ui, cx + 90, cy + 40, theme.text_secondary, buf);
        char brand[64], cpu[96]; cpu_get_brand(brand, sizeof brand);
        snprintf(cpu, sizeof cpu, cpu_count > 1 ? "%s (%d cores)" : "%s", brand, cpu_count);
        char isa[64]; cpu_simd_describe(isa, sizeof isa);
        char mem[32]; snprintf(mem, sizeof mem, "%lu MB", pmm_total_bytes() >> 20);
        char disp[32]; snprintf(disp, sizeof disp, "%d × %d", wm_screen_w(), wm_screen_h());
        const char *labels[] = { "Name", "Processor", "Extensions", "Memory", "Display", "Boot loader", "Kernel" };
        const char *vals[] = { system_hostname, cpu, isa, mem, disp, "Limine", "inFlux x86-64, preemptive, multithreaded" };
        int ty = cy + 96;
        for (int i = 0; i < 7; i++) { font_draw_aligned(c, font_ui_semibold, rect_make(cx, ty, 90, 20), theme.text, labels[i], 2); font_draw_ellipsized(c, font_ui, cx + 102, ty + 3, cw - 110, theme.text_secondary, vals[i]); ty += 24; }
        font_draw(c, font_ui_small, cx, ty + 16, theme.text_tertiary, "This operating system was written entirely by Claude (Anthropic).");
        font_draw(c, font_ui_small, cx, ty + 32, theme.text_tertiary, "Icons from the Kora icon theme. Fonts: Inter and Source Code Pro.");
        break; }
    }
    window_end_draw(s->w, NULL);
}

static void click(struct settings *s, int x, int y) {
    int W = s->w->frame.w;
    if (x < SIDEBAR_W) { int i = (y - TITLEBAR_H - 12) / 32; if (y >= TITLEBAR_H + 12 && i >= 0 && i < NPANES) { if (i != PANE_DISPLAYS) disp_revert(s, false); s->dd_open = false; s->pane = i; s->hostname.focused = false; } return; }
    int cx = SIDEBAR_W + 30, cy = TITLEBAR_H + 54, cw = W - cx - 30;
    switch (s->pane) {
    case PANE_GENERAL:
        if (rect_contains(rect_make(cx + 16, cy + 36, cw - 32, 26), x, y)) textfield_mouse(&s->hostname, rect_make(cx + 16, cy + 36, cw - 32, 26), x, y); else s->hostname.focused = false;
        if (rect_contains(rect_make(cx + cw - 16 - 90, cy + 92, 90, 26), x, y)) s->pressed = 1;
        break;
    case PANE_APPEARANCE:
        if (rect_contains(rect_make(cx, cy + 24, 120, 80), x, y)) { theme_apply(false, theme.accent_index); wm_theme_changed(); prefs_save(); }
        if (rect_contains(rect_make(cx + 140, cy + 24, 120, 80), x, y)) { theme_apply(true, theme.accent_index); wm_theme_changed(); prefs_save(); }
        for (int i = 0; i < 8; i++) if (rect_contains(rect_make(cx + i * 30, cy + 172, 22, 22), x, y)) { theme_apply(theme.dark, i); wm_theme_changed(); prefs_save(); }
        if (rect_contains(rect_make(cx + 200, cy + 210, 40, 24), x, y)) { clock_24h = !clock_24h; wm_dirty_all(); prefs_save(); }
        break;
    case PANE_WALLPAPER:
        for (size_t i = 0; i < ARRAY_LEN(wallpapers); i++) { int wx = cx + (i % 4) * 130, wy = cy + 30 + (i / 4) * 110; if (rect_contains(rect_make(wx, wy, 120, 80), x, y)) { theme_set_wallpaper(wallpapers[i].top, wallpapers[i].bottom); wm_theme_changed(); prefs_save(); } }
        break;
    case PANE_DISPLAYS: {
        struct disp_layout L; displays_layout(s, &L);
        if (s->confirming) {
            if (rect_contains(L.keep, x, y)) s->pressed = DPRESS_KEEP;
            else if (rect_contains(L.revert, x, y)) s->pressed = DPRESS_REVERT;
            break;
        }
        if (s->dd_open) {
            if (rect_contains(L.list, x, y)) { int k = (y - L.list.y - 4) / DROPDOWN_ITEM_H; if (k >= 0 && k < L.list_visible) dd_choose(s, &L, s->dd_first + k); }
            else s->dd_open = false;
            break;
        }
        if (rect_contains(L.dropdown, x, y)) dd_open(s, &L);
        break; }
    case PANE_DATETIME:
        if (rect_contains(rect_make(cx + cw - 16 - 180, cy + 100, 30, 26), x, y)) s->pressed = 2;
        if (rect_contains(rect_make(cx + cw - 16 - 30, cy + 100, 30, 26), x, y)) s->pressed = 3;
        if (rect_contains(rect_make(cx + cw - 16 - 38, cy + 175, 38, 22), x, y)) { sntp_enabled = !sntp_enabled; prefs_save(); wm_dirty_all(); }
        break;
    }
}
static void release(struct settings *s, int x, int y) {
    int W = s->w->frame.w; int cx = SIDEBAR_W + 30, cy = TITLEBAR_H + 54, cw = W - cx - 30;
    if (s->pane == PANE_DISPLAYS && s->pressed >= DPRESS_KEEP) {
        struct disp_layout L; displays_layout(s, &L);
        int p = s->pressed; s->pressed = 0;
        if (p == DPRESS_KEEP && rect_contains(L.keep, x, y)) disp_keep(s);
        else if (p == DPRESS_REVERT && rect_contains(L.revert, x, y)) disp_revert(s, false);
        return;
    }
    if (s->pressed == 1 && rect_contains(rect_make(cx + cw - 16 - 90, cy + 92, 90, 26), x, y)) { strlcpy(system_hostname, s->hostname.buf, sizeof system_hostname); prefs_save(); }
    if (s->pressed == 2 && rect_contains(rect_make(cx + cw - 16 - 180, cy + 100, 30, 26), x, y)) { tz_offset_minutes -= 30; if (tz_offset_minutes < -12 * 60) tz_offset_minutes = -12 * 60; wm_dirty_all(); prefs_save(); }
    if (s->pressed == 3 && rect_contains(rect_make(cx + cw - 16 - 30, cy + 100, 30, 26), x, y)) { tz_offset_minutes += 30; if (tz_offset_minutes > 14 * 60) tz_offset_minutes = 14 * 60; wm_dirty_all(); prefs_save(); }
    s->pressed = 0;
}

void settings_main(void *arg) {
    struct settings *s = kzalloc(sizeof *s);
    textfield_init(&s->hostname, system_hostname);
    s->w = window_create("System Settings", 760, 520, WIN_CLOSABLE | WIN_MINIMIZABLE | WIN_FULL_TITLE);
    window_set_menus(s->w, &settings_menus);
    draw(s);
    window_show(s->w);
    window_set_timer(s->w, 500);
    struct event ev; bool quit = false;
    while (!quit && window_wait_event(s->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: quit = true; break;
        case EV_MENU: if (ev.cmd == CMD_QUIT || ev.cmd == CMD_CLOSE) quit = true; else if (ev.cmd >= SCMD_PANE && ev.cmd < SCMD_PANE + NPANES) { if (ev.cmd - SCMD_PANE != PANE_DISPLAYS) disp_revert(s, false); s->pane = ev.cmd - SCMD_PANE; } break;
        case EV_MOUSE_DOWN: if (ev.button == BTN_LEFT) click(s, ev.x, ev.y); break;
        case EV_MOUSE_UP: release(s, ev.x, ev.y); break;
        case EV_KEY_DOWN:
            if (s->pane == PANE_GENERAL && s->hostname.focused) { if (ev.key == KEY_ENTER) strlcpy(system_hostname, s->hostname.buf, sizeof system_hostname); else textfield_key(&s->hostname, &ev); }
            else if (s->pane == PANE_DISPLAYS && s->confirming) { if (ev.key == KEY_ENTER) disp_keep(s); else if (ev.key == KEY_ESC) disp_revert(s, false); }
            else if (s->pane == PANE_DISPLAYS && s->dd_open) dd_key(s, &ev);
            else if (s->pane == PANE_DISPLAYS && (ev.key == KEY_SPACE || ev.key == KEY_DOWN || ev.key == KEY_ENTER)) { struct disp_layout L; displays_layout(s, &L); dd_open(s, &L); }
            break;
        case EV_MOUSE_MOVE:
            if (s->pane == PANE_DISPLAYS && s->dd_open) { struct disp_layout L; displays_layout(s, &L); int k = (ev.y - L.list.y - 4) / DROPDOWN_ITEM_H; s->dd_hover = rect_contains(L.list, ev.x, ev.y) && k >= 0 && k < L.list_visible ? s->dd_first + k : -1; }
            break;
        case EV_MOUSE_SCROLL:
            if (s->pane == PANE_DISPLAYS && s->dd_open) { struct disp_layout L; displays_layout(s, &L); s->dd_first = CLAMP(s->dd_first - ev.scroll, 0, MAX(0, L.nmodes - L.list_visible)); }
            break;
        case EV_TIMER: if (s->confirming && (int64_t)system_ticks >= (int64_t)s->confirm_deadline) disp_revert(s, true); break;
        case EV_BLUR: s->dd_open = false; break;
        default: break;
        }
        if (!quit) draw(s);
    }
    disp_revert(s, false);   /* never leave an unconfirmed mode behind */
    window_destroy(s->w);
    kfree(s);
}
