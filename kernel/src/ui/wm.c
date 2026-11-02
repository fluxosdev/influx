/*
 * inFlux window server: compositing, input routing, menu bar, dock, cursor.
 * Runs in its own kernel thread ("WindowServer"); apps talk to it through the
 * window_* API and receive events through per-window queues.
 */
#include "ui.h"
#include "mm.h"
#include "string.h"
#include "io.h"

/* ---------------------------------------------------------------- state */
static struct mutex wm_lock;
static LIST_HEAD(windows);            /* bottom -> top */
static struct window *focused;
static int next_win_id = 1;
static int mouse_x, mouse_y, mouse_buttons;
static struct canvas *wallpaper;
static struct canvas *logo_icon;
static int scr_w, scr_h;

#define MAX_DAMAGE 24
static struct rect damage[MAX_DAMAGE];
static int ndamage;
static spinlock_t damage_lock = SPINLOCK_INIT;
static struct semaphore wake_sem;

static struct window *drag_win; static int drag_ox, drag_oy;
static struct window *resize_win; static int resize_ox, resize_oy, resize_w0, resize_h0;
static struct window *capture_win;          /* window receiving mouse events while a button is held */
static struct window *hover_win;
static int light_press_win_light = -1;      /* traffic light being pressed */

/* menus */
static int open_menu = -1;                  /* -1 none, -2 system menu, >=0 app menu index */
static int menu_hover = -1;
static struct rect menu_popup;              /* current popup rect */
static int menu_title_x[16], menu_title_w[16];
static int dock_hover = -1, dock_pressed = -1;
static struct rect dock_rect;
static int last_minute = -1;

static char *clipboard; static size_t clipboard_len;

static const struct menu_item system_items[] = {
    MENU("About inFlux", CMD_ABOUT, 0, 0), MENU_SEP,
    MENU("System Settings…", CMD_SETTINGS, ',', MOD_CMD), MENU("Task Manager…", CMD_TASKMAN, 0, 0), MENU_SEP,
    MENU("Restart…", CMD_RESTART, 0, 0), MENU("Shut Down…", CMD_SHUTDOWN, 0, 0),
};
static const struct menu system_menu = { "", system_items, ARRAY_LEN(system_items) };

/* ---------------------------------------------------------------- cursor */
static const char *cursor_art[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      XX    ",
};
#define CUR_W 12
#define CUR_H 17
static struct canvas *cursor_img;

static void cursor_init(void) {
    cursor_img = canvas_create(CUR_W + 2, CUR_H + 2);
    /* white outline: draw white around each black pixel, then black */
    for (int y = 0; y < CUR_H; y++) for (int x = 0; x < CUR_W; x++) {
        char ch = cursor_art[y][x];
        if (ch == ' ') continue;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int px = x + 1 + dx, py = y + 1 + dy;
            if (cursor_img->px[py * cursor_img->stride + px] == 0) cursor_img->px[py * cursor_img->stride + px] = RGB(255, 255, 255);
        }
    }
    for (int y = 0; y < CUR_H; y++) for (int x = 0; x < CUR_W; x++) {
        char ch = cursor_art[y][x];
        if (ch == 'X') cursor_img->px[(y + 1) * cursor_img->stride + x + 1] = RGB(0, 0, 0);
        else if (ch == '.') cursor_img->px[(y + 1) * cursor_img->stride + x + 1] = RGB(255, 255, 255);
    }
}
static struct rect cursor_rect(void) { return rect_make(mouse_x - 1, mouse_y - 1, CUR_W + 2, CUR_H + 2); }

/* ---------------------------------------------------------------- damage */
static void damage_add(struct rect r) {
    r = rect_intersect(r, rect_make(0, 0, scr_w, scr_h));
    if (rect_empty(r)) return;
    spin_lock(&damage_lock);
    for (int i = 0; i < ndamage; i++) {
        if (rect_overlaps(damage[i], r) || rect_overlaps(rect_make(r.x - 1, r.y - 1, r.w + 2, r.h + 2), damage[i])) { damage[i] = rect_union(damage[i], r); spin_unlock(&damage_lock); sem_post(&wake_sem); return; }
    }
    if (ndamage < MAX_DAMAGE) damage[ndamage++] = r;
    else { struct rect u = damage[0]; for (int i = 1; i < ndamage; i++) u = rect_union(u, damage[i]); damage[0] = rect_union(u, r); ndamage = 1; }
    spin_unlock(&damage_lock);
    sem_post(&wake_sem);
}
void wm_dirty_all(void) { damage_add(rect_make(0, 0, scr_w, scr_h)); }
int wm_screen_w(void) { return scr_w; }
int wm_screen_h(void) { return scr_h; }
struct rect wm_desktop_rect(void) { return rect_make(0, MENUBAR_H, scr_w, scr_h - MENUBAR_H - DOCK_H - DOCK_MARGIN); }

static struct rect win_outer(struct window *w) {   /* title bar + content */
    int top = (w->flags & WIN_FULL_TITLE) ? 0 : TITLEBAR_H;
    return rect_make(w->frame.x, w->frame.y - top, w->frame.w, w->frame.h + top);
}
static struct rect win_shadow_rect(struct window *w) {
    struct rect o = win_outer(w);
    int s = (w == focused) ? 30 : 16;
    return rect_make(o.x - s, o.y - s + 6, o.w + 2 * s, o.h + 2 * s);
}
static struct rect win_titlebar(struct window *w) { struct rect o = win_outer(w); return rect_make(o.x, o.y, o.w, TITLEBAR_H); }

/* ---------------------------------------------------------------- wallpaper */
static void wallpaper_build(void) {
    if (!wallpaper) wallpaper = canvas_create(scr_w, scr_h);
    gfx_vgradient(wallpaper, 0, 0, scr_w, scr_h, theme.wallpaper_top, theme.wallpaper_bottom);
    /* soft radial highlight at the upper third for depth */
    int cx = scr_w / 2, cy = scr_h / 3, rmax = scr_w * 3 / 4;
    for (int y = 0; y < scr_h; y++) {
        uint32_t *row = wallpaper->px + y * scr_w;
        for (int x = 0; x < scr_w; x++) {
            int dx = x - cx, dy = y - cy;
            int d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
            if (d >= rmax) continue;
            int a = (rmax - d) * 40 / rmax;
            row[x] = color_blend(row[x], RGBA(255, 255, 255, a));
        }
    }
}
void wm_theme_changed(void) { wallpaper_build(); wm_dirty_all(); wm_broadcast(EV_THEME); }
void wm_broadcast(int type) {
    mutex_lock(&wm_lock);
    struct window *w;
    list_for_each_entry(w, &windows, link) { struct event e = { .type = type }; window_post_event(w, &e); }
    mutex_unlock(&wm_lock);
}
bool clock_24h = false;

