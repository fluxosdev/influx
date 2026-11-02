/* Install inFlux: a small wizard around the install engine. */
#include "ui.h"
#include "installer.h"
#include "mm.h"
#include "string.h"

struct inst { struct window *w; int page; int selected; struct blockdev *disks[16]; int ndisks; int percent; char log[8][160]; int nlog; int pressed; int result; bool running; };

static void format_size(uint64_t b, char *out, size_t n) { if (b >= 1ull << 30) snprintf(out, n, "%lu.%lu GB", b >> 30, ((b >> 20) % 1024) / 103); else snprintf(out, n, "%lu MB", b >> 20); }
static struct rect btn_next(struct inst *s) { return rect_make(s->w->frame.w - 130, s->w->frame.h - 44, 110, 30); }
static struct rect btn_back(struct inst *s) { return rect_make(s->w->frame.w - 250, s->w->frame.h - 44, 110, 30); }

static void draw(struct inst *s) {
    struct canvas *c = window_begin_draw(s->w);
    int W = c->w, H = c->h;
    gfx_fill(c, theme.bg);
    gfx_fill_rect(c, 0, 0, 170, H, theme.sidebar_bg); gfx_vline(c, 169, 0, H, theme.separator);
    struct canvas *logo = icon_get("logo", 128);
    if (logo) gfx_blit_scaled_tinted(c, 45, 50, 80, 80, logo, theme.text);
    font_draw_centered(c, font_ui_semibold_large, rect_make(0, 140, 170, 20), theme.text, "inFlux");
    const char *steps[] = { "Choose a disk", "Confirm", "Install", "Finish" };
    for (int i = 0; i < 4; i++) { bool cur = s->page == i; gfx_fill_circle(c, 24, 200 + i * 26 + 8, 4, cur ? theme.accent : theme.text_tertiary); font_draw(c, cur ? font_ui_semibold : font_ui, 36, 200 + i * 26, cur ? theme.text : theme.text_secondary, steps[i]); }
    int x = 190, y = 24;
    char buf[200];
    switch (s->page) {
    case 0:
        font_draw(c, font_ui_title, x, y, theme.text, "Install inFlux");
        font_draw(c, font_ui, x, y + 36, theme.text_secondary, "Choose the disk to install to. Everything on it will be erased.");
        for (int i = 0; i < s->ndisks; i++) {
            struct rect r = rect_make(x, y + 70 + i * 48, W - x - 24, 42);
            bool sel = s->selected == i;
            gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 8, sel ? theme.accent : theme.bg_secondary);
            struct canvas *ic = icon_get("drive", 64); if (ic) gfx_blit_scaled(c, r.x + 8, r.y + 7, 28, 28, ic);
            char sz[32]; format_size(s->disks[i]->sectors * s->disks[i]->sector_size, sz, sizeof sz);
            snprintf(buf, sizeof buf, "%s  ·  %s", s->disks[i]->model[0] ? s->disks[i]->model : "Disk", sz);
            font_draw(c, font_ui_semibold, r.x + 44, r.y + 6, sel ? theme.accent_text : theme.text, buf);
            snprintf(buf, sizeof buf, "/dev/%s on %s", s->disks[i]->name, s->disks[i]->bus ? s->disks[i]->bus : "?");
            font_draw(c, font_ui_small, r.x + 44, r.y + 24, sel ? theme.accent_text : theme.text_secondary, buf);
        }
        if (!s->ndisks) font_draw(c, font_ui, x, y + 80, theme.text_tertiary, "No disks found.");
        ui_button(c, btn_next(s), "Continue", BTN_DEFAULT, s->pressed == 1, s->selected >= 0);
        break;
    case 1: {
        font_draw(c, font_ui_title, x, y, theme.text, "Ready to install");
        struct blockdev *d = s->disks[s->selected];
        char sz[32]; format_size(d->sectors * d->sector_size, sz, sizeof sz);
        snprintf(buf, sizeof buf, "inFlux will be installed on /dev/%s (%s, %s).", d->name, d->model, sz);
        font_draw(c, font_ui, x, y + 36, theme.text_secondary, buf);
        const char *lines[] = { "• GPT partition table with a 1 MB BIOS boot partition", "• 64 MB EFI System Partition (FAT32) with Limine, kernel and system image", "• btrfs root volume for your files, mounted at /home", "• Boots on both BIOS and UEFI firmware" };
        for (int i = 0; i < 4; i++) font_draw(c, font_ui, x, y + 70 + i * 22, theme.text, lines[i]);
        font_draw(c, font_ui_semibold, x, y + 170, RGB(0xFF, 0x3B, 0x30), "All existing data on this disk will be erased.");
        ui_button(c, btn_back(s), "Back", BTN_NORMAL, s->pressed == 2, true);
        ui_button(c, btn_next(s), "Install", BTN_DESTRUCTIVE, s->pressed == 1, true);
        break; }
    case 2: case 3: {
        font_draw(c, font_ui_title, x, y, theme.text, s->page == 2 ? "Installing…" : (s->result == 0 ? "Installation complete" : "Installation failed"));
        int bw = W - x - 24;
        gfx_fill_rounded_rect(c, x, y + 44, bw, 10, 5, theme.dark ? RGB(0x3A, 0x3A, 0x3E) : RGB(0xDD, 0xDD, 0xE0));
        gfx_fill_rounded_rect(c, x, y + 44, MAX(10, bw * s->percent / 100), 10, 5, s->result < 0 && s->page == 3 ? RGB(0xFF, 0x3B, 0x30) : theme.accent);
        snprintf(buf, sizeof buf, "%d%%", s->percent); font_draw(c, font_ui_small, x, y + 60, theme.text_secondary, buf);
        for (int i = 0; i < s->nlog; i++) font_draw_ellipsized(c, font_ui_small, x, y + 86 + i * 18, bw, i == s->nlog - 1 ? theme.text : theme.text_secondary, s->log[i]);
        if (s->page == 3) {
            if (s->result == 0) { font_draw(c, font_ui, x, H - 80, theme.text_secondary, "Remove the installation medium, then restart to boot from the new disk."); ui_button(c, btn_next(s), "Restart", BTN_DEFAULT, s->pressed == 1, true); ui_button(c, btn_back(s), "Close", BTN_NORMAL, s->pressed == 2, true); }
            else ui_button(c, btn_next(s), "Close", BTN_DEFAULT, s->pressed == 1, true);
        }
        break; }
    }
    window_end_draw(s->w, NULL);
}

