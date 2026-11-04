/*
 * libui: the desktop toolkit for user programs. The kernel's drawing,
 * fonts, images, widgets and theme code (kernel/src/gfx, ui/widgets.c,
 * ui/theme.c) is compiled into this library unchanged; this file supplies
 * what those sources expect from the kernel (heap and threads come from the
 * shim headers) and the window API on top of the window system calls:
 * a window draws into its own canvas here, and window_end_draw sends the
 * changed rectangle to the window server.
 */
#include "ui.h"
#include "mm.h"
#include "simd.h"
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct thread ui_current_thread;
struct cpu_features cpu_feat;
int64_t tz_offset_minutes;
bool clock_24h;
char system_hostname[64] = "influx";

/* ---- raw system calls ---- */
static long sc6(long n, long a, long b, long c, long d, long e) {
    long r; register long r10 __asm__("r10") = d; register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return r;
}
enum {
    SYS_WIN_CREATE = 0x1010, SYS_WIN_PRESENT, SYS_WIN_EVENT, SYS_WIN_DESTROY, SYS_WIN_TITLE, SYS_WIN_SHOW, SYS_WIN_GEOMETRY,
    SYS_WIN_MENUS, SYS_WIN_TIMER, SYS_WIN_MIN_SIZE, SYS_WIN_POST, SYS_WIN_FOCUS, SYS_WIN_RESIZE,
    SYS_UI_THEME = 0x1020, SYS_UI_DIALOG, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET, SYS_APP_LAUNCH, SYS_APP_OPEN,
};
struct user_event { int32_t type, x, y, button, buttons, scroll, key; uint32_t ch; int32_t mods, cmd, w, h; };
struct user_geom { int32_t x, y, w, h, top, flags; };
struct user_theme { int32_t dark, accent, clock_24h, pad; int64_t tz_offset_minutes; };
struct user_dialog { uint64_t title, message, extra, out; uint32_t outsz; int32_t destructive; };

/* ---- what the kernel sources call ---- */
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
void kvprintf(const char *fmt, va_list ap) { vfprintf(stderr, fmt, ap); }
void panic(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fputs("panic: ", stderr); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); abort(); }
uint64_t time_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
uint64_t time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t time_unix(void) { return (uint64_t)time(NULL); }

struct thread_start { void (*entry)(void *); void *arg; };
static void *thread_tramp(void *p) { struct thread_start s = *(struct thread_start *)p; free(p); s.entry(s.arg); return NULL; }
struct thread *thread_create(struct process *proc, const char *name, void (*entry)(void *), void *arg) {
    struct thread_start *s = malloc(sizeof *s);
    if (!s) return NULL;
    s->entry = entry; s->arg = arg;
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 * 1024);
    int e = pthread_create(&t, &a, thread_tramp, s);
    pthread_attr_destroy(&a);
    if (e) { free(s); return NULL; }
    return &ui_current_thread;
}

/* files, as the kernel's VFS helpers behave */
int vfs_read_file(const char *path, void **data, size_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0) { int e = -errno; close(fd); return e; }
    if (S_ISDIR(st.st_mode)) { close(fd); return -EISDIR; }
    size_t cap = st.st_size > 0 ? (size_t)st.st_size : 4096, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return -ENOMEM; }
    for (;;) {
        if (n == cap) { char *nb = realloc(buf, cap * 2 + 1); if (!nb) { free(buf); close(fd); return -ENOMEM; } buf = nb; cap *= 2; }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; int e = -errno; free(buf); close(fd); return e; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    *data = buf; *size = n;
    return 0;
}
int vfs_write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -errno;
    const char *p = data;
    while (size) {
        ssize_t r = write(fd, p, size);
        if (r < 0) { if (errno == EINTR) continue; int e = -errno; close(fd); return e; }
        p += r; size -= (size_t)r;
    }
    return close(fd) < 0 ? -errno : 0;
}
int vfs_mkdir(const char *path) { return mkdir(path, 0755) < 0 ? -errno : 0; }
/* lexical normalization: '.' and '..' resolved, slashes collapsed */
int vfs_normalize(const char *cwd, const char *path, char *out, size_t n) {
    char tmp[PATH_MAX * 2];
    if (!path || !*path) { if (n) out[0] = 0; return -ENOENT; }
    if (path[0] == '/') snprintf(tmp, sizeof tmp, "%s", path);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd && *cwd ? cwd : "/", path);
    char *parts[256]; int np = 0;
    char *save, *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (!strcmp(tok, "..")) { if (np) np--; }
        else if (strcmp(tok, ".") && np < 256) parts[np++] = tok;
        tok = strtok_r(NULL, "/", &save);
    }
    size_t o = 0;
    if (!np) { if (n < 2) { if (n) out[0] = 0; return -ENAMETOOLONG; } strcpy(out, "/"); return 0; }
    for (int i = 0; i < np; i++) {
        size_t l = strlen(parts[i]);
        if (o + 1 + l + 1 > n) { if (n) out[0] = 0; return -ENAMETOOLONG; }
        out[o++] = '/'; memcpy(out + o, parts[i], l); o += l;
    }
    out[o] = 0;
    return 0;
}
const char *vfs_basename(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }

/* ---- theme ---- */
static void fetch_theme(void) {
    struct user_theme t;
    if (sc6(SYS_UI_THEME, (long)&t, 0, 0, 0, 0) < 0) return;
    theme_apply(t.dark, t.accent);
    clock_24h = t.clock_24h; tz_offset_minutes = t.tz_offset_minutes;
}

void ui_init(void) {
    __builtin_cpu_init();
    cpu_feat.avx = __builtin_cpu_supports("avx");
    cpu_feat.avx2 = __builtin_cpu_supports("avx2");
    cpu_feat.fma = __builtin_cpu_supports("fma");
    simd_init();
    gethostname(system_hostname, sizeof system_hostname);
    fonts_init();
    theme_apply(false, 0);
    fetch_theme();
}

/* ---- windows ---- */
static bool refresh_geometry(struct window *w) {
    struct user_geom g;
    if (sc6(SYS_WIN_GEOMETRY, w->id, (long)&g, 0, 0, 0) < 0) return false;
    if (w->canvas && (w->canvas->w != g.w || w->canvas->h != g.h)) { canvas_destroy(w->canvas); w->canvas = NULL; }
    w->frame = rect_make(g.x, g.y, g.w, g.h);
    if (!w->canvas) {
        w->canvas = canvas_create_try(g.w, g.h);
        if (!w->canvas) return false;
        gfx_fill(w->canvas, theme.bg);
    }
    return true;
}

static struct window *create(const char *title, int cw, int ch, int flags, bool may_fail) {
    struct window *w = calloc(1, sizeof *w);
    if (!w) { if (may_fail) return NULL; panic("window_create: out of memory"); }
    long id = sc6(SYS_WIN_CREATE, cw, ch, (long)title, (long)((unsigned)flags | (1u << 30)), 0);
    if (id < 0) { free(w); if (may_fail) return NULL; panic("window_create: %ld", id); }
    w->id = (int)id;
    w->flags = flags;
    w->min_w = 200; w->min_h = 100;
    snprintf(w->title, sizeof w->title, "%s", title);
    if (!refresh_geometry(w)) { sc6(SYS_WIN_DESTROY, w->id, 0, 0, 0, 0); free(w); if (may_fail) return NULL; panic("window_create: no memory for the canvas"); }
    return w;
}
struct window *window_create(const char *title, int cw, int ch, int flags) { return create(title, cw, ch, flags, false); }
struct window *window_create_try(const char *title, int cw, int ch, int flags) { return create(title, cw, ch, flags, true); }
void window_destroy(struct window *w) { if (!w) return; sc6(SYS_WIN_DESTROY, w->id, 0, 0, 0, 0); canvas_destroy(w->canvas); free(w); }
void window_show(struct window *w) { window_end_draw(w, NULL); sc6(SYS_WIN_SHOW, w->id, 0, 0, 0, 0); }
struct canvas *window_begin_draw(struct window *w) { return w->canvas; }
void window_end_draw(struct window *w, const struct rect *dirty) {
    struct canvas *c = w->canvas;
    if (!c) return;
    struct rect r = dirty ? rect_intersect(*dirty, rect_make(0, 0, c->w, c->h)) : rect_make(0, 0, c->w, c->h);
    if (rect_empty(r)) return;
    sc6(SYS_WIN_PRESENT, w->id, (long)c->px, c->stride, c->h, (long)&r);
}
void window_invalidate(struct window *w) { window_end_draw(w, NULL); }
void window_set_title(struct window *w, const char *t) { snprintf(w->title, sizeof w->title, "%s", t); sc6(SYS_WIN_TITLE, w->id, (long)t, 0, 0, 0); }
void window_set_menus(struct window *w, const struct app_menus *m) { sc6(SYS_WIN_MENUS, w->id, m ? (long)m->menus : 0, m ? m->count : 0, 0, 0); }
void window_set_timer(struct window *w, int ms) { sc6(SYS_WIN_TIMER, w->id, ms, 0, 0, 0); }
void window_set_min_size(struct window *w, int mw, int mh) { w->min_w = mw; w->min_h = mh; sc6(SYS_WIN_MIN_SIZE, w->id, mw, mh, 0, 0); }
void window_focus(struct window *w) { sc6(SYS_WIN_FOCUS, w->id, 0, 0, 0, 0); }
void window_resize(struct window *w, int cw, int ch) { sc6(SYS_WIN_RESIZE, w->id, cw, ch, 0, 0); refresh_geometry(w); }
int window_content_top(struct window *w) { return (w->flags & WIN_FULL_TITLE) ? TITLEBAR_H : 0; }
bool window_post_event(struct window *w, const struct event *ev) { return sc6(SYS_WIN_POST, w->id, ev->type, ev->cmd, 0, 0) == 0; }