/* ---------------------------------------------------------------- windows API */
struct window *window_create(const char *title, int cw, int ch, int flags) {
    struct window *w = kzalloc(sizeof *w);
    w->id = next_win_id++;
    strlcpy(w->title, title, sizeof w->title);
    w->flags = flags & ~(WIN_VISIBLE | WIN_MINIMIZED);
    if (flags & WIN_DIALOG) w->flags = (w->flags & ~WIN_RESIZABLE & ~WIN_MINIMIZABLE) | WIN_CLOSABLE | WIN_DIALOG;
    int top = (w->flags & WIN_FULL_TITLE) ? TITLEBAR_H : 0;
    w->frame = rect_make(0, 0, cw, ch + top);
    w->canvas = canvas_create(cw, ch + top);
    gfx_fill(w->canvas, theme.bg);
    mutex_init(&w->lock);
    sem_init(&w->evq_sem, 0);
    w->evq_lock = (spinlock_t)SPINLOCK_INIT;
    w->min_w = 200; w->min_h = 100;
    w->light_hover = -1;
    w->owner = current_thread->proc;
    w->app_id = w->owner->app_id;
    if (w->app_id >= 0 && w->app_id < APP_COUNT) w->icon = icon_get(app_table[w->app_id].icon, 32);
    list_init(&w->link);
    /* cascade placement */
    mutex_lock(&wm_lock);
    struct rect d = wm_desktop_rect();
    int n = list_count(&windows);
    if (flags & WIN_DIALOG) { w->frame.x = (scr_w - cw) / 2; w->frame.y = (scr_h - ch) / 2 - 40; }
    else {
        int ox = 60 + (n % 8) * 28, oy = MENUBAR_H + TITLEBAR_H + 24 + (n % 8) * 26;
        if (ox + cw > d.x + d.w) ox = MAX(8, d.x + d.w - cw - 8);
        if (oy + ch + top > d.y + d.h) oy = MAX(MENUBAR_H + TITLEBAR_H + 4, d.y + d.h - ch - top);
        w->frame.x = ox; w->frame.y = oy;
    }
    mutex_unlock(&wm_lock);
    return w;
}

static void set_focus_locked(struct window *w) {
    if (focused == w) return;
    struct window *old = focused;
    focused = w;
    if (old) { struct event e = { .type = EV_BLUR }; window_post_event(old, &e); damage_add(win_shadow_rect(old)); }
    if (w) { struct event e = { .type = EV_FOCUS }; window_post_event(w, &e); damage_add(win_shadow_rect(w)); }
    open_menu = -1;
    damage_add(rect_make(0, 0, scr_w, MENUBAR_H));
}

void window_show(struct window *w) {
    mutex_lock(&wm_lock);
    if (!(w->flags & WIN_VISIBLE)) { w->flags |= WIN_VISIBLE; list_add_tail(&w->link, &windows); }
    w->flags &= ~WIN_MINIMIZED;
    set_focus_locked(w);
    damage_add(win_shadow_rect(w));
    mutex_unlock(&wm_lock);
    wm_notify_app_state();
}

void window_destroy(struct window *w) {
    mutex_lock(&wm_lock);
    struct rect r = win_shadow_rect(w);
    if (w->flags & WIN_VISIBLE) list_del(&w->link);
    w->flags &= ~WIN_VISIBLE;
    if (focused == w) {
        focused = NULL;
        /* focus the topmost remaining window */
        if (!list_empty(&windows)) {
            struct window *t = list_entry(windows.prev, struct window, link);
            if (!(t->flags & WIN_MINIMIZED)) set_focus_locked(t);
        }
        damage_add(rect_make(0, 0, scr_w, MENUBAR_H));
    }
    if (drag_win == w) drag_win = NULL;
    if (resize_win == w) resize_win = NULL;
    if (capture_win == w) capture_win = NULL;
    if (hover_win == w) hover_win = NULL;
    damage_add(r);
    mutex_unlock(&wm_lock);
    for (int i = w->evq_head; i != w->evq_tail; i = (i + 1) % EVQ_SIZE) if (w->evq[i].type == EV_OPEN) kfree(w->evq[i].path);
    mutex_lock(&w->lock);
    canvas_destroy(w->canvas);
    w->canvas = NULL;
    mutex_unlock(&w->lock);
    kfree(w);
    wm_notify_app_state();
}

struct canvas *window_begin_draw(struct window *w) { mutex_lock(&w->lock); return w->canvas; }
void window_end_draw(struct window *w, const struct rect *dirty) {
    mutex_unlock(&w->lock);
    if (!(w->flags & WIN_VISIBLE)) return;
    int top = (w->flags & WIN_FULL_TITLE) ? 0 : 0;
    if (dirty) damage_add(rect_make(w->frame.x + dirty->x, w->frame.y + dirty->y - top, dirty->w, dirty->h));
    else damage_add(w->frame);
}
void window_invalidate(struct window *w) { damage_add(win_outer(w)); }
void window_set_title(struct window *w, const char *title) { strlcpy(w->title, title, sizeof w->title); damage_add(win_titlebar(w)); }
void window_set_menus(struct window *w, const struct app_menus *m) { w->menus = m; if (focused == w) damage_add(rect_make(0, 0, scr_w, MENUBAR_H)); }
void window_set_timer(struct window *w, int ms) { w->timer_ms = ms; w->next_timer = system_ticks + ms; }
void window_set_min_size(struct window *w, int mw, int mh) { w->min_w = mw; w->min_h = mh; }
int window_content_top(struct window *w) { return (w->flags & WIN_FULL_TITLE) ? TITLEBAR_H : 0; }

void window_move(struct window *w, int x, int y) {
    mutex_lock(&wm_lock);
    damage_add(win_shadow_rect(w));
    w->frame.x = x; w->frame.y = y;
    damage_add(win_shadow_rect(w));
    mutex_unlock(&wm_lock);
}

static void resize_locked(struct window *w, int cw, int ch) {
    /* wm_lock held; ch is the canvas height */
    if (cw < w->min_w) cw = w->min_w;
    if (ch < w->min_h) ch = w->min_h;
    if (cw == w->frame.w && ch == w->frame.h) return;
    damage_add(win_shadow_rect(w));
    mutex_lock(&w->lock);
    struct canvas *nc = canvas_create(cw, ch);
    gfx_fill(nc, theme.bg);
    gfx_blit_opaque(nc, 0, 0, w->canvas);
    canvas_destroy(w->canvas);
    w->canvas = nc;
    w->frame.w = cw; w->frame.h = ch;
    mutex_unlock(&w->lock);
    struct event e = { .type = EV_RESIZE, .w = cw, .h = ch };
    window_post_event(w, &e);
    damage_add(win_shadow_rect(w));
}
void window_resize(struct window *w, int cw, int ch) { mutex_lock(&wm_lock); resize_locked(w, cw, ch); mutex_unlock(&wm_lock); }

