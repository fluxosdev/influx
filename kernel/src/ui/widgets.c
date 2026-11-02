#include "ui.h"
#include "string.h"
#include "mm.h"

void ui_button(struct canvas *c, struct rect r, const char *label, int style, bool pressed, bool enabled) {
    color_t bg = theme.control_bg, fg = theme.text, border = theme.control_border;
    if (style == BTN_DEFAULT) { bg = theme.accent; fg = theme.accent_text; border = color_darken(theme.accent, 20); }
    if (style == BTN_DESTRUCTIVE) { bg = RGB(0xFF, 0x3B, 0x30); fg = RGB(255, 255, 255); border = RGB(0xD9, 0x2C, 0x22); }
    if (!enabled) { bg = theme.bg_secondary; fg = theme.text_disabled; border = theme.separator; }
    if (pressed && enabled) bg = color_darken(bg, 30);
    gfx_shadow(c, r.x, r.y + 1, r.w, r.h, 6, 2, 40);
    gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 6, bg);
    gfx_draw_rounded_rect(c, r.x, r.y, r.w, r.h, 6, border);
    font_draw_centered(c, font_ui, r, fg, label);
}

void ui_checkbox(struct canvas *c, int x, int y, bool checked, const char *label) {
    gfx_fill_rounded_rect(c, x, y, 16, 16, 4, checked ? theme.accent : theme.control_bg);
    gfx_draw_rounded_rect(c, x, y, 16, 16, 4, checked ? color_darken(theme.accent, 20) : theme.control_border);
    if (checked) { gfx_line(c, x + 4, y + 8, x + 7, y + 11, theme.accent_text); gfx_line(c, x + 7, y + 11, x + 12, y + 5, theme.accent_text); gfx_line(c, x + 4, y + 9, x + 7, y + 12, theme.accent_text); gfx_line(c, x + 7, y + 12, x + 12, y + 6, theme.accent_text); }
    if (label) font_draw(c, font_ui, x + 24, y + (16 - font_height(font_ui)) / 2, theme.text, label);
}

void ui_switch(struct canvas *c, int x, int y, bool on) {
    int w = 38, h = 22;
    gfx_fill_rounded_rect(c, x, y, w, h, h / 2, on ? theme.accent : (theme.dark ? RGB(0x50, 0x50, 0x54) : RGB(0xD9, 0xD9, 0xDE)));
    int kx = on ? x + w - h + 2 : x + 2;
    gfx_shadow(c, kx, y + 3, h - 4, h - 4, (h - 4) / 2, 2, 60);
    gfx_fill_circle(c, kx + (h - 4) / 2, y + h / 2, (h - 4) / 2, RGB(255, 255, 255));
}

void ui_scrollbar_v(struct canvas *c, struct rect track, int content, int view, int pos) {
    if (content <= view) return;
    int th = MAX(24, track.h * view / content);
    int ty = track.y + (track.h - th) * pos / (content - view);
    gfx_fill_rounded_rect(c, track.x + 3, ty, track.w - 6, th, (track.w - 6) / 2, theme.dark ? RGBA(255, 255, 255, 0x60) : RGBA(0, 0, 0, 0x50));
}

void ui_panel(struct canvas *c, struct rect r, color_t bg) {
    gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 8, bg);
    gfx_draw_rounded_rect(c, r.x, r.y, r.w, r.h, 8, theme.separator);
}
void ui_separator_h(struct canvas *c, int x, int y, int w) { gfx_hline(c, x, y, w, theme.separator); }
void ui_icon_label(struct canvas *c, int x, int y, struct canvas *icon, int iconsz, const char *label, color_t col, struct font *f) {
    if (icon) gfx_blit_scaled(c, x, y, iconsz, iconsz, icon);
    font_draw(c, f, x + iconsz + 8, y + (iconsz - font_height(f)) / 2, col, label);
}

/* ---- text field ---- */
void textfield_init(struct textfield *t, const char *initial) { memset(t, 0, sizeof *t); textfield_set(t, initial ? initial : ""); t->sel_anchor = -1; }
void textfield_set(struct textfield *t, const char *text) { strlcpy(t->buf, text, sizeof t->buf); t->len = strlen(t->buf); t->cursor = t->len; t->sel_anchor = -1; }

static void tf_sel(struct textfield *t, int *a, int *b) {
    if (t->sel_anchor < 0 || t->sel_anchor == t->cursor) { *a = *b = t->cursor; return; }
    *a = MIN(t->sel_anchor, t->cursor); *b = MAX(t->sel_anchor, t->cursor);
}
static void tf_delete_sel(struct textfield *t) {
    int a, b; tf_sel(t, &a, &b);
    if (a == b) return;
    memmove(t->buf + a, t->buf + b, t->len - b + 1);
    t->len -= b - a; t->cursor = a; t->sel_anchor = -1;
}

