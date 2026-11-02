/* Calculator: fixed-point decimal arithmetic (no FPU in the kernel). */
#include "ui.h"
#include "string.h"
#include "mm.h"

typedef __int128 dec_t;
#define SCALE 1000000000000LL   /* 12 decimal places */

struct calc {
    struct window *w;
    char entry[32]; bool entering; bool has_dot;
    dec_t acc; int op; bool have_acc; bool error;
    dec_t last_operand; int last_op;
    int pressed; int hover;
};

static const char *keys[5][4] = {
    { "AC", "±", "%", "÷" },
    { "7", "8", "9", "×" },
    { "4", "5", "6", "−" },
    { "1", "2", "3", "+" },
    { "0", "", ".", "=" },
};
#define CW 232
#define DISP_H 100
#define BTN_SZ 56
#define GAP 1

static void dec_format(dec_t v, char *out, size_t n) {
    bool neg = v < 0; if (neg) v = -v;
    dec_t ip = v / SCALE, fp = v % SCALE;
    char ibuf[48]; int il = 0;
    if (ip == 0) ibuf[il++] = '0';
    while (ip > 0 && il < 40) { ibuf[il++] = '0' + (int)(ip % 10); ip /= 10; }
    char fbuf[16]; int fl = 0;
    for (int i = 0; i < 12; i++) { fbuf[11 - i] = '0' + (int)(fp % 10); fp /= 10; }
    fl = 12; while (fl > 0 && fbuf[fl - 1] == '0') fl--;
    /* limit to ~12 significant digits */
    int sig = il; if (sig + fl > 13) fl = MAX(0, 13 - sig);
    size_t p = 0;
    if (neg) out[p++] = '-';
    for (int i = il - 1; i >= 0 && p < n - 1; i--) out[p++] = ibuf[i];
    if (fl) { out[p++] = '.'; for (int i = 0; i < fl && p < n - 1; i++) out[p++] = fbuf[i]; }
    out[p] = 0;
}
static dec_t dec_parse(const char *s) {
    bool neg = *s == '-'; if (neg) s++;
    dec_t ip = 0; while (isdigit(*s)) ip = ip * 10 + (*s++ - '0');
    dec_t fp = 0, scale = SCALE;
    if (*s == '.') { s++; while (isdigit(*s) && scale > 1) { scale /= 10; fp += (*s++ - '0') * scale; } }
    dec_t v = ip * SCALE + fp;
    return neg ? -v : v;
}
static dec_t apply(struct calc *c, dec_t a, int op, dec_t b) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b / SCALE;
    case '/': if (b == 0) { c->error = true; return 0; } return a * SCALE / b;
    }
    return b;
}
static dec_t current_value(struct calc *c) { return c->entering ? dec_parse(c->entry) : c->acc; }

static void draw(struct calc *c) {
    struct canvas *cv = window_begin_draw(c->w);
    gfx_fill(cv, RGB(0x1C, 0x1C, 0x1E));
    char disp[48];
    if (c->error) strcpy(disp, "Error");
    else if (c->entering) strcpy(disp, c->entry);
    else dec_format(c->acc, disp, sizeof disp);
    struct font *f = font_ui_huge;
    if (font_text_width(f, disp) > CW - 30) f = font_ui_display;
    if (font_text_width(f, disp) > CW - 30) f = font_ui_large;
    int tw = font_text_width(f, disp);
    font_draw(cv, f, CW - 16 - tw, DISP_H - font_height(f) - 10, RGB(255, 255, 255), disp);
    for (int r = 0; r < 5; r++) for (int col = 0; col < 4; col++) {
        const char *k = keys[r][col];
        if (!k[0]) continue;
        int w = (r == 4 && col == 0) ? BTN_SZ * 2 + GAP * 2 : BTN_SZ;
        int x = 2 + col * (BTN_SZ + GAP * 2), y = DISP_H + r * (BTN_SZ + GAP * 2);
        color_t bg; color_t fg = RGB(255, 255, 255);
        bool is_op = col == 3;
        bool is_fn = r == 0 && col < 3;
        if (is_op) bg = RGB(0xFF, 0x9F, 0x0A); else if (is_fn) bg = RGB(0xA5, 0xA5, 0xA5); else bg = RGB(0x50, 0x50, 0x50);
        if (is_fn) fg = RGB(0, 0, 0);
        bool active_op = is_op && r < 4 && c->have_acc && !c->entering && c->op == "/*-+"[r];
        if (active_op) { bg = RGB(255, 255, 255); fg = RGB(0xFF, 0x9F, 0x0A); }
        int idx = r * 4 + col;
        if (c->pressed == idx) bg = color_lighten(bg, 60);
        gfx_fill_rounded_rect(cv, x, y, w, BTN_SZ, 6, bg);
        struct font *bf = font_ui_display;
        const char *label = k;
        if (!strcmp(k, "AC") && (c->entering || c->have_acc)) label = "C";
        if (!strcmp(k, "AC") || !strcmp(k, "C")) bf = font_ui_semibold_xl;
        font_draw_centered(cv, bf, rect_make(x, y, w, BTN_SZ), fg, label);
    }
    window_end_draw(c->w, NULL);
}

