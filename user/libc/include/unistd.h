#pragma once
#include <stddef.h>
#include <stdint.h>
typedef long ssize_t;
ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
long lseek(int fd, long off, int whence);
int getpid(void);
void *sbrk(long incr);
int sleep_ms(unsigned ms);
unsigned sleep(unsigned secs);
int sched_yield(void);
char *getcwd(char *buf, size_t n);
int chdir(const char *path);
void _exit(int code) __attribute__((noreturn));
int isatty(int fd);