bool window_wait_event(struct window *w, struct event *ev, int timeout_ms) {
    struct user_event ue;
    char path[PATH_MAX];
    for (;;) {
        long r = sc6(SYS_WIN_EVENT, w->id, (long)&ue, timeout_ms, (long)path, sizeof path);
        if (r == -EINTR) continue;
        if (r <= 0) return false;
        memset(ev, 0, sizeof *ev);
        ev->type = ue.type; ev->x = ue.x; ev->y = ue.y; ev->button = ue.button; ev->buttons = ue.buttons; ev->scroll = ue.scroll;
        ev->key = ue.key; ev->ch = ue.ch; ev->mods = ue.mods; ev->cmd = ue.cmd; ev->w = ue.w; ev->h = ue.h;
        if (ev->type == EV_OPEN) ev->path = strdup(path);
        if (ev->type == EV_RESIZE) refresh_geometry(w);
        if (ev->type == EV_THEME) fetch_theme();
        return true;
    }
}

/* ---- desktop services ---- */
void clipboard_set(const char *text, size_t n) { sc6(SYS_CLIPBOARD_SET, (long)text, (long)n, 0, 0, 0); }
char *clipboard_get(void) {
    long n = sc6(SYS_CLIPBOARD_GET, 0, 0, 0, 0, 0);
    if (n <= 0) return NULL;
    char *b = malloc((size_t)n + 1);
    if (!b) return NULL;
    sc6(SYS_CLIPBOARD_GET, (long)b, n + 1, 0, 0, 0);
    return b;
}
void dialog_message(const char *title, const char *message) {
    struct user_dialog d = { (uint64_t)title, (uint64_t)message, 0, 0, 0, 0 };
    sc6(SYS_UI_DIALOG, 0, (long)&d, 0, 0, 0);
}
bool dialog_confirm(const char *title, const char *message, const char *ok_label, bool destructive) {
    struct user_dialog d = { (uint64_t)title, (uint64_t)message, (uint64_t)ok_label, 0, 0, destructive };
    return sc6(SYS_UI_DIALOG, 1, (long)&d, 0, 0, 0) > 0;
}
bool dialog_prompt(const char *title, const char *message, const char *initial, char *out, size_t outsz) {
    struct user_dialog d = { (uint64_t)title, (uint64_t)message, (uint64_t)initial, (uint64_t)out, (uint32_t)outsz, 0 };
    return sc6(SYS_UI_DIALOG, 2, (long)&d, 0, 0, 0) > 0;
}
void notify_post(const char *title, const char *body) { sc6(0x1026, (long)title, (long)body, 0, 0, 0); }
int app_launch(int id, const char *arg) { return (int)sc6(SYS_APP_LAUNCH, id, (long)arg, 0, 0, 0); }
void app_open_path(const char *path) { sc6(SYS_APP_OPEN, (long)path, 0, 0, 0, 0); }

/* icons from /sys/icons, scaled and cached like the kernel does */
static struct { char name[48]; int size; struct canvas *img; } icons[64];
static int nicons;
static pthread_mutex_t icon_lock = PTHREAD_MUTEX_INITIALIZER;
struct canvas *icon_get(const char *name, int size) {
    pthread_mutex_lock(&icon_lock);
    for (int i = 0; i < nicons; i++) if (icons[i].size == size && !strcmp(icons[i].name, name)) { struct canvas *c = icons[i].img; pthread_mutex_unlock(&icon_lock); return c; }
    int src = size <= 32 ? 32 : size <= 64 ? 64 : size <= 128 ? 128 : 256;
    char path[128]; snprintf(path, sizeof path, "/sys/icons/%s-%d.img", name, src);
    struct canvas *img = image_load(path);
    if (!img && src != 128) { snprintf(path, sizeof path, "/sys/icons/%s-128.img", name); img = image_load(path); }
    if (img && img->w != size) {
        struct canvas *s = canvas_create(size, size);
        gfx_blit_scaled(s, 0, 0, size, size, img);
        canvas_destroy(img);
        img = s;
    }
    if (nicons < 64) { snprintf(icons[nicons].name, sizeof icons[nicons].name, "%s", name); icons[nicons].size = size; icons[nicons].img = img; nicons++; }
    pthread_mutex_unlock(&icon_lock);
    return img;
}
