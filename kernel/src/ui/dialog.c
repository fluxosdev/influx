#include "ui.h"
#include "string.h"

/* Modal dialogs run on the calling thread with their own event loop. */
#define DW 420
#define DLG_MAX_LINES 24
struct dlg {
    struct window *w; const char *title, *msg, *ok; bool has_field, destructive, alert; struct textfield tf; int hover, pressed; int result;
    /* layout, computed once and shared by drawing and hit-testing */
    const char *starts[DLG_MAX_LINES]; int lens[DLG_MAX_LINES]; int nlines;
    int text_y, h; struct rect field, cancel, ok_btn;
};

/* Word-wrap at spaces; a word wider than the line (a long path) is broken mid-word. */
static int wrap_lines(const char *s, int maxw, const char **starts, int *lens, int max) {
    int n = 0; const char *p = s;
    while (*p && n < max) {
        const char *line = p, *last_space = NULL; int w = 0;
        while (*p && *p != '\n') {
            const char *q = p; uint32_t cp = utf8_decode(&q);
            int cw = font_char_width(font_ui, cp);
            if (w + cw > maxw) {
                if (last_space) { p = last_space + 1; break; }
                if (p > line) break;
            }
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

static void dlg_layout(struct dlg *d) {
    d->nlines = wrap_lines(d->msg, DW - 60, d->starts, d->lens, DLG_MAX_LINES);
    d->text_y = 88 + 26;
    int y = d->text_y + d->nlines * (font_height(font_ui) + 2) + 10;
    if (d->has_field) { d->field = rect_make(30, y, DW - 60, 26); y += 36; }
    int bw = (DW - 60 - 10) / 2;
    d->cancel = d->alert ? rect_make(0, 0, 0, 0) : rect_make(30, y, bw, 28);
    d->ok_btn = d->alert ? rect_make((DW - bw) / 2, y, bw, 28) : rect_make(30 + bw + 10, y, bw, 28);
    d->h = y + 28 + 24;
}

static void dlg_draw(struct dlg *d) {
    struct canvas *c = window_begin_draw(d->w);
    gfx_fill(c, theme.bg);
    struct canvas *icon = icon_get("logo", 64);
    if (icon) gfx_blit_scaled_tinted(c, (DW - 48) / 2, 26, 48, 48, icon, theme.text);
    font_draw_centered(c, font_ui_semibold, rect_make(0, 88, DW, 20), theme.text, d->title);
    int y = d->text_y;
    for (int i = 0; i < d->nlines; i++) {
        int w = font_text_width_n(font_ui, d->starts[i], d->lens[i]);
        font_draw_n(c, font_ui, (DW - w) / 2, y, theme.text_secondary, d->starts[i], d->lens[i]);
        y += font_height(font_ui) + 2;
    }
    if (d->has_field) textfield_draw(c, &d->tf, d->field);
    if (!d->alert) ui_button(c, d->cancel, "Cancel", BTN_NORMAL, d->pressed == 1, true);
    ui_button(c, d->ok_btn, d->ok, d->destructive ? BTN_DESTRUCTIVE : BTN_DEFAULT, d->pressed == 2, true);
    window_end_draw(d->w, NULL);
}

static int dlg_run(struct dlg *d) {
    dlg_layout(d);
    int h = d->h;
    /* the layout's y coordinates already include the title strip that WIN_FULL_TITLE adds */
    d->w = window_create("", DW, h - TITLEBAR_H, WIN_DIALOG | WIN_UTILITY | WIN_FULL_TITLE);
    d->w->flags &= ~WIN_CLOSABLE;
    d->tf.focused = d->has_field;
    d->hover = 0; d->pressed = 0; d->result = -1;
    struct rect cancel = d->cancel, ok = d->ok_btn, field = d->field;
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
    struct dlg d = { .title = title, .msg = message, .ok = "OK", .alert = true };
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
