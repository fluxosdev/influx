/* Terminal: a VT100/ANSI terminal emulator window hosting the inFlux shell. */
#include "ui.h"
#include "tty.h"
#include "mm.h"
#include "string.h"

#define SCROLLBACK 2000
#define PAD 8
#define ATTR_BOLD 1
#define ATTR_UNDERLINE 2
#define ATTR_REVERSE 4
#define ATTR_DIM 8

struct cell { uint32_t cp; color_t fg, bg; uint8_t attr; };
struct term {
    struct window *w;
    struct tty *tty;
    int cols, rows, cw, ch;
    struct cell *grid;                /* rows * cols */
    struct cell *sb; int sb_lines, sb_head;   /* scrollback ring of full rows */
    int cx, cy; bool cursor_visible;
    color_t fg, bg; uint8_t attr;
    color_t def_fg, def_bg;
    int view;                         /* scrollback offset (0 = live) */
    /* parser */
    int state; int params[16]; int nparams; bool priv_mode; char osc[64]; int osc_len;
    int saved_x, saved_y;
    bool dirty;
    struct mutex lock;
    int shell_pid;
    bool closing;
    char title[64];
    uint64_t last_output;
};

static const color_t ansi_colors[16] = {
    0xFF1E1E1E, 0xFFFF5F57, 0xFF5AF78E, 0xFFF3F99D, 0xFF57C7FF, 0xFFFF6AC1, 0xFF9AEDFE, 0xFFF1F1F0,
    0xFF686868, 0xFFFF6E67, 0xFF5AF78E, 0xFFF4F99D, 0xFF6DC6FF, 0xFFFF7FC8, 0xFFA6EFFF, 0xFFFFFFFF,
};
static color_t color256(int n) {
    if (n < 16) return ansi_colors[n];
    if (n < 232) { n -= 16; int r = n / 36, g = (n / 6) % 6, b = n % 6; return RGB(r ? r * 40 + 55 : 0, g ? g * 40 + 55 : 0, b ? b * 40 + 55 : 0); }
    int v = (n - 232) * 10 + 8; return RGB(v, v, v);
}