static void raise_locked(struct window *w) {
    if (!(w->flags & WIN_VISIBLE)) return;
    list_del(&w->link);
    list_add_tail(&w->link, &windows);
    damage_add(win_shadow_rect(w));
}
void window_focus(struct window *w) {
    mutex_lock(&wm_lock);
    w->flags &= ~WIN_MINIMIZED;
    if (!(w->flags & WIN_VISIBLE)) { w->flags |= WIN_VISIBLE; list_add_tail(&w->link, &windows); }
    raise_locked(w);
    set_focus_locked(w);
    mutex_unlock(&wm_lock);
}
void window_minimize(struct window *w) {
    mutex_lock(&wm_lock);
    w->flags |= WIN_MINIMIZED;
    damage_add(win_shadow_rect(w));
    if (focused == w) {
        focused = NULL;
        struct window *t;
        list_for_each_entry(t, &windows, link) if (!(t->flags & WIN_MINIMIZED)) focused = t;   /* topmost visible */
        if (focused) { struct event e = { .type = EV_FOCUS }; window_post_event(focused, &e); damage_add(win_shadow_rect(focused)); }
        damage_add(rect_make(0, 0, scr_w, MENUBAR_H));
    }
    mutex_unlock(&wm_lock);
}

void window_post_event(struct window *w, const struct event *ev) {
    spin_lock(&w->evq_lock);
    int next = (w->evq_tail + 1) % EVQ_SIZE;
    if (ev->type == EV_MOUSE_MOVE && w->evq_tail != w->evq_head) {
        int last = (w->evq_tail + EVQ_SIZE - 1) % EVQ_SIZE;
        if (w->evq[last].type == EV_MOUSE_MOVE) { w->evq[last] = *ev; spin_unlock(&w->evq_lock); return; }
    }
    if (next == w->evq_head) { spin_unlock(&w->evq_lock); return; }   /* full: drop */
    w->evq[w->evq_tail] = *ev;
    w->evq_tail = next;
    spin_unlock(&w->evq_lock);
    sem_post(&w->evq_sem);
}

bool window_wait_event(struct window *w, struct event *ev, int timeout_ms) {
    for (;;) {
        if (thread_should_exit()) { memset(ev, 0, sizeof *ev); ev->type = EV_QUIT; return true; }
        int wait = timeout_ms;
        if (w->timer_ms > 0) {
            int64_t until = (int64_t)w->next_timer - (int64_t)system_ticks;
            if (until <= 0) { w->next_timer = system_ticks + w->timer_ms; memset(ev, 0, sizeof *ev); ev->type = EV_TIMER; return true; }
            if (wait < 0 || until < wait) wait = (int)until;
        }
        bool got = wait < 0 ? (sem_wait(&w->evq_sem), true) : sem_wait_timeout(&w->evq_sem, wait);
        if (!got) {
            if (w->timer_ms > 0 && (int64_t)system_ticks >= (int64_t)w->next_timer) continue;
            return false;
        }
        spin_lock(&w->evq_lock);
        if (w->evq_head == w->evq_tail) { spin_unlock(&w->evq_lock); continue; }
        *ev = w->evq[w->evq_head];
        w->evq_head = (w->evq_head + 1) % EVQ_SIZE;
        spin_unlock(&w->evq_lock);
        return true;
    }
}

struct window *window_find(int id) { struct window *w; list_for_each_entry(w, &windows, link) if (w->id == id) return w; return NULL; }
int window_list(struct window **out, int max) { int n = 0; struct window *w; mutex_lock(&wm_lock); list_for_each_entry(w, &windows, link) if (n < max) out[n++] = w; mutex_unlock(&wm_lock); return n; }
struct window *window_top_of_process(struct process *p) {
    struct window *w, *best = NULL;
    list_for_each_entry(w, &windows, link) if (w->owner == p) best = w;
    return best;
}

void clipboard_set(const char *text, size_t n) {
    mutex_lock(&wm_lock);
    kfree(clipboard);
    clipboard = kmalloc(n + 1); memcpy(clipboard, text, n); clipboard[n] = 0; clipboard_len = n;
    mutex_unlock(&wm_lock);
}
char *clipboard_get(void) {
    mutex_lock(&wm_lock);
    char *r = clipboard ? strdup(clipboard) : NULL;
    mutex_unlock(&wm_lock);
    return r;
}

/* ---------------------------------------------------------------- drawing */
static void draw_rounded_blit(struct canvas *dst, int x, int y, const struct canvas *src, int r, int mask) {
    /* plain blit minus the corner squares, then corners with coverage */
    int w = src->w, h = src->h;
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    gfx_blit_rect(dst, x + r, y, src, rect_make(r, 0, w - 2 * r, h), true);
    gfx_blit_rect(dst, x, y + r, src, rect_make(0, r, r, h - 2 * r), true);
    gfx_blit_rect(dst, x + w - r, y + r, src, rect_make(w - r, r, r, h - 2 * r), true);
    struct rect clip = dst->clip;
    for (int cy = 0; cy < r; cy++) for (int cx = 0; cx < r; cx++) {
        int dx = (cx * 16 + 8) - r * 16, dy = (cy * 16 + 8) - r * 16;
        int d = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
        int cov = r * 16 + 8 - d; cov = CLAMP(cov, 0, 16);
        int corners[4][3] = { { x + cx, y + cy, 1 }, { x + w - 1 - cx, y + cy, 2 }, { x + cx, y + h - 1 - cy, 4 }, { x + w - 1 - cx, y + h - 1 - cy, 8 } };
        int srcpos[4][2] = { { cx, cy }, { w - 1 - cx, cy }, { cx, h - 1 - cy }, { w - 1 - cx, h - 1 - cy } };
        for (int k = 0; k < 4; k++) {
            int px = corners[k][0], py = corners[k][1];
            if (!rect_contains(clip, px, py)) continue;
            uint32_t s = src->px[srcpos[k][1] * src->stride + srcpos[k][0]];
            int c = (mask & corners[k][2]) ? cov : 16;
            if (c == 0) continue;
            uint32_t a = ALPHA(s) * c / 16;
            uint32_t *d = dst->px + py * dst->stride + px;
            *d = color_blend(*d, WITH_ALPHA(s, a));
        }
    }
}

