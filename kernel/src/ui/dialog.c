#include "ui.h"
#include "string.h"

/* Modal dialogs run on the calling thread with their own event loop. */
struct dlg { struct window *w; const char *title, *msg, *ok; bool has_field, destructive; struct textfield tf; int hover, pressed; int result; };
#define DW 420

static int wrap_lines(const char *s, int maxw, const char **starts, int *lens, int max) {
    int n = 0; const char *p = s;
    while (*p && n < max) {
        const char *line = p, *last_space = NULL; int w = 0;
        while (*p && *p != '\n') {
            const char *q = p; uint32_t cp = utf8_decode(&q);
            int cw = font_char_width(font_ui, cp);
            if (w + cw > maxw && last_space) { p = last_space + 1; break; }
            if (cp == ' ') last_space = p;
            w += cw; p = q;
        }
        int len = (int)(p - line);
        if (len > 0 && *p != '\n' && last_space && p == last_space + 1) len = (int)(last_space - line);
        starts[n] = line; lens[n] = len; n++;
        if (*p == '\n') p++;
    }
    return n;
}

static void dlg_draw(struct dlg *d) {
    struct canvas *c = window_begin_draw(d->w);
    gfx_fill(c, theme.bg);
    struct canvas *icon = icon_get("logo", 64);
    if (icon) gfx_blit_scaled_tinted(c, (DW - 48) / 2, 26, 48, 48, icon, theme.text);
    int y = 88;
    font_draw_centered(c, font_ui_semibold, rect_make(0, y, DW, 20), theme.text, d->title);
    y += 26;
    const char *starts[8]; int lens[8];
    int n = wrap_lines(d->msg, DW - 60, starts, lens, 8);
    for (int i = 0; i < n; i++) {
        int w = font_text_width_n(font_ui, starts[i], lens[i]);
        font_draw_n(c, font_ui, (DW - w) / 2, y, theme.text_secondary, starts[i], lens[i]);
        y += font_height(font_ui) + 2;
    }
    y += 10;
    if (d->has_field) { textfield_draw(c, &d->tf, rect_make(30, y, DW - 60, 26)); y += 36; }
    int bw = (DW - 60 - 10) / 2;
    ui_button(c, rect_make(30, y, bw, 28), "Cancel", BTN_NORMAL, d->pressed == 1, true);
    ui_button(c, rect_make(30 + bw + 10, y, bw, 28), d->ok, d->destructive ? BTN_DESTRUCTIVE : BTN_DEFAULT, d->pressed == 2, true);
    window_end_draw(d->w, NULL);
}

static int dlg_run(struct dlg *d) {
    int lines = 1; for (const char *p = d->msg; *p; p++) if (*p == '\n') lines++;
    int h = 88 + 26 + MAX(lines, (int)strlen(d->msg) / 50 + 1) * 17 + 10 + (d->has_field ? 36 : 0) + 28 + 24;
    d->w = window_create("", DW, h, WIN_DIALOG | WIN_UTILITY | WIN_FULL_TITLE);
    d->w->flags &= ~WIN_CLOSABLE;
    d->tf.focused = d->has_field;
    d->hover = 0; d->pressed = 0; d->result = -1;
    int y_btn = h - 28 - 24;
    int bw = (DW - 60 - 10) / 2;
    struct rect cancel = rect_make(30, y_btn, bw, 28), ok = rect_make(30 + bw + 10, y_btn, bw, 28);
    struct rect field = rect_make(30, h - 28 - 24 - 36, DW - 60, 26);
    dlg_draw(d);
    window_show(d->w);
    window_set_timer(d->w, 500);
    struct event ev;
    while (d->result < 0 && window_wait_event(d->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: d->result = 0; break;
        case EV_TIMER: dlg_draw(d); break;
        case EV_MOUSE_DOWN:
            if (rect_contains(cancel, ev.x, ev.y)) d->pressed = 1; else if (rect_contains(ok, ev.x, ev.y)) d->pressed = 2;
            else if (d->has_field && rect_contains(field, ev.x, ev.y)) textfield_mouse(&d->tf, field, ev.x, ev.y);
            dlg_draw(d); break;
        case EV_MOUSE_UP:
            if (d->pressed == 1 && rect_contains(cancel, ev.x, ev.y)) d->result = 0;
            else if (d->pressed == 2 && rect_contains(ok, ev.x, ev.y)) d->result = 1;
            d->pressed = 0; dlg_draw(d); break;
        case EV_KEY_DOWN:
            if (ev.key == KEY_ESC) d->result = 0;
            else if (ev.key == KEY_ENTER) d->result = 1;
            else if (d->has_field && textfield_key(&d->tf, &ev)) dlg_draw(d);
            break;
        }
    }
    window_destroy(d->w);
    return d->result > 0;
}

void dialog_message(const char *title, const char *message) {
    struct dlg d = { .title = title, .msg = message, .ok = "OK" };
    dlg_run(&d);
}
bool dialog_confirm(const char *title, const char *message, const char *ok_label, bool destructive) {
    struct dlg d = { .title = title, .msg = message, .ok = ok_label, .destructive = destructive };
    return dlg_run(&d) == 1;
}
bool dialog_prompt(const char *title, const char *message, const char *initial, char *out, size_t outsz) {
    struct dlg d = { .title = title, .msg = message, .ok = "OK", .has_field = true };
    textfield_init(&d.tf, initial);
    d.tf.sel_anchor = 0;
    if (dlg_run(&d) != 1) return false;
    strlcpy(out, d.tf.buf, outsz);
    return true;
}