void textfield_draw(struct canvas *c, struct textfield *t, struct rect r) {
    gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 6, theme.input_bg);
    gfx_draw_rounded_rect(c, r.x, r.y, r.w, r.h, 6, t->focused ? theme.accent : theme.input_border);
    if (t->focused) gfx_draw_rounded_rect(c, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 7, color_with_alpha(theme.accent, 0x60));
    struct canvas sub = canvas_sub(c, rect_make(r.x + 8, r.y, r.w - 16, r.h));
    int ty = (r.h - font_height(font_ui)) / 2;
    int cx = font_text_width_n(font_ui, t->buf, t->cursor);
    if (cx - t->scroll > sub.w - 2) t->scroll = cx - sub.w + 2;
    if (cx - t->scroll < 0) t->scroll = cx;
    if (t->len == 0 && t->placeholder) font_draw(&sub, font_ui, 0, ty, theme.text_tertiary, t->placeholder);
    int a, b; tf_sel(t, &a, &b);
    if (a != b) {
        int sx = font_text_width_n(font_ui, t->buf, a), ex = font_text_width_n(font_ui, t->buf, b);
        gfx_fill_rect(&sub, sx - t->scroll, 2, ex - sx, r.h - 4, color_with_alpha(theme.accent, 0x60));
    }
    font_draw(&sub, font_ui, -t->scroll, ty, theme.text, t->buf);
    if (t->focused && ((system_ticks / 500) & 1) == 0) gfx_fill_rect(&sub, cx - t->scroll, 4, 1, r.h - 8, theme.text);
}

bool textfield_key(struct textfield *t, const struct event *ev) {
    if (ev->type != EV_KEY_DOWN) return false;
    bool shift = ev->mods & MOD_SHIFT, cmd = (ev->mods & (MOD_CMD | MOD_CTRL)) != 0;
    int k = ev->key; if (k >= 'A' && k <= 'Z') k += 32;
    if (cmd && k == 'a') { t->sel_anchor = 0; t->cursor = t->len; return true; }
    if (cmd && k == 'c') { int a, b; tf_sel(t, &a, &b); if (a != b) clipboard_set(t->buf + a, b - a); return true; }
    if (cmd && k == 'x') { int a, b; tf_sel(t, &a, &b); if (a != b) { clipboard_set(t->buf + a, b - a); tf_delete_sel(t); } return true; }
    if (cmd && k == 'v') { char *s = clipboard_get(); if (s) { tf_delete_sel(t); for (char *p = s; *p && t->len < (int)sizeof t->buf - 1; p++) { if (*p == '\n') continue; memmove(t->buf + t->cursor + 1, t->buf + t->cursor, t->len - t->cursor + 1); t->buf[t->cursor++] = *p; t->len++; } kfree(s); } return true; }
    switch (ev->key) {
    case KEY_LEFT: if (!shift && t->sel_anchor >= 0 && t->sel_anchor != t->cursor) { int a, b; tf_sel(t, &a, &b); t->cursor = a; t->sel_anchor = -1; return true; }
                   if (shift && t->sel_anchor < 0) t->sel_anchor = t->cursor; if (!shift) t->sel_anchor = -1; if (t->cursor > 0) t->cursor--; return true;
    case KEY_RIGHT: if (!shift && t->sel_anchor >= 0 && t->sel_anchor != t->cursor) { int a, b; tf_sel(t, &a, &b); t->cursor = b; t->sel_anchor = -1; return true; }
                    if (shift && t->sel_anchor < 0) t->sel_anchor = t->cursor; if (!shift) t->sel_anchor = -1; if (t->cursor < t->len) t->cursor++; return true;
    case KEY_HOME: if (shift && t->sel_anchor < 0) t->sel_anchor = t->cursor; if (!shift) t->sel_anchor = -1; t->cursor = 0; return true;
    case KEY_END: if (shift && t->sel_anchor < 0) t->sel_anchor = t->cursor; if (!shift) t->sel_anchor = -1; t->cursor = t->len; return true;
    case KEY_BACKSPACE: { int a, b; tf_sel(t, &a, &b); if (a != b) { tf_delete_sel(t); return true; } if (t->cursor > 0) { memmove(t->buf + t->cursor - 1, t->buf + t->cursor, t->len - t->cursor + 1); t->cursor--; t->len--; } return true; }
    case KEY_DELETE: { int a, b; tf_sel(t, &a, &b); if (a != b) { tf_delete_sel(t); return true; } if (t->cursor < t->len) { memmove(t->buf + t->cursor, t->buf + t->cursor + 1, t->len - t->cursor); t->len--; } return true; }
    }
    if (ev->ch >= 32 && ev->ch < 127 && !cmd) {
        tf_delete_sel(t);
        if (t->len < (int)sizeof t->buf - 1) { memmove(t->buf + t->cursor + 1, t->buf + t->cursor, t->len - t->cursor + 1); t->buf[t->cursor++] = (char)ev->ch; t->len++; }
        return true;
    }
    return false;
}
void textfield_mouse(struct textfield *t, struct rect r, int x, int y) {
    int rel = x - (r.x + 8) + t->scroll;
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i <= t->len; i++) { int w = font_text_width_n(font_ui, t->buf, i); int d = w > rel ? w - rel : rel - w; if (d < bestd) { bestd = d; best = i; } }
    t->cursor = best; t->sel_anchor = -1; t->focused = true;
}