static void draw_traffic_lights(struct canvas *c, struct window *w, struct rect tb) {
    bool active = (w == focused);
    static const color_t cols[3] = { 0xFFFF5F57, 0xFFFEBC2E, 0xFF28C840 };
    static const color_t rims[3] = { 0xFFE0443E, 0xFFDEA123, 0xFF1AAB29 };
    int cy = tb.y + TITLEBAR_H / 2;
    for (int i = 0; i < 3; i++) {
        bool enabled = (i == 0 && (w->flags & WIN_CLOSABLE)) || (i == 1 && (w->flags & WIN_MINIMIZABLE)) || (i == 2 && (w->flags & WIN_RESIZABLE));
        int cx = tb.x + 20 + i * 20;
        color_t fill = active && enabled ? cols[i] : (theme.dark ? RGB(0x4A, 0x4A, 0x4E) : RGB(0xDD, 0xDD, 0xE0));
        color_t rim = active && enabled ? rims[i] : (theme.dark ? RGB(0x3A, 0x3A, 0x3E) : RGB(0xC8, 0xC8, 0xCC));
        gfx_fill_circle(c, cx, cy, 6, rim);
        gfx_fill_circle(c, cx, cy, 5, fill);
        if (w->light_hover >= 0 && enabled && active) {
            color_t sym = RGBA(0, 0, 0, 0xA0);
            if (i == 0) { gfx_line(c, cx - 3, cy - 3, cx + 3, cy + 3, sym); gfx_line(c, cx + 3, cy - 3, cx - 3, cy + 3, sym); }
            else if (i == 1) gfx_fill_rect(c, cx - 3, cy, 7, 2, sym);
            else { gfx_fill_rect(c, cx - 3, cy, 7, 2, sym); gfx_fill_rect(c, cx, cy - 3, 2, 7, sym); }
        }
    }
}

static void draw_window(struct canvas *c, struct window *w) {
    struct rect o = win_outer(w);
    bool active = (w == focused);
    if (!rect_overlaps(win_shadow_rect(w), c->clip)) return;
    gfx_shadow_ex(c, o.x, o.y + 6, o.w, o.h, WIN_RADIUS, active ? 30 : 16, active ? 120 : 70, o);
    gfx_shadow_ex(c, o.x, o.y, o.w, o.h, WIN_RADIUS, 2, 90, o);
    mutex_lock(&w->lock);
    if (w->flags & WIN_FULL_TITLE) {
        draw_rounded_blit(c, o.x, o.y, w->canvas, WIN_RADIUS, 15);
    } else {
        gfx_fill_rounded_rect_corners(c, o.x, o.y, o.w, TITLEBAR_H, WIN_RADIUS, active ? theme.titlebar : theme.titlebar_unfocused, 3);
        gfx_hline(c, o.x, o.y + TITLEBAR_H - 1, o.w, theme.titlebar_line);
        draw_rounded_blit(c, w->frame.x, w->frame.y, w->canvas, WIN_RADIUS, 12);
    }
    mutex_unlock(&w->lock);
    struct rect tb = rect_make(o.x, o.y, o.w, TITLEBAR_H);
    draw_traffic_lights(c, w, tb);
    if (!(w->flags & WIN_DIALOG) || w->title[0]) {
        int tw = font_text_width(font_ui_semibold, w->title);
        bool has_icon = w->icon && !(w->flags & WIN_DIALOG);
        int iw = has_icon ? 22 : 0;
        int maxw = o.w - 84 - 12 - iw;
        int x = o.x + (o.w - MIN(tw, maxw) - iw) / 2 + iw;
        if (x < o.x + 84 + iw) x = o.x + 84 + iw;
        if (has_icon) gfx_blit_scaled(c, x - 22, tb.y + (TITLEBAR_H - 16) / 2, 16, 16, w->icon);
        font_draw_ellipsized(c, font_ui_semibold, x, tb.y + (TITLEBAR_H - font_height(font_ui_semibold)) / 2, maxw, active ? theme.title_text : theme.title_text_unfocused, w->title);
    }
    /* hairline border */
    gfx_draw_rounded_rect(c, o.x, o.y, o.w, o.h, WIN_RADIUS, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x38));
}

/* ---- menu bar ---- */
static const char *weekday_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *month_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static void clock_string(char *out, size_t n) {
    struct rtc_time t; unix_to_datetime(time_unix() + tz_offset_minutes * 60, &t);
    int h12 = t.hour % 12; if (h12 == 0) h12 = 12;
    if (clock_24h) snprintf(out, n, "%s %s %d  %02d:%02d", weekday_names[day_of_week(t.year, t.month, t.day)], month_names[(t.month - 1) % 12], t.day, t.hour, t.minute);
    else snprintf(out, n, "%s %s %d  %d:%02d %s", weekday_names[day_of_week(t.year, t.month, t.day)], month_names[(t.month - 1) % 12], t.day, h12, t.minute, t.hour >= 12 ? "PM" : "AM");
}
static const char *current_app_name(void) {
    if (focused && focused->app_id >= 0 && focused->app_id < APP_COUNT && !(focused->flags & WIN_UTILITY)) return app_table[focused->app_id].name;
    if (focused && focused->owner) return focused->owner->name;
    return "Finder";
}
static const struct app_menus *current_menus(void) { return focused && !(focused->flags & WIN_UTILITY) ? focused->menus : NULL; }

static void draw_menubar(struct canvas *c) {
    if (!rect_overlaps(rect_make(0, 0, scr_w, MENUBAR_H), c->clip)) return;
    gfx_fill_rect(c, 0, 0, scr_w, MENUBAR_H, theme.menubar_bg);
    gfx_hline(c, 0, MENUBAR_H - 1, scr_w, theme.dark ? RGBA(255, 255, 255, 0x18) : RGBA(0, 0, 0, 0x20));
    int x = 14;
    /* system logo */
    if (open_menu == -2) gfx_fill_rounded_rect(c, x - 6, 3, 30, MENUBAR_H - 6, 5, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
    if (logo_icon) gfx_blit_scaled_tinted(c, x, (MENUBAR_H - 16) / 2, 16, 16, logo_icon, theme.menubar_text);
    x += 32;
    const char *app = current_app_name();
    int fh = font_height(font_ui_bold), ty = (MENUBAR_H - fh) / 2;
    int aw = font_text_width(font_ui_bold, app);
    menu_title_x[0] = x - 8; menu_title_w[0] = aw + 16;
    font_draw(c, font_ui_bold, x, ty, theme.menubar_text, app);
    x += aw + 18;
    const struct app_menus *m = current_menus();
    if (m) for (int i = 0; i < m->count && i < 15; i++) {
        int w = font_text_width(font_ui, m->menus[i].title);
        menu_title_x[i + 1] = x - 8; menu_title_w[i + 1] = w + 16;
        if (open_menu == i) {
            gfx_fill_rounded_rect(c, x - 8, 3, w + 16, MENUBAR_H - 6, 5, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18));
        }
        font_draw(c, font_ui, x, ty, theme.menubar_text, m->menus[i].title);
        x += w + 18;
    }
    char clk[64]; clock_string(clk, sizeof clk);
    int cw = font_text_width(font_ui, clk);
    font_draw(c, font_ui, scr_w - cw - 14, ty, theme.menubar_text, clk);
}

