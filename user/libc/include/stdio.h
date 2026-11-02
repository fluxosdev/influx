#pragma once
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
#define stdin 0
#define stdout 1
#define stderr 2
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int getchar(void);
char *fgets(char *buf, int n, int fd);
void perror(const char *s);
