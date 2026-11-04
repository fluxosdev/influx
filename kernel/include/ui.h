#pragma once
#include "ui_common.h"
#include "sched.h"

struct window {
    int id;
    char title[128];
    struct rect frame;             /* content rect in screen coordinates (excludes the title bar) */
    struct canvas *canvas;
    struct mutex lock;
    int flags;
    struct process *owner;
    int app_id;
    const struct app_menus *menus;
    struct event evq[EVQ_SIZE];
    int evq_head, evq_tail;
    spinlock_t evq_lock;
    struct semaphore evq_sem;
    struct list_head link;
    int min_w, min_h;
    void *user;
    int timer_ms; uint64_t next_timer;
    struct rect saved_frame;       /* for zoom */
    bool zoomed;
    int light_hover;               /* traffic light under the cursor (-1 none) */
    struct canvas *icon;
    void *owned_menus;             /* a user process's menus, copied in by the kernel; freed with the window */
};

struct window *window_create(const char *title, int w, int h, int flags);
struct window *window_create_try(const char *title, int w, int h, int flags);   /* NULL when the canvas can't be allocated */
void window_destroy(struct window *w);
void window_show(struct window *w);
struct canvas *window_begin_draw(struct window *w);
void window_end_draw(struct window *w, const struct rect *dirty);
void window_invalidate(struct window *w);
void window_set_title(struct window *w, const char *title);
void window_set_menus(struct window *w, const struct app_menus *m);
void window_set_timer(struct window *w, int ms);
void window_set_min_size(struct window *w, int mw, int mh);
void window_move(struct window *w, int x, int y);
void window_resize(struct window *w, int cw, int ch);
void window_focus(struct window *w);
void window_minimize(struct window *w);
bool window_wait_event(struct window *w, struct event *ev, int timeout_ms);   /* -1 = forever */
bool window_post_event(struct window *w, const struct event *ev);   /* false: queue full, event dropped */
int window_content_top(struct window *w);   /* TITLEBAR_H for WIN_FULL_TITLE windows */
struct window *window_find(int id);
int window_list(struct window **out, int max);
struct window *window_top_of_process(struct process *p);
bool window_focus_process(struct process *p);                      /* focus its top window; false if none */
bool window_post_process(struct process *p, const struct event *ev);   /* post to its top window; false if none or its queue is full */

void prefs_load(void);   /* ~/.config/settings.conf */
void prefs_save(void);
void wm_theme_changed(void);

/* window manager */
void wm_init(void);
void wm_run(void) NORETURN;
void wm_dirty_all(void);
int wm_screen_w(void);
int wm_screen_h(void);
struct rect wm_desktop_rect(void);   /* area between menu bar and dock */
void wm_system_command(int cmd);
bool wm_set_resolution(int w, int h);
int display_persist_resolution(int w, int h);   /* writes resolution: into the installed limine.conf; <0 if none */
void wm_notify_app_state(void);      /* dock running indicators changed */
void wm_broadcast(int type);         /* post an event to every window */
extern bool clock_24h;

extern const struct app_info app_table[APP_COUNT];
int app_launch(int id, const char *arg);
bool app_is_running(int id);
void app_activate(int id);
void app_open_path(const char *path);        /* open a file with the right app */
void about_show(void);
void power_restart(void) NORETURN;
void power_shutdown(void) NORETURN;