static const struct menu *open_menu_def(void) {
    if (open_menu == -2) return &system_menu;
    const struct app_menus *m = current_menus();
    if (m && open_menu >= 0 && open_menu < m->count) return &m->menus[open_menu];
    return NULL;
}
static void shortcut_string(const struct menu_item *it, char *out, size_t n) {
    out[0] = 0;
    if (!it->key) return;
    char s[32] = ""; 
    if (it->mods & MOD_CTRL) strlcat(s, "⌃", sizeof s);
    if (it->mods & MOD_ALT) strlcat(s, "⌥", sizeof s);
    if (it->mods & MOD_SHIFT) strlcat(s, "⇧", sizeof s);
    if (it->mods & MOD_CMD) strlcat(s, "⌘", sizeof s);
    char k[8];
    if (it->key == KEY_BACKSPACE) strcpy(k, "⌫"); else if (it->key == KEY_ENTER) strcpy(k, "⏎"); else if (it->key == KEY_DELETE) strcpy(k, "⌦");
    else if (it->key == KEY_LEFT) strcpy(k, "←"); else if (it->key == KEY_RIGHT) strcpy(k, "→"); else if (it->key == KEY_UP) strcpy(k, "↑"); else if (it->key == KEY_DOWN) strcpy(k, "↓");
    else if (it->key == ',') strcpy(k, ","); else { k[0] = toupper(it->key); k[1] = 0; }
    strlcat(s, k, sizeof s);
    strlcpy(out, s, n);
}
#define MENU_ITEM_H 22
#define MENU_SEP_H 11
static void layout_menu(void) {
    const struct menu *m = open_menu_def();
    if (!m) return;
    int w = 180, h = 10;
    for (int i = 0; i < m->count; i++) {
        if (m->items[i].separator) { h += MENU_SEP_H; continue; }
        char sc[32]; shortcut_string(&m->items[i], sc, sizeof sc);
        int iw = font_text_width(font_ui, m->items[i].label) + 60 + (sc[0] ? font_text_width(font_ui, sc) + 20 : 0);
        if (iw > w) w = iw;
        h += MENU_ITEM_H;
    }
    int idx = open_menu == -2 ? 0 : open_menu + 1;
    int x = open_menu == -2 ? 8 : menu_title_x[idx];
    if (x + w > scr_w - 8) x = scr_w - 8 - w;
    menu_popup = rect_make(x, MENUBAR_H + 1, w, h);
}
static struct rect menu_popup_damage(void) { return rect_make(menu_popup.x - 30, menu_popup.y - 10, menu_popup.w + 60, menu_popup.h + 50); }

static void draw_menu_popup(struct canvas *c) {
    const struct menu *m = open_menu_def();
    if (!m) return;
    struct rect p = menu_popup;
    if (!rect_overlaps(menu_popup_damage(), c->clip)) return;
    gfx_shadow_ex(c, p.x, p.y + 4, p.w, p.h, 8, 22, 90, p);
    gfx_fill_rounded_rect(c, p.x, p.y, p.w, p.h, 8, theme.menu_bg);
    gfx_draw_rounded_rect(c, p.x, p.y, p.w, p.h, 8, theme.dark ? RGBA(255, 255, 255, 0x28) : RGBA(0, 0, 0, 0x28));
    int y = p.y + 5;
    for (int i = 0; i < m->count; i++) {
        const struct menu_item *it = &m->items[i];
        if (it->separator) { gfx_hline(c, p.x + 10, y + MENU_SEP_H / 2, p.w - 20, theme.menu_sep); y += MENU_SEP_H; continue; }
        bool hl = (menu_hover == i) && !it->disabled;
        if (hl) gfx_fill_rounded_rect(c, p.x + 5, y, p.w - 10, MENU_ITEM_H, 5, theme.accent);
        color_t tc = it->disabled ? theme.text_disabled : hl ? theme.accent_text : theme.menu_text;
        int ty = y + (MENU_ITEM_H - font_height(font_ui)) / 2;
        if (it->checked) font_draw(c, font_ui, p.x + 10, ty, tc, "✓");
        font_draw(c, font_ui, p.x + 24, ty, tc, it->label);
        char sc[32]; shortcut_string(it, sc, sizeof sc);
        if (sc[0]) { int sw = font_text_width(font_ui, sc); font_draw(c, font_ui, p.x + p.w - 14 - sw, ty, it->disabled ? theme.text_disabled : hl ? theme.accent_text : theme.text_secondary, sc); }
        y += MENU_ITEM_H;
    }
}
static int menu_item_at(int mx, int my) {
    const struct menu *m = open_menu_def();
    if (!m || !rect_contains(menu_popup, mx, my)) return -1;
    int y = menu_popup.y + 5;
    for (int i = 0; i < m->count; i++) {
        int h = m->items[i].separator ? MENU_SEP_H : MENU_ITEM_H;
        if (my >= y && my < y + h) return m->items[i].separator ? -1 : i;
        y += h;
    }
    return -1;
}

/* ---- dock ---- */
struct dock_item { int app; const char *icon; const char *label; bool separator; };
static const struct dock_item dock_items[] = {
    { APP_FINDER, "finder", "Finder", false }, { APP_TERMINAL, "terminal", "Terminal", false },
    { APP_EDITOR, "editor", "Text Editor", false }, { APP_CALC, "calculator", "Calculator", false },
    { APP_TASKMAN, "taskman", "Task Manager", false }, { APP_SETTINGS, "settings", "System Settings", false },
    { -1, NULL, NULL, true }, { -2, "trash", "Trash", false },
};
#define NDOCK ((int)ARRAY_LEN(dock_items))
static int dock_item_x[NDOCK];
static bool app_running_cache[APP_COUNT];
static bool trash_full;

