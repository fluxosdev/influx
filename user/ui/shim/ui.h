/* user-space toolkit (user/ui): the kernel's window API, backed by the window system calls */
#pragma once
#include "ui_common.h"
#include "sched.h"

struct window {
    int id;                        /* the process's window slot */
    char title[128];
    struct rect frame;             /* as the kernel reports it: size of the drawing canvas, title bar included for WIN_FULL_TITLE */
    struct canvas *canvas;
    int flags;
    int min_w, min_h;
    void *user;
    struct rect dirty; bool any_dirty;
};

struct window *window_create(const char *title, int w, int h, int flags);
struct window *window_create_try(const char *title, int w, int h, int flags);
void window_destroy(struct window *w);
void window_show(struct window *w);
struct canvas *window_begin_draw(struct window *w);
void window_end_draw(struct window *w, const struct rect *dirty);
void window_invalidate(struct window *w);
void window_set_title(struct window *w, const char *title);
void window_set_menus(struct window *w, const struct app_menus *m);
void window_set_timer(struct window *w, int ms);
void window_set_min_size(struct window *w, int mw, int mh);
void window_focus(struct window *w);
void window_resize(struct window *w, int cw, int ch);
bool window_wait_event(struct window *w, struct event *ev, int timeout_ms);   /* -1 = forever; EV_OPEN paths are malloc'd */
bool window_post_event(struct window *w, const struct event *ev);   /* from any thread: EV_USER, EV_MENU or EV_TIMER */
int window_content_top(struct window *w);

int app_launch(int id, const char *arg);
void app_open_path(const char *path);
void ui_init(void);                /* fonts and theme; call first */
extern bool clock_24h;
