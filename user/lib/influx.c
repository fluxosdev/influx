/* libinflux: wrappers for the inFlux-specific system calls (see influx.h). */
#include <influx.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static long ret(long r) { if (r < 0 && r > -4096) { errno = (int)-r; return -1; } return r; }
static long sc(long n, long a, long b, long c, long d) {
    long r; register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

/* the buffer size the app presents: the creation size until it sees a resize event */
static int win_wh[8][2];
int win_create(int w, int h, const char *title) { int r = (int)ret(sc(SYS_WIN_CREATE, w, h, (long)title, 0)); if (r >= 0 && r < 8) { win_wh[r][0] = w; win_wh[r][1] = h; } return r; }
int win_present(int id, const uint32_t *px) { int ok = id >= 0 && id < 8; return (int)ret(sc(SYS_WIN_PRESENT, id, (long)px, ok ? win_wh[id][0] : 0, ok ? win_wh[id][1] : 0)); }
int win_present_size(int id, const uint32_t *px, int w, int h) { if (w <= 0 || h <= 0) { errno = EINVAL; return -1; } return (int)ret(sc(SYS_WIN_PRESENT, id, (long)px, w, h)); }
int win_event(int id, struct win_event *ev, int timeout_ms) {
    int r = (int)ret(sc(SYS_WIN_EVENT, id, (long)ev, timeout_ms, 0));
    if (r == 1 && ev->type == WEV_RESIZE && id >= 0 && id < 8 && ev->w > 0 && ev->h > 0) { win_wh[id][0] = ev->w; win_wh[id][1] = ev->h; }
    return r;
}
int win_destroy(int id) { return (int)ret(sc(SYS_WIN_DESTROY, id, 0, 0, 0)); }
int win_set_title(int id, const char *t) { return (int)ret(sc(SYS_WIN_TITLE, id, (long)t, 0, 0)); }

int influx_sysinfo(struct influx_sysinfo *si) { return (int)ret(sc(SYS_INFLUX_SYSINFO, (long)si, 0, 0, 0)); }
int influx_ps(struct procinfo *out, int max) { return (int)ret(sc(SYS_INFLUX_PS, (long)out, max, 0, 0)); }
int influx_lsblk(struct blkinfo *out, int max) { return (int)ret(sc(SYS_INFLUX_LSBLK, (long)out, max, 0, 0)); }
int influx_spawn(const char *path, char *const argv[], char *const envp[]) { return (int)ret(sc(SYS_INFLUX_SPAWN, (long)path, (long)argv, (long)envp, 0)); }
int influx_notify(const char *title, const char *body) { return (int)ret(sc(SYS_INFLUX_NOTIFY, (long)title, (long)body, 0, 0)); }
long uptime_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