static void layout_dock(void) {
    int w = 16;
    for (int i = 0; i < NDOCK; i++) { dock_item_x[i] = w; w += dock_items[i].separator ? 18 : DOCK_ICON + 8; }
    w += 8;
    dock_rect = rect_make((scr_w - w) / 2, scr_h - DOCK_H - DOCK_MARGIN, w, DOCK_H);
}
static struct rect dock_damage(void) { return rect_make(dock_rect.x - 20, dock_rect.y - 50, dock_rect.w + 40, dock_rect.h + DOCK_MARGIN + 50); }
void wm_notify_app_state(void) {
    for (int i = 0; i < APP_COUNT; i++) app_running_cache[i] = app_is_running(i);
    damage_add(dock_damage());
}
static void draw_dock(struct canvas *c) {
    if (!rect_overlaps(dock_damage(), c->clip)) return;
    struct rect d = dock_rect;
    gfx_shadow_ex(c, d.x, d.y + 4, d.w, d.h, 18, 16, 60, d);
    gfx_fill_rounded_rect(c, d.x, d.y, d.w, d.h, 18, theme.dock_bg);
    gfx_draw_rounded_rect(c, d.x, d.y, d.w, d.h, 18, theme.dock_border);
    for (int i = 0; i < NDOCK; i++) {
        const struct dock_item *it = &dock_items[i];
        int x = d.x + dock_item_x[i];
        if (it->separator) { gfx_vline(c, x + 8, d.y + 12, d.h - 24, theme.dark ? RGBA(255, 255, 255, 0x40) : RGBA(0, 0, 0, 0x30)); continue; }
        const char *icon = it->icon;
        if (it->app == -2 && trash_full) icon = "trash";
        bool hov = (dock_hover == i);
        int sz = hov ? DOCK_ICON + 8 : DOCK_ICON;
        int ix = x + (DOCK_ICON - sz) / 2, iy = d.y + 6 + (DOCK_ICON - sz) / 2 - (hov ? 4 : 0);
        if (dock_pressed == i) iy += 2;
        struct canvas *img = icon_get(icon, 64);
        if (img) gfx_blit_scaled(c, ix, iy, sz, sz, img);
        bool running = it->app >= 0 && app_running_cache[it->app];
        if (running) gfx_fill_circle(c, x + DOCK_ICON / 2, d.y + d.h - 5, 2, theme.dark ? RGBA(255, 255, 255, 0xC0) : RGBA(0, 0, 0, 0x90));
        if (hov) {
            /* tooltip */
            int tw = font_text_width(font_ui_medium, it->label) + 20, th = 24;
            int tx = x + DOCK_ICON / 2 - tw / 2, ty = d.y - th - 14;
            gfx_shadow(c, tx, ty + 2, tw, th, 6, 8, 60);
            gfx_fill_rounded_rect(c, tx, ty, tw, th, 6, theme.dark ? RGBA(0x30, 0x30, 0x34, 0xF0) : RGBA(0xF4, 0xF4, 0xF6, 0xF4));
            font_draw_centered(c, font_ui_medium, rect_make(tx, ty, tw, th), theme.text, it->label);
        }
    }
}
static int dock_item_at(int mx, int my) {
    if (!rect_contains(dock_rect, mx, my)) return -1;
    for (int i = 0; i < NDOCK; i++) {
        if (dock_items[i].separator) continue;
        int x = dock_rect.x + dock_item_x[i];
        if (mx >= x - 4 && mx < x + DOCK_ICON + 4) return i;
    }
    return -1;
}

/* ---- compose ---- */
static void compose(struct rect r) {
    canvas_set_clip(screen, r);
    gfx_blit_rect(screen, r.x, r.y, wallpaper, r, false);
    struct window *w;
    list_for_each_entry(w, &windows, link) if (!(w->flags & WIN_MINIMIZED)) draw_window(screen, w);
    draw_dock(screen);
    draw_menubar(screen);
    if (open_menu != -1) draw_menu_popup(screen);
    gfx_blit(screen, mouse_x - 1, mouse_y - 1, cursor_img);
    canvas_reset_clip(screen);
    fb_flush(r);
}

static void flush_damage(void) {
    struct rect local[MAX_DAMAGE]; int n;
    spin_lock(&damage_lock);
    n = ndamage; memcpy(local, damage, n * sizeof(struct rect)); ndamage = 0;
    spin_unlock(&damage_lock);
    if (!n) return;
    mutex_lock(&wm_lock);
    for (int i = 0; i < n; i++) compose(local[i]);
    mutex_unlock(&wm_lock);
}

/* ---------------------------------------------------------------- input handling */
static struct window *window_at(int x, int y, bool *in_title) {
    struct window *w;
    for (struct list_head *p = windows.prev; p != &windows; p = p->prev) {
        w = list_entry(p, struct window, link);
        if (w->flags & WIN_MINIMIZED) continue;
        struct rect o = win_outer(w);
        if (rect_contains(o, x, y)) {
            if (in_title) *in_title = y < o.y + TITLEBAR_H;
            return w;
        }
    }
    return NULL;
}
static void send_mouse(struct window *w, int type, int x, int y, int button, int scroll) {
    struct event e = { 0 };
    e.type = type; e.x = x - w->frame.x; e.y = y - w->frame.y + ((w->flags & WIN_FULL_TITLE) ? 0 : 0);
    e.button = button; e.buttons = mouse_buttons; e.scroll = scroll; e.mods = input_modifiers();
    window_post_event(w, &e);
}
static int traffic_light_at(struct window *w, int x, int y) {
    struct rect tb = win_titlebar(w);
    int cy = tb.y + TITLEBAR_H / 2;
    for (int i = 0; i < 3; i++) { int cx = tb.x + 20 + i * 20; if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 64) return i; }
    return -1;
}
static void close_menu(void) { if (open_menu != -1) { damage_add(menu_popup_damage()); open_menu = -1; menu_hover = -1; damage_add(rect_make(0, 0, scr_w, MENUBAR_H)); } }
static void open_menu_idx(int idx) {
    close_menu();
    open_menu = idx; menu_hover = -1;
    layout_menu();
    damage_add(menu_popup_damage());
    damage_add(rect_make(0, 0, scr_w, MENUBAR_H));
}
static int menu_title_at(int x, int y) {
    if (y >= MENUBAR_H) return -1;
    if (x >= 4 && x < 40) return -2;
    const struct app_menus *m = current_menus();
    if (!m) return -1;
    for (int i = 0; i < m->count && i < 15; i++) if (x >= menu_title_x[i + 1] && x < menu_title_x[i + 1] + menu_title_w[i + 1]) return i;
    return -1;
}
static void toggle_zoom(struct window *w) {
    if (!(w->flags & WIN_RESIZABLE)) return;
    struct rect d = wm_desktop_rect();
    int top = (w->flags & WIN_FULL_TITLE) ? 0 : TITLEBAR_H;
    if (!w->zoomed) {
        w->saved_frame = w->frame; w->zoomed = true;
        damage_add(win_shadow_rect(w));
        w->frame.x = d.x + 4; w->frame.y = d.y + top + 4;
        resize_locked(w, d.w - 8, d.h - top - 8);
    } else {
        w->zoomed = false;
        damage_add(win_shadow_rect(w));
        w->frame.x = w->saved_frame.x; w->frame.y = w->saved_frame.y;
        resize_locked(w, w->saved_frame.w, w->saved_frame.h);
    }
    damage_add(win_shadow_rect(w));
}

