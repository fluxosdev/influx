/* inFlux system call interface for user programs */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum {
    SYS_EXIT = 0, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_LSEEK, SYS_GETPID, SYS_SBRK, SYS_SLEEP, SYS_YIELD,
    SYS_GETCWD, SYS_CHDIR, SYS_READDIR, SYS_STAT, SYS_MKDIR, SYS_UNLINK, SYS_SPAWN, SYS_WAITPID, SYS_UNAME, SYS_TIME,
    SYS_UPTIME, SYS_IOCTL, SYS_DUP2, SYS_PIPE, SYS_KILL, SYS_SYSINFO, SYS_PS, SYS_FSTAT, SYS_RENAME, SYS_SYNC, SYS_LSBLK,
    SYS_WIN_CREATE, SYS_WIN_PRESENT, SYS_WIN_EVENT, SYS_WIN_DESTROY, SYS_WIN_TITLE,
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
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400
#define O_DIRECTORY 0x10000
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define VN_FILE 1
#define VN_DIR 2
#define VN_DEV 3
#define VN_BLK 4

struct dirent { char name[256]; uint32_t type; uint64_t size, mtime; };
struct stat { uint32_t type, mode; uint64_t size, mtime, ino; };
struct utsname { char sysname[32], nodename[64], release[32], version[64], machine[32]; };
struct sysinfo { uint64_t uptime_ms, total_mem, free_mem, used_mem; uint32_t procs, threads; };
struct procinfo { int32_t pid; char name[32]; int32_t cpu; uint64_t mem; int32_t threads; int32_t user; };
struct blkinfo { char name[16], model[48], fs[16]; uint64_t bytes; int32_t partition; };

long syscall0(long n);
long syscall1(long n, long a);
long syscall2(long n, long a, long b);
long syscall3(long n, long a, long b, long c);
long syscall4(long n, long a, long b, long c, long d);

int readdir(int fd, int index, struct dirent *d);      /* 1 = got entry, 0 = end */
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path);
int unlink(const char *path);
int rename(const char *a, const char *b);
int spawn(const char *path, char *const argv[]);
int waitpid(int pid, int *status);
int uname(struct utsname *u);
long time(long *t);
long uptime_ms(void);
int sysinfo(struct sysinfo *si);
int ps(struct procinfo *out, int max);
int lsblk(struct blkinfo *out, int max);
int kill(int pid, int sig);
int sync(void);
int pipe(int fds[2]);
int dup2(int oldfd, int newfd);
int ioctl(int fd, int cmd, void *arg);
