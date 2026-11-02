#include <influx.h>
#include <unistd.h>
#include <string.h>

long syscall0(long n) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory"); return r; }
long syscall1(long n, long a) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory"); return r; }
long syscall2(long n, long a, long b) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory"); return r; }
long syscall3(long n, long a, long b, long c) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
long syscall4(long n, long a, long b, long c, long d) { long r; register long r10 __asm__("r10") = d; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory"); return r; }

int errno;
static long ret(long r) { if (r < 0) { errno = (int)-r; return -1; } return r; }

ssize_t write(int fd, const void *buf, size_t n) { return ret(syscall3(SYS_WRITE, fd, (long)buf, n)); }
ssize_t read(int fd, void *buf, size_t n) { return ret(syscall3(SYS_READ, fd, (long)buf, n)); }
int open(const char *path, int flags, ...) { return (int)ret(syscall2(SYS_OPEN, (long)path, flags)); }
int close(int fd) { return (int)ret(syscall1(SYS_CLOSE, fd)); }
long lseek(int fd, long off, int whence) { return ret(syscall3(SYS_LSEEK, fd, off, whence)); }
int getpid(void) { return (int)syscall0(SYS_GETPID); }
void *sbrk(long incr) { long r = syscall1(SYS_SBRK, incr); return r < 0 ? (void *)-1 : (void *)r; }
int sleep_ms(unsigned ms) { return (int)syscall1(SYS_SLEEP, ms); }
unsigned sleep(unsigned s) { syscall1(SYS_SLEEP, (long)s * 1000); return 0; }
int sched_yield(void) { return (int)syscall0(SYS_YIELD); }
char *getcwd(char *buf, size_t n) { return syscall2(SYS_GETCWD, (long)buf, n) < 0 ? NULL : buf; }
int chdir(const char *path) { return (int)ret(syscall1(SYS_CHDIR, (long)path)); }
void _exit(int code) { for (;;) syscall1(SYS_EXIT, code); }
int readdir(int fd, int index, struct dirent *d) { return (int)syscall3(SYS_READDIR, fd, index, (long)d); }
int stat(const char *path, struct stat *st) { return (int)ret(syscall2(SYS_STAT, (long)path, (long)st)); }
int fstat(int fd, struct stat *st) { return (int)ret(syscall2(SYS_FSTAT, fd, (long)st)); }
int mkdir(const char *path) { return (int)ret(syscall1(SYS_MKDIR, (long)path)); }
int unlink(const char *path) { return (int)ret(syscall1(SYS_UNLINK, (long)path)); }
int rename(const char *a, const char *b) { return (int)ret(syscall2(SYS_RENAME, (long)a, (long)b)); }
int spawn(const char *path, char *const argv[]) { return (int)ret(syscall2(SYS_SPAWN, (long)path, (long)argv)); }
int waitpid(int pid, int *status) { return (int)ret(syscall2(SYS_WAITPID, pid, (long)status)); }
int uname(struct utsname *u) { return (int)ret(syscall1(SYS_UNAME, (long)u)); }
long time(long *t) { long r = syscall0(SYS_TIME); if (t) *t = r; return r; }
long uptime_ms(void) { return syscall0(SYS_UPTIME); }
int sysinfo(struct sysinfo *si) { return (int)ret(syscall1(SYS_SYSINFO, (long)si)); }
int ps(struct procinfo *out, int max) { return (int)ret(syscall2(SYS_PS, (long)out, max)); }
int lsblk(struct blkinfo *out, int max) { return (int)ret(syscall2(SYS_LSBLK, (long)out, max)); }
int kill(int pid, int sig) { return (int)ret(syscall2(SYS_KILL, pid, sig)); }
int sync(void) { return (int)syscall0(SYS_SYNC); }
int pipe(int fds[2]) { return (int)ret(syscall1(SYS_PIPE, (long)fds)); }
int dup2(int o, int n) { return (int)ret(syscall2(SYS_DUP2, o, n)); }
int ioctl(int fd, int cmd, void *arg) { return (int)ret(syscall3(SYS_IOCTL, fd, cmd, (long)arg)); }
int isatty(int fd) { int wh[2]; return syscall3(SYS_IOCTL, fd, 2, (long)wh) == 0; }
