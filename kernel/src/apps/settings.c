/* System Settings: appearance, wallpaper, displays, date & time, storage, about. */
#include "ui.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

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
};
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
        snprintf(buf, sizeof buf, "Software: inFlux %s", INFLUX_VERSION);
        font_draw(c, font_ui_semibold, cx, cy + 150, theme.text, buf);
        font_draw(c, font_ui, cx, cy + 172, theme.text_secondary, "Your system is up to date.");
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
        gfx_fill_rounded_rect(c, cx + (cw - 200) / 2, cy, 200, 125, 8, RGB(0x1C, 0x1C, 0x1E));
        gfx_vgradient(c, cx + (cw - 200) / 2 + 6, cy + 6, 188, 113, theme.wallpaper_top, theme.wallpaper_bottom);
        gfx_fill_rect(c, cx + (cw - 200) / 2 + 90, cy + 125, 20, 10, RGB(0x8E, 0x8E, 0x93));
        gfx_fill_rect(c, cx + (cw - 200) / 2 + 70, cy + 135, 60, 3, RGB(0x8E, 0x8E, 0x93));
        int ty = cy + 160;
        const char *labels[] = { "Resolution", "Color depth", "Refresh", "Framebuffer", "Menu bar", "Dock" };
        char res[32], mb[32], dk[32]; snprintf(res, sizeof res, "%d × %d", wm_screen_w(), wm_screen_h()); snprintf(mb, sizeof mb, "%d px", MENUBAR_H); snprintf(dk, sizeof dk, "%d px icons", DOCK_ICON);
        const char *vals[] = { res, "32-bit (8 bits per channel)", "Software composited", "Linear, write-combining", mb, dk };
        for (int i = 0; i < 6; i++) { font_draw_aligned(c, font_ui_semibold, rect_make(cx, ty, 120, 20), theme.text, labels[i], 2); font_draw(c, font_ui, cx + 132, ty + 3, theme.text_secondary, vals[i]); ty += 24; }
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
        font_draw(c, font_ui_small, cx, cy + 156, theme.text_secondary, "The hardware clock is read at boot and assumed to be in UTC.");
        break; }
    case PANE_STORAGE: {
        int sy = cy;
        uint64_t total = pmm_total_bytes(), used = pmm_used_bytes();
        for (int i = -1; i < vfs_mount_count(); i++) {
            char name[64], mp[PATH_MAX]; uint64_t t, u; const char *fs; const char *bus = "Memory";
            if (i < 0) { strcpy(name, "System (RAM disk)"); t = total; u = used; fs = "tmpfs"; strcpy(mp, "/"); }
            else { struct vnode *r = vfs_mount_at(i, mp, sizeof mp); strlcpy(name, r->name[0] ? r->name : vfs_basename(mp), sizeof name); t = r->fs_total; u = t > r->fs_free ? t - r->fs_free : 0; fs = r->fs_name ? r->fs_name : "?"; bus = "Disk"; }
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
        char brand[64]; cpu_get_brand(brand, sizeof brand);
        char mem[32]; snprintf(mem, sizeof mem, "%lu MB", pmm_total_bytes() >> 20);
        char disp[32]; snprintf(disp, sizeof disp, "%d × %d", wm_screen_w(), wm_screen_h());
        const char *labels[] = { "Name", "Processor", "Memory", "Display", "Boot loader", "Kernel" };
        const char *vals[] = { system_hostname, brand, mem, disp, "Limine", "inFlux x86-64, preemptive, multithreaded" };
        int ty = cy + 96;
        for (int i = 0; i < 6; i++) { font_draw_aligned(c, font_ui_semibold, rect_make(cx, ty, 90, 20), theme.text, labels[i], 2); font_draw_ellipsized(c, font_ui, cx + 102, ty + 3, cw - 110, theme.text_secondary, vals[i]); ty += 24; }
        font_draw(c, font_ui_small, cx, ty + 16, theme.text_tertiary, "This operating system was written entirely by Claude (Anthropic).");
        font_draw(c, font_ui_small, cx, ty + 32, theme.text_tertiary, "Icons from the Kora icon theme. Fonts: Inter and Source Code Pro.");
        break; }
    }
    window_end_draw(s->w, NULL);
}

