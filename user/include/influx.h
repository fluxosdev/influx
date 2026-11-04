/* inFlux-specific system calls for user programs (everything POSIX comes from musl). Link with -linflux. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum {
    SYS_INFLUX_SPAWN = 0x1000, SYS_INFLUX_PS, SYS_INFLUX_LSBLK, SYS_INFLUX_SYSINFO,
    SYS_INFLUX_NOTIFY = 0x1026,
    SYS_WIN_CREATE = 0x1010, SYS_WIN_PRESENT, SYS_WIN_EVENT, SYS_WIN_DESTROY, SYS_WIN_TITLE,
};

/* ---- desktop windows for user programs ---- */
enum { WEV_NONE = 0, WEV_MOUSE_MOVE, WEV_MOUSE_DOWN, WEV_MOUSE_UP, WEV_MOUSE_SCROLL, WEV_MOUSE_LEAVE, WEV_KEY_DOWN, WEV_KEY_UP,
       WEV_CLOSE, WEV_RESIZE, WEV_FOCUS, WEV_BLUR, WEV_MENU, WEV_TIMER, WEV_QUIT };
#define WCMD_QUIT 1
#define WKEY_ESC 27
#define WKEY_SPACE 32
#define WBTN_LEFT 1
struct win_event { int32_t type, x, y, button, buttons, scroll, key; uint32_t ch; int32_t mods, cmd, w, h; };
int win_create(int w, int h, const char *title);                 /* returns a window id */
int win_present(int id, const uint32_t *argb_pixels);            /* w*h pixels, 0xAARRGGBB */
int win_present_size(int id, const uint32_t *argb_pixels, int w, int h);   /* a w*h buffer (w, h <= 4096); the window shows its top-left part */
int win_event(int id, struct win_event *ev, int timeout_ms);     /* 1 = event, 0 = timeout */
int win_destroy(int id);
int win_set_title(int id, const char *title);

/* ---- system information ---- */
struct influx_sysinfo { uint64_t uptime_ms, total_mem, free_mem, used_mem; uint32_t procs, threads; };
struct procinfo { int32_t pid; char name[32]; int32_t cpu; uint64_t mem; int32_t threads; int32_t user; };
struct blkinfo { char name[16], model[48], fs[16]; uint64_t bytes; int32_t partition; };
int influx_sysinfo(struct influx_sysinfo *si);
int influx_ps(struct procinfo *out, int max);
int influx_lsblk(struct blkinfo *out, int max);
/* starts a program as a child (wait for it with waitpid); envp NULL: the system default */
int influx_spawn(const char *path, char *const argv[], char *const envp[]);
long uptime_ms(void);
/* a notification banner on the desktop (with the notification sound) */
int influx_notify(const char *title, const char *body);