static void handle_menu_command(int cmd) {
    if (cmd == CMD_ABOUT || cmd == CMD_SETTINGS || cmd == CMD_TASKMAN || cmd == CMD_RESTART || cmd == CMD_SHUTDOWN) { wm_system_command(cmd); return; }
    if (!focused) return;
    if (cmd == CMD_CLOSE) { struct event e = { .type = EV_CLOSE }; window_post_event(focused, &e); return; }
    if (cmd == CMD_MINIMIZE) { struct window *f = focused; mutex_unlock(&wm_lock); window_minimize(f); mutex_lock(&wm_lock); return; }
    if (cmd == CMD_ZOOM) { toggle_zoom(focused); return; }
    struct event e = { .type = EV_MENU, .cmd = cmd };
    window_post_event(focused, &e);
}

static void on_mouse_button(const struct input_event *in) {
    int x = mouse_x, y = mouse_y;
    mouse_buttons = in->buttons;
    if (in->pressed && in->button == BTN_LEFT) {
        if (open_menu != -1) {
            int item = menu_item_at(x, y);
            int title = menu_title_at(x, y);
            if (item >= 0) { const struct menu *m = open_menu_def(); int cmd = m->items[item].cmd; close_menu(); handle_menu_command(cmd); }
            else if (title != -1 && title != open_menu) open_menu_idx(title);
            else close_menu();
            return;
        }
        int title = menu_title_at(x, y);
        if (title != -1) { open_menu_idx(title); return; }
        if (y < MENUBAR_H) return;
        int di = dock_item_at(x, y);
        if (di >= 0) { dock_pressed = di; damage_add(dock_damage()); return; }
        bool in_title = false;
        struct window *w = window_at(x, y, &in_title);
        if (!w) { if (focused) { struct window *f = focused; UNUSED(f); } return; }
        if (w != focused) set_focus_locked(w);
        raise_locked(w);
        if (in_title) {
            int l = traffic_light_at(w, x, y);
            if (l >= 0) { light_press_win_light = l; return; }
            drag_win = w; drag_ox = x - w->frame.x; drag_oy = y - w->frame.y;
            return;
        }
        if ((w->flags & WIN_RESIZABLE) && x >= w->frame.x + w->frame.w - 16 && y >= w->frame.y + w->frame.h - 16) {
            resize_win = w; resize_ox = x; resize_oy = y; resize_w0 = w->frame.w; resize_h0 = w->frame.h;
            return;
        }
        capture_win = w;
        send_mouse(w, EV_MOUSE_DOWN, x, y, in->button, 0);
        return;
    }
    if (in->pressed) {   /* right / middle */
        struct window *w = window_at(x, y, NULL);
        if (w && rect_contains(w->frame, x, y)) { if (w != focused) { set_focus_locked(w); raise_locked(w); } send_mouse(w, EV_MOUSE_DOWN, x, y, in->button, 0); }
        return;
    }
    /* release */
    if (in->button == BTN_LEFT) {
        if (dock_pressed >= 0) {
            int di = dock_item_at(x, y);
            int pressed = dock_pressed; dock_pressed = -1; damage_add(dock_damage());
            if (di == pressed) {
                const struct dock_item *it = &dock_items[di];
                mutex_unlock(&wm_lock);
                if (it->app >= 0) { if (app_is_running(it->app)) app_activate(it->app); else app_launch(it->app, NULL); }
                else if (it->app == -2) app_launch(APP_FINDER, "/home/flux/.Trash");
                mutex_lock(&wm_lock);
            }
            return;
        }
        if (light_press_win_light >= 0 && focused) {
            int l = light_press_win_light; light_press_win_light = -1;
            if (traffic_light_at(focused, x, y) == l) {
                if (l == 0 && (focused->flags & WIN_CLOSABLE)) { struct event e = { .type = EV_CLOSE }; window_post_event(focused, &e); }
                else if (l == 1 && (focused->flags & WIN_MINIMIZABLE)) { struct window *f = focused; mutex_unlock(&wm_lock); window_minimize(f); mutex_lock(&wm_lock); }
                else if (l == 2) toggle_zoom(focused);
            }
            return;
        }
        drag_win = NULL; resize_win = NULL;
        if (capture_win) { send_mouse(capture_win, EV_MOUSE_UP, x, y, in->button, 0); if (!mouse_buttons) capture_win = NULL; }
    } else {
        struct window *w = capture_win ? capture_win : window_at(x, y, NULL);
        if (w) send_mouse(w, EV_MOUSE_UP, x, y, in->button, 0);
    }
}

static void on_mouse_move(const struct input_event *in) {
    struct rect old = cursor_rect();
    mouse_x = CLAMP(mouse_x + in->dx, 0, scr_w - 1);
    mouse_y = CLAMP(mouse_y + in->dy, 0, scr_h - 1);
    damage_add(old); damage_add(cursor_rect());
    int x = mouse_x, y = mouse_y;
    if (drag_win) {
        damage_add(win_shadow_rect(drag_win));
        drag_win->frame.x = x - drag_ox;
        drag_win->frame.y = MAX(y - drag_oy, MENUBAR_H + ((drag_win->flags & WIN_FULL_TITLE) ? 0 : TITLEBAR_H));
        drag_win->zoomed = false;
        damage_add(win_shadow_rect(drag_win));
        return;
    }
    if (resize_win) {
        resize_locked(resize_win, resize_w0 + (x - resize_ox), resize_h0 + (y - resize_oy));
        resize_win->zoomed = false;
        return;
    }
    if (open_menu != -1) {
        int item = menu_item_at(x, y);
        if (item != menu_hover) { menu_hover = item; damage_add(menu_popup); }
        int title = menu_title_at(x, y);
        if (title != -1 && title != open_menu) open_menu_idx(title);
        return;
    }
    int di = dock_item_at(x, y);
    if (di != dock_hover) { dock_hover = di; damage_add(dock_damage()); }
    if (capture_win) { send_mouse(capture_win, EV_MOUSE_MOVE, x, y, 0, 0); return; }
    bool in_title = false;
    struct window *w = window_at(x, y, &in_title);
    /* traffic light hover feedback */
    struct window *f = focused;
    if (f) {
        int l = (w == f && in_title) ? traffic_light_at(f, x, y) : -1;
        int newh = l >= 0 ? l : (w == f && in_title && x < f->frame.x + 70 ? 0 : -1);
        if (newh != f->light_hover) { f->light_hover = newh; damage_add(win_titlebar(f)); }
    }
    if (hover_win && hover_win != w) { struct event e = { .type = EV_MOUSE_LEAVE }; window_post_event(hover_win, &e); }
    hover_win = w;
    if (w && !in_title && rect_contains(w->frame, x, y)) send_mouse(w, EV_MOUSE_MOVE, x, y, 0, 0);
}