enum { TCMD_NEW = CMD_APP + 1, TCMD_RESET, TCMD_PASTE, TCMD_CLEAR };
static const struct menu_item shell_items[] = { MENU("New Window", TCMD_NEW, 'n', MOD_CMD), MENU_SEP, MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit Terminal", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item edit_items[] = { MENU("Paste", TCMD_PASTE, 'v', MOD_CMD), MENU("Clear Scrollback", TCMD_CLEAR, 'k', MOD_CMD), MENU_SEP, MENU("Reset Terminal", TCMD_RESET, 0, 0) };
static const struct menu menus[] = { { "Shell", shell_items, ARRAY_LEN(shell_items) }, { "Edit", edit_items, ARRAY_LEN(edit_items) } };
static const struct app_menus term_menus = { menus, ARRAY_LEN(menus) };

static void clear_cells(struct term *t, struct cell *c, int n) { for (int i = 0; i < n; i++) { c[i].cp = ' '; c[i].fg = t->fg; c[i].bg = t->bg; c[i].attr = 0; } }
static struct cell *row(struct term *t, int y) { return t->grid + y * t->cols; }

static void scroll_up(struct term *t) {
    /* push top row to scrollback */
    memcpy(t->sb + t->sb_head * t->cols, row(t, 0), t->cols * sizeof(struct cell));
    t->sb_head = (t->sb_head + 1) % SCROLLBACK;
    if (t->sb_lines < SCROLLBACK) t->sb_lines++;
    memmove(row(t, 0), row(t, 1), (t->rows - 1) * t->cols * sizeof(struct cell));
    clear_cells(t, row(t, t->rows - 1), t->cols);
}
static void newline(struct term *t) { if (t->cy == t->rows - 1) scroll_up(t); else t->cy++; }
static void put_char(struct term *t, uint32_t cp) {
    if (t->cx >= t->cols) { t->cx = 0; newline(t); }
    struct cell *c = &row(t, t->cy)[t->cx];
    c->cp = cp; c->fg = t->fg; c->bg = t->bg; c->attr = t->attr;
    t->cx++;
}
static void erase_display(struct term *t, int mode) {
    if (mode == 2 || mode == 3) { for (int y = 0; y < t->rows; y++) clear_cells(t, row(t, y), t->cols); if (mode == 3) t->sb_lines = 0; return; }
    if (mode == 0) { clear_cells(t, &row(t, t->cy)[t->cx], t->cols - t->cx); for (int y = t->cy + 1; y < t->rows; y++) clear_cells(t, row(t, y), t->cols); }
    else { for (int y = 0; y < t->cy; y++) clear_cells(t, row(t, y), t->cols); clear_cells(t, row(t, t->cy), t->cx + 1); }
}
static void erase_line(struct term *t, int mode) {
    if (mode == 0) clear_cells(t, &row(t, t->cy)[t->cx], t->cols - t->cx);
    else if (mode == 1) clear_cells(t, row(t, t->cy), t->cx + 1);
    else clear_cells(t, row(t, t->cy), t->cols);
}
static void sgr(struct term *t) {
    if (t->nparams == 0) { t->params[0] = 0; t->nparams = 1; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) { t->fg = t->def_fg; t->bg = t->def_bg; t->attr = 0; }
        else if (p == 1) t->attr |= ATTR_BOLD; else if (p == 2) t->attr |= ATTR_DIM; else if (p == 4) t->attr |= ATTR_UNDERLINE; else if (p == 7) t->attr |= ATTR_REVERSE;
        else if (p == 22) t->attr &= ~(ATTR_BOLD | ATTR_DIM); else if (p == 24) t->attr &= ~ATTR_UNDERLINE; else if (p == 27) t->attr &= ~ATTR_REVERSE;
        else if (p >= 30 && p <= 37) t->fg = ansi_colors[p - 30]; else if (p == 39) t->fg = t->def_fg;
        else if (p >= 40 && p <= 47) t->bg = ansi_colors[p - 40]; else if (p == 49) t->bg = t->def_bg;
        else if (p >= 90 && p <= 97) t->fg = ansi_colors[p - 90 + 8]; else if (p >= 100 && p <= 107) t->bg = ansi_colors[p - 100 + 8];
        else if ((p == 38 || p == 48) && i + 1 < t->nparams) {
            color_t c = 0;
            if (t->params[i + 1] == 5 && i + 2 < t->nparams) { c = color256(t->params[i + 2]); i += 2; }
            else if (t->params[i + 1] == 2 && i + 4 < t->nparams) { c = RGB(t->params[i + 2], t->params[i + 3], t->params[i + 4]); i += 4; }
            if (p == 38) t->fg = c; else t->bg = c;
        }
    }
}
static void csi(struct term *t, char final) {
    int p0 = t->nparams > 0 ? t->params[0] : 0, p1 = t->nparams > 1 ? t->params[1] : 0;
    int n = p0 ? p0 : 1;
    switch (final) {
    case 'A': t->cy = MAX(0, t->cy - n); break;
    case 'B': t->cy = MIN(t->rows - 1, t->cy + n); break;
    case 'C': t->cx = MIN(t->cols - 1, t->cx + n); break;
    case 'D': t->cx = MAX(0, t->cx - n); break;
    case 'E': t->cx = 0; t->cy = MIN(t->rows - 1, t->cy + n); break;
    case 'F': t->cx = 0; t->cy = MAX(0, t->cy - n); break;
    case 'G': t->cx = CLAMP(n - 1, 0, t->cols - 1); break;
    case 'H': case 'f': t->cy = CLAMP((p0 ? p0 : 1) - 1, 0, t->rows - 1); t->cx = CLAMP((p1 ? p1 : 1) - 1, 0, t->cols - 1); break;
    case 'J': erase_display(t, p0); break;
    case 'K': erase_line(t, p0); break;
    case 'X': clear_cells(t, &row(t, t->cy)[t->cx], MIN(n, t->cols - t->cx)); break;
    case 'P': { struct cell *r = row(t, t->cy); int k = MIN(n, t->cols - t->cx); memmove(r + t->cx, r + t->cx + k, (t->cols - t->cx - k) * sizeof(struct cell)); clear_cells(t, r + t->cols - k, k); break; }
    case '@': { struct cell *r = row(t, t->cy); int k = MIN(n, t->cols - t->cx); memmove(r + t->cx + k, r + t->cx, (t->cols - t->cx - k) * sizeof(struct cell)); clear_cells(t, r + t->cx, k); break; }
    case 'L': for (int i = 0; i < n; i++) { memmove(row(t, t->cy + 1), row(t, t->cy), (t->rows - t->cy - 1) * t->cols * sizeof(struct cell)); clear_cells(t, row(t, t->cy), t->cols); } break;
    case 'M': for (int i = 0; i < n; i++) { memmove(row(t, t->cy), row(t, t->cy + 1), (t->rows - t->cy - 1) * t->cols * sizeof(struct cell)); clear_cells(t, row(t, t->rows - 1), t->cols); } break;
    case 'S': for (int i = 0; i < n; i++) scroll_up(t); break;
    case 'm': sgr(t); break;
    case 's': t->saved_x = t->cx; t->saved_y = t->cy; break;
    case 'u': t->cx = t->saved_x; t->cy = t->saved_y; break;
    case 'h': if (t->priv_mode && p0 == 25) t->cursor_visible = true; break;
    case 'l': if (t->priv_mode && p0 == 25) t->cursor_visible = false; break;
    case 'd': t->cy = CLAMP(n - 1, 0, t->rows - 1); break;
    }
}