static void press_key(struct calc *c, const char *k) {
    if (c->error && strcmp(k, "AC")) return;
    if (isdigit(k[0]) || !strcmp(k, ".")) {
        if (!c->entering) { c->entry[0] = 0; c->entering = true; c->has_dot = false; }
        size_t l = strlen(c->entry);
        if (!strcmp(k, ".")) { if (c->has_dot) return; if (l == 0) { strcpy(c->entry, "0"); l = 1; } c->has_dot = true; }
        if (l >= 16) return;
        if (!strcmp(c->entry, "0") && isdigit(k[0])) l = 0;
        c->entry[l] = k[0]; c->entry[l + 1] = 0;
        return;
    }
    if (!strcmp(k, "AC")) { c->entering = false; c->have_acc = false; c->acc = 0; c->op = 0; c->error = false; c->entry[0] = 0; return; }
    if (!strcmp(k, "±")) {
        if (c->entering) { if (c->entry[0] == '-') memmove(c->entry, c->entry + 1, strlen(c->entry)); else { memmove(c->entry + 1, c->entry, strlen(c->entry) + 1); c->entry[0] = '-'; } }
        else c->acc = -c->acc;
        return;
    }
    if (!strcmp(k, "%")) { dec_t v = current_value(c) / 100; c->acc = v; c->entering = false; c->have_acc = true; return; }
    int op = 0;
    if (!strcmp(k, "÷")) op = '/'; else if (!strcmp(k, "×")) op = '*'; else if (!strcmp(k, "−")) op = '-'; else if (!strcmp(k, "+")) op = '+';
    if (op) {
        dec_t v = current_value(c);
        if (c->have_acc && c->op && c->entering) c->acc = apply(c, c->acc, c->op, v); else c->acc = v;
        c->op = op; c->have_acc = true; c->entering = false;
        return;
    }
    if (!strcmp(k, "=")) {
        if (c->op) {
            dec_t v = c->entering ? dec_parse(c->entry) : c->last_operand;
            if (c->entering) { c->last_operand = v; c->last_op = c->op; }
            c->acc = apply(c, c->acc, c->op, v);
            c->op = 0;
        }
        c->entering = false; c->have_acc = true;
    }
}

static int key_at(int x, int y) {
    if (y < DISP_H) return -1;
    int r = (y - DISP_H) / (BTN_SZ + GAP * 2), col = (x - 2) / (BTN_SZ + GAP * 2);
    if (r < 0 || r > 4 || col < 0 || col > 3) return -1;
    if (r == 4 && col == 1) col = 0;
    return r * 4 + col;
}

static const struct menu_item file_items[] = { MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU_SEP, MENU("Quit Calculator", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item edit_items[] = { MENU("Copy", CMD_COPY, 'c', MOD_CMD), MENU("Paste", CMD_PASTE, 'v', MOD_CMD), MENU_SEP, MENU("Clear", CMD_CLEAR, KEY_ESC, 0) };
static const struct menu_item view_items[] = { MENU("Basic", CMD_APP + 1, '1', MOD_CMD), MENU_DISABLED("Scientific", CMD_APP + 2) };
static const struct menu menus[] = { { "File", file_items, ARRAY_LEN(file_items) }, { "Edit", edit_items, ARRAY_LEN(edit_items) }, { "View", view_items, ARRAY_LEN(view_items) } };
static const struct app_menus calc_menus = { menus, ARRAY_LEN(menus) };

void calc_main(void *arg) {
    struct calc c = { 0 };
    c.pressed = -1; c.hover = -1;
    c.w = window_create("Calculator", CW, DISP_H + 5 * (BTN_SZ + GAP * 2), WIN_CLOSABLE | WIN_MINIMIZABLE);
    window_set_menus(c.w, &calc_menus);
    draw(&c);
    window_show(c.w);
    struct event ev;
    while (window_wait_event(c.w, &ev, -1)) {
        if (ev.type == EV_CLOSE || ev.type == EV_QUIT || (ev.type == EV_MENU && ev.cmd == CMD_QUIT)) break;
        if (ev.type == EV_MOUSE_DOWN && ev.button == BTN_LEFT) { c.pressed = key_at(ev.x, ev.y); draw(&c); }
        else if (ev.type == EV_MOUSE_UP && ev.button == BTN_LEFT) {
            int k = key_at(ev.x, ev.y);
            if (k >= 0 && k == c.pressed) press_key(&c, keys[k / 4][k % 4]);
            c.pressed = -1; draw(&c);
        } else if (ev.type == EV_KEY_DOWN) {
            char k[8] = { 0 };
            if (ev.ch >= '0' && ev.ch <= '9') { k[0] = ev.ch; }
            else if (ev.ch == '.') strcpy(k, "."); else if (ev.ch == '+') strcpy(k, "+"); else if (ev.ch == '-') strcpy(k, "−");
            else if (ev.ch == '*') strcpy(k, "×"); else if (ev.ch == '/') strcpy(k, "÷"); else if (ev.ch == '%') strcpy(k, "%");
            else if (ev.ch == '=' || ev.key == KEY_ENTER) strcpy(k, "="); else if (ev.key == KEY_ESC) strcpy(k, "AC");
            else if (ev.key == KEY_BACKSPACE && c.entering) { size_t l = strlen(c.entry); if (l) { if (c.entry[l - 1] == '.') c.has_dot = false; c.entry[l - 1] = 0; } if (!c.entry[0]) c.entering = false; }
            if (k[0]) press_key(&c, k);
            draw(&c);
        } else if (ev.type == EV_MENU) {
            if (ev.cmd == CMD_CLEAR) press_key(&c, "AC");
            else if (ev.cmd == CMD_COPY) { char d[48]; dec_format(current_value(&c), d, sizeof d); clipboard_set(d, strlen(d)); }
            else if (ev.cmd == CMD_PASTE) { char *s = clipboard_get(); if (s) { c.acc = dec_parse(s); c.entering = false; c.have_acc = true; kfree(s); } }
            draw(&c);
        }
    }
    window_destroy(c.w);
}