static void on_scroll(const struct input_event *in) {
    struct window *w = window_at(mouse_x, mouse_y, NULL);
    if (w && rect_contains(w->frame, mouse_x, mouse_y)) send_mouse(w, EV_MOUSE_SCROLL, mouse_x, mouse_y, 0, in->scroll);
}

static bool menu_shortcut(const struct app_menus *m, int key, int mods, int *cmd, bool ctrl_alias) {
    if (!m) return false;
    int want = mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_CMD);
    for (int i = 0; i < m->count; i++) for (int j = 0; j < m->menus[i].count; j++) {
        const struct menu_item *it = &m->menus[i].items[j];
        if (it->separator || !it->key || it->disabled) continue;
        int k = it->key; int km = it->mods;
        int kk = key; if (kk >= 'A' && kk <= 'Z') kk += 32;
        /* accept Ctrl as an alias for Cmd on PC keyboards */
        int alias = km; if ((km & MOD_CMD) && ctrl_alias) alias = (km & ~MOD_CMD) | MOD_CTRL;
        if (kk == k && (want == km || want == alias)) { *cmd = it->cmd; return true; }
    }
    return false;
}

static void cycle_windows(void) {
    if (list_count(&windows) < 2) return;
    struct window *bottom = list_entry(windows.next, struct window, link);
    /* move the current top to the bottom and focus the next */
    struct window *top = list_entry(windows.prev, struct window, link);
    list_del(&top->link); list_add(&top->link, &windows);
    UNUSED(bottom);
    struct window *nt = list_entry(windows.prev, struct window, link);
    nt->flags &= ~WIN_MINIMIZED;
    set_focus_locked(nt);
    damage_add(win_shadow_rect(top)); damage_add(win_shadow_rect(nt));
}

static void on_key(const struct input_event *in) {
    if (in->type == INPUT_KEY_DOWN) {
        if (open_menu != -1) {
            if (in->key == KEY_ESC) { close_menu(); return; }
            const struct menu *m = open_menu_def();
            if (in->key == KEY_DOWN || in->key == KEY_UP) {
                int dir = in->key == KEY_DOWN ? 1 : -1, i = menu_hover;
                for (int k = 0; k < m->count; k++) { i = (i + dir + m->count) % m->count; if (!m->items[i].separator && !m->items[i].disabled) break; }
                menu_hover = i; damage_add(menu_popup); return;
            }
            if (in->key == KEY_ENTER && menu_hover >= 0) { int cmd = m->items[menu_hover].cmd; close_menu(); handle_menu_command(cmd); return; }
            if (in->key == KEY_LEFT || in->key == KEY_RIGHT) {
                const struct app_menus *am = current_menus(); int n = am ? am->count : 0;
                int cur = open_menu == -2 ? -1 : open_menu;
                int nxt = in->key == KEY_RIGHT ? cur + 1 : cur - 1;
                if (nxt < -1) nxt = n - 1; if (nxt >= n) nxt = -1;
                open_menu_idx(nxt == -1 ? -2 : nxt);
                return;
            }
            return;
        }
        bool ctrl_alias = !focused || !(focused->flags & WIN_NO_CTRL_ALIAS);
        bool cmd = (in->mods & MOD_CMD) || ((in->mods & MOD_CTRL) && ctrl_alias);
        int k = in->key; if (k >= 'A' && k <= 'Z') k += 32;
        if (cmd && !(in->mods & MOD_SHIFT) && k == 'q' && focused && !(focused->flags & WIN_UTILITY)) { struct event e = { .type = EV_MENU, .cmd = CMD_QUIT }; window_post_event(focused, &e); return; }
        if (cmd && k == 'w' && focused) { struct event e = { .type = EV_CLOSE }; window_post_event(focused, &e); return; }
        if (cmd && k == 'm' && focused && (focused->flags & WIN_MINIMIZABLE)) { struct window *f = focused; mutex_unlock(&wm_lock); window_minimize(f); mutex_lock(&wm_lock); return; }
        if (cmd && k == KEY_TAB) { cycle_windows(); return; }
        if (cmd && k == ',') { wm_system_command(CMD_SETTINGS); return; }
        if ((in->mods & MOD_CTRL) && (in->mods & MOD_ALT) && k == KEY_DELETE) { wm_system_command(CMD_TASKMAN); return; }
        int mc;
        if (cmd && focused && menu_shortcut(focused->menus, in->key, in->mods, &mc, ctrl_alias)) { handle_menu_command(mc); return; }
    }
    if (focused) {
        struct event e = { 0 };
        e.type = in->type == INPUT_KEY_DOWN ? EV_KEY_DOWN : EV_KEY_UP;
        e.key = in->key; e.ch = in->ch; e.mods = in->mods;
        window_post_event(focused, &e);
    }
}

/* ---------------------------------------------------------------- main loop */
void wm_init(void) {
    mutex_init(&wm_lock);
    sem_init(&wake_sem, 0);
    scr_w = fb_width(); scr_h = fb_height();
    mouse_x = scr_w / 2; mouse_y = scr_h / 2;
    theme_apply(false, 0);
    wallpaper_build();
    cursor_init();
    logo_icon = icon_get("logo", 64);
    layout_dock();
    wm_notify_app_state();
    wm_dirty_all();
}

void wm_run(void) {
    struct input_event in;
    uint64_t last_clock_check = 0;
    for (;;) {
        bool got = input_pop(&in, 16);
        if (got) {
            mutex_lock(&wm_lock);
            switch (in.type) {
            case INPUT_MOUSE_MOVE: on_mouse_move(&in); break;
            case INPUT_MOUSE_BUTTON: on_mouse_button(&in); break;
            case INPUT_MOUSE_SCROLL: on_scroll(&in); break;
            case INPUT_KEY_DOWN: case INPUT_KEY_UP: on_key(&in); break;
            }
            mutex_unlock(&wm_lock);
            /* drain further pending input before composing */
            while (input_pop(&in, 0)) {
                mutex_lock(&wm_lock);
                switch (in.type) {
                case INPUT_MOUSE_MOVE: on_mouse_move(&in); break;
                case INPUT_MOUSE_BUTTON: on_mouse_button(&in); break;
                case INPUT_MOUSE_SCROLL: on_scroll(&in); break;
                case INPUT_KEY_DOWN: case INPUT_KEY_UP: on_key(&in); break;
                }
                mutex_unlock(&wm_lock);
            }
        }
        if (system_ticks - last_clock_check > 1000) {
            last_clock_check = system_ticks;
            struct rtc_time t; unix_to_datetime(time_unix() + tz_offset_minutes * 60, &t);
            if (t.minute != last_minute) { last_minute = t.minute; damage_add(rect_make(scr_w - 240, 0, 240, MENUBAR_H)); }
        }
        flush_damage();
        while (sem_trywait(&wake_sem)) {}
    }
}
