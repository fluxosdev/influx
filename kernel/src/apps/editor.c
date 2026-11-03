/* Text Editor: a plain-text editor with selection, clipboard, undo and file I/O. */
#include "ui.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

#define STATUS_H 22
#define PAD_X 10
#define PAD_Y 6
#define TAB_W 4

struct editor {
    struct window *w;
    char *text; int len, cap;
    int cursor, anchor;           /* byte offsets; anchor -1 = no selection */
    int scroll_y, scroll_x;       /* in lines / pixels */
    int pref_col;                 /* preferred x for vertical moves */
    char path[PATH_MAX];
    bool dirty, mono, wrap;
    char *undo_text; int undo_len, undo_cursor;
    bool dragging;
    uint64_t last_click; int click_count;
    int *line_starts; int nlines;
};

enum { ECMD_FONT_MONO = CMD_APP + 1, ECMD_FONT_PROP, ECMD_GOTO_LINE, ECMD_WORD_COUNT };
static const struct menu_item file_items[] = { MENU("New", CMD_NEW, 'n', MOD_CMD), MENU("Open…", CMD_OPEN, 'o', MOD_CMD), MENU_SEP, MENU("Save", CMD_SAVE, 's', MOD_CMD), MENU("Save As…", CMD_SAVE_AS, 's', MOD_CMD | MOD_SHIFT), MENU_SEP, MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit Text Editor", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item edit_items[] = { MENU("Undo", CMD_UNDO, 'z', MOD_CMD), MENU_SEP, MENU("Cut", CMD_CUT, 'x', MOD_CMD), MENU("Copy", CMD_COPY, 'c', MOD_CMD), MENU("Paste", CMD_PASTE, 'v', MOD_CMD), MENU("Select All", CMD_SELECT_ALL, 'a', MOD_CMD), MENU_SEP, MENU("Go to Line…", ECMD_GOTO_LINE, 'l', MOD_CMD) };
static const struct menu_item format_items[] = { MENU("Monospaced Font", ECMD_FONT_MONO, 0, 0), MENU("Proportional Font", ECMD_FONT_PROP, 0, 0) };
static const struct menu_item view_items[] = { MENU("Word Count", ECMD_WORD_COUNT, 0, 0) };
static const struct menu menus[] = { { "File", file_items, ARRAY_LEN(file_items) }, { "Edit", edit_items, ARRAY_LEN(edit_items) }, { "Format", format_items, ARRAY_LEN(format_items) }, { "View", view_items, ARRAY_LEN(view_items) } };
static const struct app_menus editor_menus = { menus, ARRAY_LEN(menus) };

static struct font *efont(struct editor *e) { return e->mono ? font_mono : font_ui_large; }
static int line_h(struct editor *e) { return font_height(efont(e)) + 4; }

static void reindex(struct editor *e) {
    kfree(e->line_starts);
    int n = 1; for (int i = 0; i < e->len; i++) if (e->text[i] == '\n') n++;
    e->line_starts = kmalloc((n + 1) * sizeof(int));
    e->nlines = n; int k = 0; e->line_starts[k++] = 0;
    for (int i = 0; i < e->len; i++) if (e->text[i] == '\n') e->line_starts[k++] = i + 1;
    e->line_starts[n] = e->len + 1;
}
static int line_of(struct editor *e, int off) {
    int lo = 0, hi = e->nlines - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (e->line_starts[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
}
static int line_end(struct editor *e, int l) { int end = e->line_starts[l + 1] - 1; return end > e->len ? e->len : end; }
/* width of a line prefix, expanding tabs */
static int text_x(struct editor *e, int start, int end) {
    struct font *f = efont(e); int x = 0, col = 0;
    int sw = font_char_width(f, ' ');
    const char *p = e->text + start, *stop = e->text + end;
    while (p < stop) {
        if (*p == '\t') { int n = TAB_W - (col % TAB_W); x += n * sw; col += n; p++; continue; }
        const char *q = p; uint32_t cp = utf8_decode(&q);
        x += font_char_width(f, cp); col++; p = q;
    }
    return x;
}
static int offset_for_x(struct editor *e, int l, int x) {
    int s = e->line_starts[l], end = line_end(e, l);
    const char *p = e->text + s; int best = s, bestd = 1 << 30;
    for (;;) {
        int w = text_x(e, s, (int)(p - e->text));
        int d = w > x ? w - x : x - w;
        if (d < bestd) { bestd = d; best = (int)(p - e->text); }
        if (p >= e->text + end) break;
        if (*p == '\t') p++; else utf8_decode(&p);
    }
    return best;
}

static void snapshot(struct editor *e) {
    kfree(e->undo_text);
    e->undo_text = kmalloc(e->len + 1); memcpy(e->undo_text, e->text, e->len + 1); e->undo_len = e->len; e->undo_cursor = e->cursor;
}
static void undo(struct editor *e) {
    if (!e->undo_text) return;
    char *t = e->undo_text; int l = e->undo_len, c = e->undo_cursor;
    e->undo_text = NULL; snapshot(e);
    if (e->cap < l + 1) { e->cap = l + 256; e->text = krealloc(e->text, e->cap); }
    memcpy(e->text, t, l + 1); e->len = l; e->cursor = CLAMP(c, 0, l); e->anchor = -1; e->dirty = true;
    kfree(t); reindex(e);
}
static void sel_range(struct editor *e, int *a, int *b) { if (e->anchor < 0) { *a = *b = e->cursor; return; } *a = MIN(e->anchor, e->cursor); *b = MAX(e->anchor, e->cursor); }
static void insert_text(struct editor *e, const char *s, int n) {
    if (e->len + n + 1 > e->cap) { e->cap = (e->len + n + 1) * 2; e->text = krealloc(e->text, e->cap); }
    memmove(e->text + e->cursor + n, e->text + e->cursor, e->len - e->cursor + 1);
    memcpy(e->text + e->cursor, s, n);
    e->len += n; e->cursor += n; e->dirty = true; reindex(e);
}
static void delete_range(struct editor *e, int a, int b) {
    if (a >= b) return;
    memmove(e->text + a, e->text + b, e->len - b + 1);
    e->len -= b - a; e->cursor = a; e->anchor = -1; e->dirty = true; reindex(e);
}
static bool delete_selection(struct editor *e) { int a, b; sel_range(e, &a, &b); if (a == b) { e->anchor = -1; return false; } delete_range(e, a, b); return true; }

static void update_title(struct editor *e) {
    char t[160];
    snprintf(t, sizeof t, "%s%s", e->path[0] ? vfs_basename(e->path) : "Untitled", e->dirty ? " — Edited" : "");
    window_set_title(e->w, t);
}
static bool load_file(struct editor *e, const char *path) {
    void *data; size_t sz;
    if (vfs_read_file(path, &data, &sz) < 0) return false;
    kfree(e->text); e->text = data; e->len = (int)sz; e->cap = (int)sz + 1;
    /* strip NULs / CR */
    int k = 0; for (int i = 0; i < e->len; i++) if (e->text[i] != '\r' && e->text[i]) e->text[k++] = e->text[i];
    e->len = k; e->text[k] = 0;
    e->cursor = 0; e->anchor = -1; e->scroll_y = 0; e->dirty = false;
    kfree(e->undo_text); e->undo_text = NULL;
    strlcpy(e->path, path, sizeof e->path);
    reindex(e); update_title(e);
    return true;
}
static bool save_file(struct editor *e, bool as) {
    if (as || !e->path[0]) {
        char p[PATH_MAX];
        char def[PATH_MAX]; if (e->path[0]) strlcpy(def, e->path, sizeof def); else strcpy(def, "/home/flux/Documents/Untitled.txt");
        if (!dialog_prompt("Save As", "Save the document to:", def, p, sizeof p) || !p[0]) return false;
        char norm[PATH_MAX]; vfs_normalize("/home/flux", p, norm, sizeof norm);
        strlcpy(e->path, norm, sizeof e->path);
    }
    int r = vfs_write_file(e->path, e->text, e->len);
    if (r < 0) { dialog_message("Text Editor", "The document could not be saved. Check that the folder exists and the volume is writable."); return false; }
    e->dirty = false; update_title(e);
    return true;
}

static struct rect text_rect(struct editor *e) { return rect_make(0, 0, e->w->frame.w, e->w->frame.h - STATUS_H); }
static int visible_lines(struct editor *e) { return (text_rect(e).h - PAD_Y) / line_h(e); }
static void ensure_cursor_visible(struct editor *e) {
    int l = line_of(e, e->cursor);
    int vis = visible_lines(e);
    if (l < e->scroll_y) e->scroll_y = l;
    if (l >= e->scroll_y + vis) e->scroll_y = l - vis + 1;
    int cx = text_x(e, e->line_starts[l], e->cursor);
    int tw = text_rect(e).w - PAD_X * 2 - 12;
    if (cx - e->scroll_x < 0) e->scroll_x = cx;
    if (cx - e->scroll_x > tw) e->scroll_x = cx - tw;
}
static void clamp_scroll(struct editor *e) { int max = e->nlines - visible_lines(e); if (max < 0) max = 0; e->scroll_y = CLAMP(e->scroll_y, 0, max); }

static void draw(struct editor *e) {
    struct canvas *c = window_begin_draw(e->w);
    struct font *f = efont(e);
    int lh = line_h(e), W = c->w, H = c->h;
    gfx_fill(c, theme.bg);
    struct rect tr = text_rect(e);
    struct canvas sub = canvas_sub(c, rect_make(0, 0, tr.w - 12, tr.h));
    int a, b; sel_range(e, &a, &b);
    bool focused = true;
    int vis = visible_lines(e) + 1;
    for (int l = e->scroll_y; l < e->nlines && l < e->scroll_y + vis; l++) {
        int y = PAD_Y + (l - e->scroll_y) * lh;
        int s = e->line_starts[l], end = line_end(e, l);
        /* selection highlight */
        if (a != b && b > s && a <= end) {
            int sa = MAX(a, s), sb = MIN(b, end);
            int x0 = text_x(e, s, sa), x1 = text_x(e, s, sb);
            if (b > end) x1 += font_char_width(f, ' ');
            gfx_fill_rect(&sub, PAD_X + x0 - e->scroll_x, y, x1 - x0, lh, focused ? color_with_alpha(theme.accent, 0x50) : theme.selection_inactive);
        }
        /* draw text with tab expansion */
        int x = PAD_X - e->scroll_x, col = 0; const char *p = e->text + s, *stop = e->text + end;
        int sw = font_char_width(f, ' ');
        const char *run = p;
        while (p <= stop) {
            if (p == stop || *p == '\t') {
                if (p > run) x += font_draw_n(&sub, f, x, y + 2, theme.text, run, p - run);
                if (p == stop) break;
                int n = TAB_W - (col % TAB_W); x += n * sw; col += n; p++; run = p; continue;
            }
            utf8_decode(&p); col++;
        }
        /* cursor */
        if (e->cursor >= s && e->cursor <= end && ((system_ticks / 500) & 1) == 0) {
            int cx = PAD_X + text_x(e, s, e->cursor) - e->scroll_x;
            gfx_fill_rect(&sub, cx, y + 1, 2, lh - 2, theme.accent);
        }
    }
    ui_scrollbar_v(c, rect_make(W - 12, 0, 12, tr.h), e->nlines * lh + PAD_Y, tr.h, e->scroll_y * lh);
    /* status bar */
    gfx_fill_rect(c, 0, H - STATUS_H, W, STATUS_H, theme.bg_secondary);
    gfx_hline(c, 0, H - STATUS_H, W, theme.separator);
    int l = line_of(e, e->cursor);
    char st[128];
    snprintf(st, sizeof st, "Ln %d, Col %d", l + 1, e->cursor - e->line_starts[l] + 1);
    font_draw(c, font_ui_small, 10, H - STATUS_H + (STATUS_H - font_height(font_ui_small)) / 2, theme.text_secondary, st);
    snprintf(st, sizeof st, "%d lines  ·  %d bytes  ·  UTF-8%s", e->nlines, e->len, e->path[0] ? "" : "  ·  unsaved");
    font_draw_aligned(c, font_ui_small, rect_make(0, H - STATUS_H, W - 10, STATUS_H), theme.text_secondary, st, 2);
    window_end_draw(e->w, NULL);
}

static int offset_at(struct editor *e, int x, int y) {
    int l = e->scroll_y + (y - PAD_Y) / line_h(e);
    if (y < PAD_Y) l = e->scroll_y - 1;
    l = CLAMP(l, 0, e->nlines - 1);
    return offset_for_x(e, l, x - PAD_X + e->scroll_x);
}
static void move_cursor(struct editor *e, int off, bool extend) {
    if (extend) { if (e->anchor < 0) e->anchor = e->cursor; } else e->anchor = -1;
    e->cursor = CLAMP(off, 0, e->len);
}
static bool is_word(char c) { return isalnum(c) || c == '_'; }
static int word_left(struct editor *e, int off) { while (off > 0 && !is_word(e->text[off - 1])) off--; while (off > 0 && is_word(e->text[off - 1])) off--; return off; }
static int word_right(struct editor *e, int off) { while (off < e->len && !is_word(e->text[off])) off++; while (off < e->len && is_word(e->text[off])) off++; return off; }

static bool confirm_discard(struct editor *e) {
    if (!e->dirty) return true;
    char msg[256]; snprintf(msg, sizeof msg, "Do you want to save the changes made to “%s”?\nYour changes will be lost if you don’t save them.", e->path[0] ? vfs_basename(e->path) : "Untitled");
    if (dialog_confirm("Unsaved Changes", msg, "Save", false)) return save_file(e, false);
    return dialog_confirm("Unsaved Changes", "Discard the changes?", "Don’t Save", true);
}

static void handle_key(struct editor *e, struct event *ev) {
    bool cmd = ev->mods & (MOD_CMD | MOD_CTRL), shift = ev->mods & MOD_SHIFT, alt = ev->mods & MOD_ALT;
    int k = ev->key; if (k >= 'A' && k <= 'Z') k += 32;
    int l = line_of(e, e->cursor);
    switch (ev->key) {
    case KEY_LEFT: if (!shift && e->anchor >= 0) { int a, b; sel_range(e, &a, &b); move_cursor(e, a, false); } else move_cursor(e, alt ? word_left(e, e->cursor) : cmd ? e->line_starts[l] : e->cursor - 1, shift); e->pref_col = -1; return;
    case KEY_RIGHT: if (!shift && e->anchor >= 0) { int a, b; sel_range(e, &a, &b); move_cursor(e, b, false); } else move_cursor(e, alt ? word_right(e, e->cursor) : cmd ? line_end(e, l) : e->cursor + 1, shift); e->pref_col = -1; return;
    case KEY_UP: case KEY_DOWN: {
        if (cmd) { move_cursor(e, ev->key == KEY_UP ? 0 : e->len, shift); return; }
        int nl = ev->key == KEY_UP ? l - 1 : l + 1;
        if (nl < 0 || nl >= e->nlines) { move_cursor(e, nl < 0 ? 0 : e->len, shift); return; }
        if (e->pref_col < 0) e->pref_col = text_x(e, e->line_starts[l], e->cursor);
        move_cursor(e, offset_for_x(e, nl, e->pref_col), shift); return; }
    case KEY_HOME: move_cursor(e, e->line_starts[l], shift); e->pref_col = -1; return;
    case KEY_END: move_cursor(e, line_end(e, l), shift); e->pref_col = -1; return;
    case KEY_PGUP: case KEY_PGDN: { int vis = visible_lines(e); int nl = CLAMP(ev->key == KEY_PGUP ? l - vis : l + vis, 0, e->nlines - 1); e->scroll_y += ev->key == KEY_PGUP ? -vis : vis; clamp_scroll(e); move_cursor(e, offset_for_x(e, nl, e->pref_col < 0 ? text_x(e, e->line_starts[l], e->cursor) : e->pref_col), shift); return; }
    case KEY_BACKSPACE: snapshot(e); if (!delete_selection(e)) { if (alt) delete_range(e, word_left(e, e->cursor), e->cursor); else if (e->cursor > 0) { int p = e->cursor - 1; while (p > 0 && (e->text[p] & 0xC0) == 0x80) p--; delete_range(e, p, e->cursor); } } e->pref_col = -1; return;
    case KEY_DELETE: snapshot(e); if (!delete_selection(e)) { if (e->cursor < e->len) { int q = e->cursor + 1; while (q < e->len && (e->text[q] & 0xC0) == 0x80) q++; delete_range(e, e->cursor, q); } } return;
    case KEY_ENTER: { snapshot(e); delete_selection(e); l = line_of(e, e->cursor); /* auto-indent */ int s = e->line_starts[l]; int ind = 0; while (e->text[s + ind] == ' ' || e->text[s + ind] == '\t') ind++; char buf[256]; buf[0] = '\n'; int n = MIN(ind, 200); memcpy(buf + 1, e->text + s, n); insert_text(e, buf, n + 1); e->pref_col = -1; return; }
    case KEY_TAB: snapshot(e); delete_selection(e); insert_text(e, "    ", 4); return;
    case KEY_ESC: e->anchor = -1; return;
    }
    if (cmd) return;
    if (ev->ch >= 32 || ev->ch >= 128) {
        char buf[8]; int n = utf8_encode(ev->ch, buf);
        if (e->len % 32 == 0 || e->anchor >= 0) snapshot(e);
        delete_selection(e);
        insert_text(e, buf, n); e->pref_col = -1;
    }
}

static void handle_command(struct editor *e, int cmd, bool *quit, bool *close) {
    switch (cmd) {
    case CMD_QUIT: if (confirm_discard(e)) *quit = true; break;
    case CMD_CLOSE: if (confirm_discard(e)) *close = true; break;
    case CMD_NEW: app_launch(APP_EDITOR, NULL); break;
    case CMD_OPEN: { char p[PATH_MAX]; if (dialog_prompt("Open", "Path of the file to open:", e->path[0] ? e->path : "/home/flux/Documents/", p, sizeof p) && p[0]) { if (!confirm_discard(e)) break; char norm[PATH_MAX]; vfs_normalize("/home/flux", p, norm, sizeof norm); if (!load_file(e, norm)) dialog_message("Text Editor", "The file could not be opened."); } break; }
    case CMD_SAVE: save_file(e, false); break;
    case CMD_SAVE_AS: save_file(e, true); break;
    case CMD_UNDO: undo(e); break;
    case CMD_SELECT_ALL: e->anchor = 0; e->cursor = e->len; break;
    case CMD_COPY: case CMD_CUT: { int a, b; sel_range(e, &a, &b); if (a != b) { clipboard_set(e->text + a, b - a); if (cmd == CMD_CUT) { snapshot(e); delete_range(e, a, b); } } break; }
    case CMD_PASTE: { char *s = clipboard_get(); if (s) { snapshot(e); delete_selection(e); insert_text(e, s, strlen(s)); kfree(s); } break; }
    case ECMD_FONT_MONO: e->mono = true; break;
    case ECMD_FONT_PROP: e->mono = false; break;
    case ECMD_GOTO_LINE: { char s[16]; if (dialog_prompt("Go to Line", "Line number:", "", s, sizeof s)) { int n = atoi(s); if (n >= 1 && n <= e->nlines) { e->cursor = e->line_starts[n - 1]; e->anchor = -1; } } break; }
    case ECMD_WORD_COUNT: { int words = 0; bool in = false; for (int i = 0; i < e->len; i++) { if (isspace(e->text[i])) in = false; else if (!in) { in = true; words++; } } char m[128]; snprintf(m, sizeof m, "Words: %d\nLines: %d\nCharacters: %d", words, e->nlines, e->len); dialog_message("Word Count", m); break; }
    }
}

void editor_main(void *arg) {
    struct editor *e = kzalloc(sizeof *e);
    e->cap = 1024; e->text = kzalloc(e->cap); e->anchor = -1; e->pref_col = -1; e->mono = true;
    reindex(e);
    e->w = window_create("Untitled", 680, 460, WIN_STANDARD);
    window_set_min_size(e->w, 300, 160);
    window_set_menus(e->w, &editor_menus);
    window_set_timer(e->w, 500);
    if (arg && ((const char *)arg)[0]) { if (!load_file(e, arg)) strlcpy(e->path, arg, sizeof e->path); update_title(e); }
    draw(e);
    window_show(e->w);
    bool quit = false, close = false;
    struct event ev;
    while (!quit && !close && window_wait_event(e->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: quit = true; break;
        case EV_CLOSE: handle_command(e, CMD_CLOSE, &quit, &close); break;
        case EV_MENU: handle_command(e, ev.cmd, &quit, &close); ensure_cursor_visible(e); break;
        case EV_OPEN: if (confirm_discard(e)) load_file(e, ev.path); kfree(ev.path); break;
        case EV_RESIZE: clamp_scroll(e); break;
        case EV_TIMER: break;
        case EV_MOUSE_SCROLL: e->scroll_y += ev.scroll * 3; clamp_scroll(e); break;
        case EV_MOUSE_DOWN:
            if (ev.button == BTN_LEFT && ev.y < text_rect(e).h) {
                int off = offset_at(e, ev.x, ev.y);
                bool dbl = system_ticks - e->last_click < 450; e->last_click = system_ticks;
                e->click_count = dbl ? e->click_count + 1 : 1;
                if (e->click_count == 2) { e->anchor = word_left(e, off); e->cursor = word_right(e, off); }
                else if (e->click_count >= 3) { int l = line_of(e, off); e->anchor = e->line_starts[l]; e->cursor = line_end(e, l); }
                else { move_cursor(e, off, ev.mods & MOD_SHIFT); e->dragging = true; }
                e->pref_col = -1;
            }
            break;
        case EV_MOUSE_MOVE: if (e->dragging && (ev.buttons & BTN_LEFT)) { int off = offset_at(e, ev.x, ev.y); if (e->anchor < 0) e->anchor = e->cursor; e->cursor = off; } break;
        case EV_MOUSE_UP: e->dragging = false; if (e->anchor == e->cursor) e->anchor = -1; break;
        case EV_KEY_DOWN: handle_key(e, &ev); ensure_cursor_visible(e); update_title(e); break;
        default: break;
        }
        if (!quit && !close) draw(e);
    }
    window_destroy(e->w);
    kfree(e->text); kfree(e->undo_text); kfree(e->line_starts); kfree(e);
}