static void click(struct settings *s, int x, int y) {
    int W = s->w->frame.w;
    if (x < SIDEBAR_W) { int i = (y - TITLEBAR_H - 12) / 32; if (y >= TITLEBAR_H + 12 && i >= 0 && i < NPANES) { s->pane = i; s->hostname.focused = false; } return; }
    int cx = SIDEBAR_W + 30, cy = TITLEBAR_H + 54, cw = W - cx - 30;
    switch (s->pane) {
    case PANE_GENERAL:
        if (rect_contains(rect_make(cx + 16, cy + 36, cw - 32, 26), x, y)) textfield_mouse(&s->hostname, rect_make(cx + 16, cy + 36, cw - 32, 26), x, y); else s->hostname.focused = false;
        if (rect_contains(rect_make(cx + cw - 16 - 90, cy + 92, 90, 26), x, y)) s->pressed = 1;
        break;
    case PANE_APPEARANCE:
        if (rect_contains(rect_make(cx, cy + 24, 120, 80), x, y)) { theme_apply(false, theme.accent_index); wm_theme_changed(); }
        if (rect_contains(rect_make(cx + 140, cy + 24, 120, 80), x, y)) { theme_apply(true, theme.accent_index); wm_theme_changed(); }
        for (int i = 0; i < 8; i++) if (rect_contains(rect_make(cx + i * 30, cy + 172, 22, 22), x, y)) { theme_apply(theme.dark, i); wm_theme_changed(); }
        if (rect_contains(rect_make(cx + 200, cy + 210, 40, 24), x, y)) { clock_24h = !clock_24h; wm_dirty_all(); }
        break;
    case PANE_WALLPAPER:
        for (size_t i = 0; i < ARRAY_LEN(wallpapers); i++) { int wx = cx + (i % 4) * 130, wy = cy + 30 + (i / 4) * 110; if (rect_contains(rect_make(wx, wy, 120, 80), x, y)) { theme_set_wallpaper(wallpapers[i].top, wallpapers[i].bottom); wm_theme_changed(); } }
        break;
    case PANE_DATETIME:
        if (rect_contains(rect_make(cx + cw - 16 - 180, cy + 100, 30, 26), x, y)) s->pressed = 2;
        if (rect_contains(rect_make(cx + cw - 16 - 30, cy + 100, 30, 26), x, y)) s->pressed = 3;
        break;
    }
}
static void release(struct settings *s, int x, int y) {
    int W = s->w->frame.w; int cx = SIDEBAR_W + 30, cy = TITLEBAR_H + 54, cw = W - cx - 30;
    if (s->pressed == 1 && rect_contains(rect_make(cx + cw - 16 - 90, cy + 92, 90, 26), x, y)) { strlcpy(system_hostname, s->hostname.buf, sizeof system_hostname); }
    if (s->pressed == 2 && rect_contains(rect_make(cx + cw - 16 - 180, cy + 100, 30, 26), x, y)) { tz_offset_minutes -= 30; if (tz_offset_minutes < -12 * 60) tz_offset_minutes = -12 * 60; wm_dirty_all(); }
    if (s->pressed == 3 && rect_contains(rect_make(cx + cw - 16 - 30, cy + 100, 30, 26), x, y)) { tz_offset_minutes += 30; if (tz_offset_minutes > 14 * 60) tz_offset_minutes = 14 * 60; wm_dirty_all(); }
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
        case EV_MENU: if (ev.cmd == CMD_QUIT || ev.cmd == CMD_CLOSE) quit = true; else if (ev.cmd >= SCMD_PANE && ev.cmd < SCMD_PANE + NPANES) s->pane = ev.cmd - SCMD_PANE; break;
        case EV_MOUSE_DOWN: if (ev.button == BTN_LEFT) click(s, ev.x, ev.y); break;
        case EV_MOUSE_UP: release(s, ev.x, ev.y); break;
        case EV_KEY_DOWN: if (s->pane == PANE_GENERAL && s->hostname.focused) { if (ev.key == KEY_ENTER) strlcpy(system_hostname, s->hostname.buf, sizeof system_hostname); else textfield_key(&s->hostname, &ev); } break;
        default: break;
        }
        if (!quit) draw(s);
    }
    window_destroy(s->w);
    kfree(s);
}