static void progress(void *ctx, int pct, const char *m) {
    struct inst *s = ctx; s->percent = pct;
    if (s->nlog == 8) { memmove(s->log, s->log + 1, 7 * sizeof s->log[0]); s->nlog--; }
    strlcpy(s->log[s->nlog++], m, sizeof s->log[0]);
    draw(s);
}

static void refresh_disks(struct inst *s) {
    s->ndisks = 0;
    for (int i = 0; i < blockdev_count() && s->ndisks < 16; i++) { struct blockdev *b = blockdev_at(i); if (!b->parent) s->disks[s->ndisks++] = b; }
    if (s->selected >= s->ndisks) s->selected = s->ndisks ? 0 : -1;
}

void installer_main(void *arg) {
    struct inst *s = kzalloc(sizeof *s);
    s->selected = -1;
    s->w = window_create("Install inFlux", 640, 420, WIN_CLOSABLE | WIN_MINIMIZABLE | WIN_FULL_TITLE);
    refresh_disks(s);
    if (s->ndisks) s->selected = 0;
    draw(s);
    window_show(s->w);
    struct event ev; bool quit = false;
    while (!quit && window_wait_event(s->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: if (!s->running) quit = true; break;
        case EV_MENU: if (ev.cmd == CMD_QUIT && !s->running) quit = true; break;
        case EV_MOUSE_DOWN:
            if (s->running || ev.button != BTN_LEFT) break;
            if (s->page == 0) for (int i = 0; i < s->ndisks; i++) if (rect_contains(rect_make(190, 94 + i * 48, s->w->frame.w - 214, 42), ev.x, ev.y)) s->selected = i;
            if (rect_contains(btn_next(s), ev.x, ev.y)) s->pressed = 1; else if (rect_contains(btn_back(s), ev.x, ev.y)) s->pressed = 2;
            break;
        case EV_MOUSE_UP: {
            if (s->running) break;
            int p = s->pressed; s->pressed = 0;
            bool next = p == 1 && rect_contains(btn_next(s), ev.x, ev.y), back = p == 2 && rect_contains(btn_back(s), ev.x, ev.y);
            if (s->page == 0 && next && s->selected >= 0) s->page = 1;
            else if (s->page == 1 && back) s->page = 0;
            else if (s->page == 1 && next) {
                s->page = 2; s->running = true; s->nlog = 0; s->percent = 0; draw(s);
                s->result = install_run(s->disks[s->selected], progress, s);
                s->running = false; s->page = 3; refresh_disks(s);
            }
            else if (s->page == 3 && next) { if (s->result == 0) power_restart(); else quit = true; }
            else if (s->page == 3 && back) quit = true;
            break; }
        default: break;
        }
        if (!quit) draw(s);
    }
    window_destroy(s->w);
    kfree(s);
}
