#pragma once
#include "gfx.h"
#include "input.h"
#include "sched.h"

#define MENUBAR_H  26
#define TITLEBAR_H 28
#define WIN_RADIUS 10
#define DOCK_ICON  48
#define DOCK_H     (DOCK_ICON + 16)
#define DOCK_MARGIN 8

enum event_type {
    EV_NONE = 0, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_MOUSE_SCROLL, EV_MOUSE_LEAVE,
    EV_KEY_DOWN, EV_KEY_UP, EV_CLOSE, EV_RESIZE, EV_FOCUS, EV_BLUR, EV_MENU, EV_TIMER, EV_QUIT, EV_OPEN, EV_USER, EV_THEME,
};
struct event {
    int type;
    int x, y;          /* content coordinates */
    int button, buttons;
    int scroll;
    int key; uint32_t ch; int mods;
    int cmd;
    int w, h;
    char *path;        /* EV_OPEN: kmalloc'd path, receiver frees */
};

/* standard menu commands */
enum {
    CMD_NONE = 0, CMD_QUIT, CMD_CLOSE, CMD_ABOUT, CMD_SETTINGS, CMD_TASKMAN, CMD_RESTART, CMD_SHUTDOWN, CMD_INSTALL,
    CMD_NEW = 10, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_NEW_WINDOW, CMD_NEW_FOLDER,
    CMD_UNDO = 20, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELECT_ALL, CMD_DELETE, CMD_FIND,
    CMD_MINIMIZE = 30, CMD_ZOOM, CMD_HELP, CMD_CLEAR,
    CMD_APP = 100,
};

struct menu_item { const char *label; int cmd; int key; int mods; bool separator; bool disabled; bool checked; };
struct menu { const char *title; const struct menu_item *items; int count; };
struct app_menus { const struct menu *menus; int count; };
#define MENU_SEP { NULL, 0, 0, 0, true, false, false }
#define MENU(label, cmd, key, mods) { label, cmd, key, mods, false, false, false }
#define MENU_DISABLED(label, cmd) { label, cmd, 0, 0, false, true, false }

#define WIN_RESIZABLE   1
#define WIN_CLOSABLE    2
#define WIN_MINIMIZABLE 4
#define WIN_STANDARD    7
#define WIN_DIALOG      8     /* close only, centered over the screen */
#define WIN_FULL_TITLE  16    /* the app draws under the title bar (toolbar/sidebar style) */
#define WIN_MINIMIZED   64
#define WIN_VISIBLE     128
#define WIN_UTILITY     256   /* floats above, no app menus */
#define WIN_NO_CTRL_ALIAS 512 /* Ctrl is not treated as Cmd (terminals) */

#define EVQ_SIZE 128

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
};

struct window *window_create(const char *title, int w, int h, int flags);
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
void window_post_event(struct window *w, const struct event *ev);
int window_content_top(struct window *w);   /* TITLEBAR_H for WIN_FULL_TITLE windows */
struct window *window_find(int id);
int window_list(struct window **out, int max);
struct window *window_top_of_process(struct process *p);

void clipboard_set(const char *text, size_t n);
char *clipboard_get(void);

/* theme */
struct theme {
    bool dark;
    int accent_index;
    color_t accent, accent_text;
    color_t bg, bg_secondary, sidebar_bg;
    color_t text, text_secondary, text_tertiary, text_disabled;
    color_t separator, border;
    color_t control_bg, control_border, control_hover;
    color_t titlebar, titlebar_unfocused, titlebar_line, title_text, title_text_unfocused;
    color_t menubar_bg, menubar_text;
    color_t menu_bg, menu_text, menu_sep;
    color_t dock_bg, dock_border;
    color_t selection, selection_inactive;
    color_t input_bg, input_border;
    color_t wallpaper_top, wallpaper_bottom;
};
extern struct theme theme;
extern const color_t accent_colors[8];
extern const char *accent_names[8];
void theme_apply(bool dark, int accent_index);
void theme_set_wallpaper(color_t top, color_t bottom);
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

/* apps */
enum { APP_FILES, APP_TERMINAL, APP_EDITOR, APP_CALC, APP_TASKMAN, APP_SETTINGS, APP_GLDEMO, APP_INSTALLER, APP_BROWSER, APP_PLAYER, APP_COUNT };
struct app_info { const char *name; const char *icon; void (*main)(void *arg); bool single_instance; };
extern const struct app_info app_table[APP_COUNT];
int app_launch(int id, const char *arg);
bool app_is_running(int id);
void app_activate(int id);
void app_open_path(const char *path);        /* open a file with the right app */
struct canvas *icon_get(const char *name, int size);
const char *icon_for_file(const char *name, int type);
void about_show(void);
void dialog_message(const char *title, const char *message);
bool dialog_confirm(const char *title, const char *message, const char *ok_label, bool destructive);
bool dialog_prompt(const char *title, const char *message, const char *initial, char *out, size_t outsz);
void power_restart(void) NORETURN;
void power_shutdown(void) NORETURN;

/* widgets */
#define BTN_NORMAL 0
#define BTN_DEFAULT 1
#define BTN_DESTRUCTIVE 2
void ui_button(struct canvas *c, struct rect r, const char *label, int style, bool pressed, bool enabled);
void ui_checkbox(struct canvas *c, int x, int y, bool checked, const char *label);
void ui_switch(struct canvas *c, int x, int y, bool on);
void ui_scrollbar_v(struct canvas *c, struct rect track, int content, int view, int pos);
void ui_panel(struct canvas *c, struct rect r, color_t bg);
/* dropdown (popup button): the closed control, and its open list */
#define DROPDOWN_ITEM_H 24
void ui_dropdown(struct canvas *c, struct rect r, const char *label, bool open, bool enabled);
void ui_dropdown_list(struct canvas *c, struct rect r, const char *const *items, int n, int first, int hover, int checked);
void ui_separator_h(struct canvas *c, int x, int y, int w);
void ui_icon_label(struct canvas *c, int x, int y, struct canvas *icon, int iconsz, const char *label, color_t col, struct font *f);

struct textfield {
    char buf[1024]; int len; int cursor; int scroll; int sel_anchor; bool focused; const char *placeholder;
    uint64_t blink;
};
void textfield_init(struct textfield *t, const char *initial);
void textfield_set(struct textfield *t, const char *text);
void textfield_draw(struct canvas *c, struct textfield *t, struct rect r);
bool textfield_key(struct textfield *t, const struct event *ev);   /* returns true if consumed */
void textfield_mouse(struct textfield *t, struct rect r, int x, int y);
