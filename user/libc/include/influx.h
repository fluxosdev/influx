/* inFlux system call interface for user programs */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum {
    SYS_EXIT = 0, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_LSEEK, SYS_GETPID, SYS_SBRK, SYS_SLEEP, SYS_YIELD,
    SYS_GETCWD, SYS_CHDIR, SYS_READDIR, SYS_STAT, SYS_MKDIR, SYS_UNLINK, SYS_SPAWN, SYS_WAITPID, SYS_UNAME, SYS_TIME,
    SYS_UPTIME, SYS_IOCTL, SYS_DUP2, SYS_PIPE, SYS_KILL, SYS_SYSINFO, SYS_PS, SYS_FSTAT, SYS_RENAME, SYS_SYNC, SYS_LSBLK,
};
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