static void term_putc(struct term *t, unsigned char c) {
    static uint32_t utf_cp; static int utf_rem;
    switch (t->state) {
    case 0:
        if (utf_rem) { if ((c & 0xC0) == 0x80) { utf_cp = (utf_cp << 6) | (c & 0x3F); if (--utf_rem == 0) put_char(t, utf_cp); return; } utf_rem = 0; }
        if (c >= 0x80) { if ((c & 0xE0) == 0xC0) { utf_cp = c & 0x1F; utf_rem = 1; } else if ((c & 0xF0) == 0xE0) { utf_cp = c & 0x0F; utf_rem = 2; } else if ((c & 0xF8) == 0xF0) { utf_cp = c & 7; utf_rem = 3; } return; }
        switch (c) {
        case 27: t->state = 1; break;
        case '\n': newline(t); t->cx = 0; break;
        case '\r': t->cx = 0; break;
        case '\b': if (t->cx > 0) t->cx--; break;
        case '\t': t->cx = MIN(t->cols - 1, (t->cx / 8 + 1) * 8); break;
        case 7: break;
        default: if (c >= 32) put_char(t, c); break;
        }
        break;
    case 1:   /* ESC */
        if (c == '[') { t->state = 2; t->nparams = 0; t->params[0] = 0; t->priv_mode = false; }
        else if (c == ']') { t->state = 3; t->osc_len = 0; }
        else if (c == '(' || c == ')') t->state = 4;
        else { if (c == 'c') { erase_display(t, 2); t->cx = t->cy = 0; t->fg = t->def_fg; t->bg = t->def_bg; t->attr = 0; } else if (c == '7') { t->saved_x = t->cx; t->saved_y = t->cy; } else if (c == '8') { t->cx = t->saved_x; t->cy = t->saved_y; } else if (c == 'M') { if (t->cy == 0) { memmove(row(t, 1), row(t, 0), (t->rows - 1) * t->cols * sizeof(struct cell)); clear_cells(t, row(t, 0), t->cols); } else t->cy--; } t->state = 0; }
        break;
    case 2:   /* CSI */
        if (c >= '0' && c <= '9') { if (t->nparams == 0) t->nparams = 1; t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (c - '0'); }
        else if (c == ';') { if (t->nparams == 0) t->nparams = 1; if (t->nparams < 16) { t->params[t->nparams++] = 0; } }
        else if (c == '?') t->priv_mode = true;
        else if (c >= 0x40 && c <= 0x7E) { csi(t, c); t->state = 0; }
        else if (c < 0x20) { term_putc(t, c); }
        break;
    case 3:   /* OSC ... BEL or ESC \ */
        if (c == 7 || c == 27) { t->osc[t->osc_len] = 0; if (t->osc_len > 2 && (t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') strlcpy(t->title, t->osc + 2, sizeof t->title); t->state = c == 27 ? 5 : 0; }
        else if (t->osc_len < 63) t->osc[t->osc_len++] = c;
        break;
    case 4: t->state = 0; break;
    case 5: t->state = 0; break;
    }
}

static void term_output(void *priv, const char *s, size_t n) {
    struct term *t = priv;
    mutex_lock(&t->lock);
    for (size_t i = 0; i < n; i++) term_putc(t, (unsigned char)s[i]);
    t->view = 0;
    bool was_dirty = t->dirty;
    t->dirty = true;
    mutex_unlock(&t->lock);
    if (!was_dirty) { struct event e = { .type = EV_USER }; window_post_event(t->w, &e); }
}

static void draw_cell_row(struct canvas *c, struct term *t, struct cell *cells, int y, bool cursor_row) {
    int py = PAD + y * t->ch;
    for (int x = 0; x < t->cols; x++) {
        struct cell *cl = &cells[x];
        color_t fg = cl->fg, bg = cl->bg;
        if (cl->attr & ATTR_REVERSE) { color_t tmp = fg; fg = bg; bg = tmp; }
        if (cl->attr & ATTR_DIM) fg = color_mix(fg, bg, 110);
        bool cur = cursor_row && x == t->cx && t->cursor_visible && t->view == 0;
        if (cur) { bg = t->def_fg; fg = t->def_bg; }
        int px = PAD + x * t->cw;
        if (bg != t->def_bg) gfx_fill_rect(c, px, py, t->cw, t->ch, bg);
        if (cl->cp != ' ' && cl->cp) {
            char u[8]; int n = utf8_encode(cl->cp, u);
            font_draw_n(c, (cl->attr & ATTR_BOLD) ? font_mono_bold : font_mono, px, py + 1, fg, u, n);
        }
        if (cl->attr & ATTR_UNDERLINE) gfx_hline(c, px, py + t->ch - 2, t->cw, fg);
    }
}

static void draw(struct term *t) {
    struct canvas *c = window_begin_draw(t->w);
    mutex_lock(&t->lock);
    gfx_fill(c, t->def_bg);
    /* rows: view offset into scrollback */
    for (int y = 0; y < t->rows; y++) {
        int line = y - t->view;   /* <0 => scrollback */
        if (line >= 0) draw_cell_row(c, t, row(t, line), y, line == t->cy);
        else {
            int idx = (t->sb_head + SCROLLBACK + line) % SCROLLBACK;
            if (-line <= t->sb_lines) draw_cell_row(c, t, t->sb + idx * t->cols, y, false);
        }
    }
    if (t->view > 0) {
        char s[32]; snprintf(s, sizeof s, "↑ %d", t->view);
        int w = font_text_width(font_ui_small, s) + 12;
        gfx_fill_rounded_rect(c, c->w - w - 10, 8, w, 18, 5, RGBA(255, 255, 255, 0x30));
        font_draw_centered(c, font_ui_small, rect_make(c->w - w - 10, 8, w, 18), RGB(255, 255, 255), s);
    }
    t->dirty = false;
    mutex_unlock(&t->lock);
    window_end_draw(t->w, NULL);
}

static void resize_grid(struct term *t, int cols, int rows) {
    if (cols < 10) cols = 10; if (rows < 3) rows = 3;
    if (cols == t->cols && rows == t->rows) return;
    mutex_lock(&t->lock);
    struct cell *ng = kmalloc(cols * rows * sizeof(struct cell));
    struct cell *nsb = kmalloc(cols * SCROLLBACK * sizeof(struct cell));
    color_t ofg = t->fg, obg = t->bg; t->fg = t->def_fg; t->bg = t->def_bg;
    struct cell *og = t->grid, *osb = t->sb; int oc = t->cols, orows = t->rows;
    t->grid = ng; t->sb = nsb; t->cols = cols; t->rows = rows;
    for (int y = 0; y < rows; y++) clear_cells(t, row(t, y), cols);
    for (int i = 0; i < SCROLLBACK; i++) clear_cells(t, nsb + i * cols, cols);
    if (og) {
        for (int y = 0; y < MIN(rows, orows); y++) memcpy(row(t, y), og + y * oc, MIN(cols, oc) * sizeof(struct cell));
        for (int i = 0; i < SCROLLBACK; i++) memcpy(nsb + i * cols, osb + i * oc, MIN(cols, oc) * sizeof(struct cell));
        kfree(og); kfree(osb);
    }
    t->fg = ofg; t->bg = obg;
    t->cx = MIN(t->cx, cols - 1); t->cy = MIN(t->cy, rows - 1);
    t->tty->cols = cols; t->tty->rows = rows;
    mutex_unlock(&t->lock);
}

static void send_key(struct term *t, struct event *ev) {
    char buf[8]; int n = 0;
    bool cmd = ev->mods & MOD_CMD;
    if (cmd) return;
    switch (ev->key) {
    case KEY_ENTER: buf[n++] = '\r'; break;
    case KEY_BACKSPACE: buf[n++] = 127; break;
    case KEY_TAB: buf[n++] = '\t'; break;
    case KEY_ESC: buf[n++] = 27; break;
    case KEY_UP: n = 3; memcpy(buf, "\033[A", 3); break;
    case KEY_DOWN: n = 3; memcpy(buf, "\033[B", 3); break;
    case KEY_RIGHT: n = 3; memcpy(buf, "\033[C", 3); break;
    case KEY_LEFT: n = 3; memcpy(buf, "\033[D", 3); break;
    case KEY_HOME: n = 3; memcpy(buf, "\033[H", 3); break;
    case KEY_END: n = 3; memcpy(buf, "\033[F", 3); break;
    case KEY_DELETE: n = 4; memcpy(buf, "\033[3~", 4); break;
    case KEY_PGUP: if (ev->mods & MOD_SHIFT) { t->view = MIN(t->view + t->rows / 2, t->sb_lines); t->dirty = true; return; } n = 4; memcpy(buf, "\033[5~", 4); break;
    case KEY_PGDN: if (ev->mods & MOD_SHIFT) { t->view = MAX(t->view - t->rows / 2, 0); t->dirty = true; return; } n = 4; memcpy(buf, "\033[6~", 4); break;
    default:
        if (ev->ch) n = utf8_encode(ev->ch, buf);
        else if ((ev->mods & MOD_ALT) && ev->key >= 32 && ev->key < 127) { buf[n++] = 27; buf[n++] = (char)ev->key; }
    }
    if (n) { t->view = 0; tty_input(t->tty, buf, n); }
}

void shell_main(void *arg);
struct shell_start { struct tty *tty; char *cwd; char *cmd; };

static void term_interrupt(void *priv) { UNUSED(priv); }

void terminal_main(void *arg) {
    struct term *t = kzalloc(sizeof *t);
    mutex_init(&t->lock);
    t->def_fg = RGB(0xE8, 0xE8, 0xE8); t->def_bg = RGB(0x1C, 0x1C, 0x1E);
    t->fg = t->def_fg; t->bg = t->def_bg; t->cursor_visible = true;
    t->cw = font_char_width(font_mono, 'M'); t->ch = font_height(font_mono) + 1;
    t->tty = tty_create(term_output, t);
    t->tty->on_interrupt = term_interrupt;
    int cols = 80, rows = 24;
    t->w = window_create("Terminal", cols * t->cw + PAD * 2, rows * t->ch + PAD * 2, WIN_STANDARD | WIN_NO_CTRL_ALIAS);
    window_set_min_size(t->w, 20 * t->cw + PAD * 2, 5 * t->ch + PAD * 2);
    window_set_menus(t->w, &term_menus);
    resize_grid(t, cols, rows);
    draw(t);
    window_show(t->w);
    /* start the shell in its own process */
    struct shell_start *ss = kzalloc(sizeof *ss);
    ss->tty = t->tty;
    const char *a = arg;
    if (a && !strncmp(a, "cd:", 3)) ss->cwd = strdup(a + 3);
    else if (a && a[0]) ss->cmd = strdup(a);
    struct process *sp = process_create("zsh", false);
    strcpy(sp->cwd, ss->cwd ? ss->cwd : "/home/flux");
    t->shell_pid = sp->pid;
    thread_create(sp, "zsh", shell_main, ss);
    window_set_timer(t->w, 500);   /* cursor blink / title refresh */
    struct event ev;
    bool quit = false;
    while (!quit && window_wait_event(t->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: quit = true; break;
        case EV_MENU:
            if (ev.cmd == CMD_QUIT) quit = true;
            else if (ev.cmd == TCMD_NEW) app_launch(APP_TERMINAL, NULL);
            else if (ev.cmd == TCMD_PASTE) { char *s = clipboard_get(); if (s) { tty_input(t->tty, s, strlen(s)); kfree(s); } }
            else if (ev.cmd == TCMD_CLEAR) { mutex_lock(&t->lock); t->sb_lines = 0; t->view = 0; erase_display(t, 2); t->cx = 0; t->cy = 0; t->dirty = true; mutex_unlock(&t->lock); tty_input(t->tty, "\f", 1); }
            else if (ev.cmd == TCMD_RESET) { mutex_lock(&t->lock); erase_display(t, 2); t->cx = t->cy = 0; t->fg = t->def_fg; t->bg = t->def_bg; t->attr = 0; t->dirty = true; mutex_unlock(&t->lock); }
            break;
        case EV_USER: break;
        case EV_TIMER: {
            struct process *sp = process_find(t->shell_pid);
            if (!sp || sp->exited) { quit = true; break; }
            char title[96]; snprintf(title, sizeof title, "%s — %dx%d", t->title[0] ? t->title : "zsh", t->cols, t->rows);
            if (strcmp(title, t->w->title)) window_set_title(t->w, title);
            break; }
        case EV_RESIZE: resize_grid(t, (ev.w - PAD * 2) / t->cw, (ev.h - PAD * 2) / t->ch); t->dirty = true; break;
        case EV_KEY_DOWN: send_key(t, &ev); break;
        case EV_MOUSE_SCROLL: mutex_lock(&t->lock); t->view = CLAMP(t->view + ev.scroll * 3, 0, t->sb_lines); t->dirty = true; mutex_unlock(&t->lock); break;
        default: break;
        }
        if (t->dirty && !quit) draw(t);
    }
    /* tear down: kill the shell and its foreground job */
    if (t->tty->fg_pid > 0) process_kill(t->tty->fg_pid, 9);
    struct process *shp = process_find(t->shell_pid);
    if (shp && !shp->exited) process_kill(t->shell_pid, 9);
    tty_destroy(t->tty);
    window_destroy(t->w);
    for (int i = 0; i < 50 && (shp = process_find(t->shell_pid)) && !shp->exited; i++) thread_sleep_ms(10);
    if ((shp = process_find(t->shell_pid))) process_wait(t->shell_pid, NULL);
    kfree(t->grid); kfree(t->sb); kfree(t);
}
