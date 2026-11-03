#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <influx.h>
#include <immintrin.h>

extern int errno;
int main(int argc, char **argv);
int __libc_start(int argc, char **argv) { return main(argc, argv); }
void exit(int code) { _exit(code); }

/* ---- string ---- */
/* Bulk copies and fills use AVX, which inFlux requires (libc is built with -mavx) */
static void copy_avx(char *d, const char *s, size_t n) {
    __m256i tail = _mm256_loadu_si256((const __m256i *)(s + n - 32));
    for (size_t i = 0; i + 32 <= n; i += 32) _mm256_storeu_si256((__m256i *)(d + i), _mm256_loadu_si256((const __m256i *)(s + i)));
    _mm256_storeu_si256((__m256i *)(d + n - 32), tail);
}
static void fill_avx(char *d, int c, size_t n) {
    __m256i v = _mm256_set1_epi8((char)c);
    for (size_t i = 0; i + 32 <= n; i += 32) _mm256_storeu_si256((__m256i *)(d + i), v);
    _mm256_storeu_si256((__m256i *)(d + n - 32), v);
}
void *memcpy(void *d, const void *s, size_t n) {
    if (n >= 32) copy_avx(d, s, n);
    else { char *dd = d; const char *ss = s; while (n--) *dd++ = *ss++; }
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    char *dd = d; const char *ss = s;
    if (dd + n <= ss || ss + n <= dd) return memcpy(d, s, n);
    if (dd < ss) while (n--) *dd++ = *ss++; else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    if (n >= 32) fill_avx(d, c, n);
    else { char *dd = d; while (n--) *dd++ = (char)c; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i]; return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i]; if (!a[i]) return 0; } return 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strrchr(const char *s, int c) { const char *r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) { size_t l = strlen(n); if (!l) return (char *)h; for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h; return NULL; }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }
const char *strerror(int e) {
    switch (e) { case 2: return "No such file or directory"; case 5: return "Input/output error"; case 9: return "Bad file descriptor"; case 12: return "Out of memory"; case 13: return "Permission denied"; case 17: return "File exists"; case 20: return "Not a directory"; case 21: return "Is a directory"; case 22: return "Invalid argument"; case 28: return "No space left on device"; case 30: return "Read-only file system"; case 39: return "Directory not empty"; case 38: return "Function not implemented"; case 8: return "Exec format error"; default: return "Unknown error"; }
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
unsigned long strtoul(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    if (base == 0) { if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; } else if (s[0] == '0') base = 8; else base = 10; }
    else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long v = 0;
    for (;; s++) { int d; if (isdigit(*s)) d = *s - '0'; else if (isalpha(*s)) d = tolower(*s) - 'a' + 10; else break; if (d >= base) break; v = v * base + d; }
    if (end) *end = (char *)s;
    return v;
}
long strtol(const char *s, char **end, int base) { while (isspace(*s)) s++; int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++; long v = (long)strtoul(s, end, base); return neg ? -v : v; }
int abs(int v) { return v < 0 ? -v : v; }

/* ---- malloc: address-ordered first-fit free list on top of sbrk; free blocks split and coalesce ----
 * A block is a 16-byte header then the payload; headers and payload sizes are multiples of 16, so every
 * payload is 16-byte aligned as the SysV ABI requires. Only free blocks are linked. */
struct blk { size_t size; struct blk *next; };
#define HDR sizeof(struct blk)
#define MALLOC_MAX ((size_t)1 << 46)   /* far below anything sbrk could grant; keeps every size sum and long cast exact */
static struct blk *free_list;
static char *heap_end;                 /* the break after our last sbrk: a free block ending here can grow in place */
static void *grow(size_t n) {          /* n new bytes at a 16-byte aligned address; n is a multiple of 16, <= MALLOC_MAX + HDR */
    char *p = sbrk((long)n);
    if (p == (void *)-1) return NULL;
    if (p != heap_end) {               /* first call, or someone else moved the break: align and start a fresh run */
        size_t pad = -(uintptr_t)p & 15;
        if (pad) { if (sbrk((long)pad) == (void *)-1) return NULL; p += pad; }
    }
    heap_end = p + n;
    return p;
}
/* put b back in address order, merging with the free neighbours it touches */
static void release(struct blk *b) {
    struct blk *prev = NULL, *nx = free_list;
    while (nx && nx < b) { prev = nx; nx = nx->next; }
    if (nx && (char *)(b + 1) + b->size == (char *)nx) { b->size += HDR + nx->size; b->next = nx->next; } else b->next = nx;
    if (prev && (char *)(prev + 1) + prev->size == (char *)b) { prev->size += HDR + b->size; prev->next = b->next; }
    else if (prev) prev->next = b; else free_list = b;
}
/* hand out the first n bytes of free block b (linked from *pp); the tail goes back on the list if worth keeping */
static void *take(struct blk **pp, struct blk *b, size_t n) {
    if (b->size >= n + HDR + 16) { struct blk *r = (struct blk *)((char *)(b + 1) + n); r->size = b->size - n - HDR; r->next = b->next; *pp = r; b->size = n; }
    else *pp = b->next;
    b->next = NULL;
    return b + 1;
}
void *malloc(size_t n) {
    if (n > MALLOC_MAX) return NULL;
    n = n ? (n + 15) & ~(size_t)15 : 16;
    struct blk **pp = &free_list, **lastp = NULL;
    for (struct blk *b = free_list; b; pp = &b->next, b = b->next) { if (b->size >= n) return take(pp, b, n); lastp = pp; }
    /* nothing fits: if the highest free block ends at the break, extend it instead of leaving it stranded */
    if (lastp && (char *)(*lastp + 1) + (*lastp)->size == heap_end && sbrk(0) == heap_end) {
        struct blk *t = *lastp;
        if (!grow(n - t->size)) return NULL;
        t->size = n; return take(lastp, t, n);
    }
    struct blk *b = grow(HDR + n);
    if (!b) return NULL;
    b->size = n; b->next = NULL;
    return b + 1;
}
void free(void *p) { if (p) release((struct blk *)p - 1); }
void *calloc(size_t a, size_t n) {
    if (n && a > MALLOC_MAX / n) return NULL;
    void *p = malloc(a * n);
    if (p) memset(p, 0, a * n);
    return p;
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    if (n > MALLOC_MAX) return NULL;
    struct blk *b = (struct blk *)p - 1;
    size_t want = (n + 15) & ~(size_t)15;
    if (b->size >= want) {             /* shrink in place, returning a worthwhile tail */
        if (b->size >= want + HDR + 16) { struct blk *r = (struct blk *)((char *)p + want); r->size = b->size - want - HDR; b->size = want; release(r); }
        return p;
    }
    void *np = malloc(n);
    if (np) { memcpy(np, p, b->size); free(p); }   /* b->size < n: copy everything the old block held */
    return np;
}

/* ---- printf ---- */
struct out { char *buf; size_t n, pos; };
static void put(struct out *o, char c) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; o->pos++; }
static void pad(struct out *o, int count, char c) { while (count-- > 0) put(o, c); }
static void fmt_num(struct out *o, unsigned long v, int neg, int base, int upper, int width, int prec, int left, int zero) {
    char tmp[32]; int len = 0; const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[len++] = '0';
    while (v) { tmp[len++] = digits[v % base]; v /= base; }
    while (len < prec && len < (int)sizeof tmp) tmp[len++] = '0';
    int total = len + (neg ? 1 : 0);
    if (!left && !zero) pad(o, width - total, ' ');
    if (neg) put(o, '-');
    if (!left && zero) pad(o, width - total, '0');
    while (len) put(o, tmp[--len]);
    if (left) pad(o, width - total, ' ');
}
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out o = { buf, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0;
        for (;; fmt++) { if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') {} else break; }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } } else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; } else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        int lng = 0; while (*fmt == 'l' || *fmt == 'z' || *fmt == 'j') { lng = 1; fmt++; } while (*fmt == 'h') fmt++;
        if (!*fmt) break;
        switch (*fmt) {
        case 'd': case 'i': { long v = lng ? va_arg(ap, long) : va_arg(ap, int); fmt_num(&o, v < 0 ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v, v < 0, 10, 0, width, prec, left, zero); break; }
        case 'u': { unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); fmt_num(&o, v, 0, 10, 0, width, prec, left, zero); break; }
        case 'x': case 'X': { unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); fmt_num(&o, v, 0, 16, *fmt == 'X', width, prec, left, zero); break; }
        case 'p': { put(&o, '0'); put(&o, 'x'); fmt_num(&o, (unsigned long)va_arg(ap, void *), 0, 16, 0, 0, 1, 0, 0); break; }
        case 'c': { char c = (char)va_arg(ap, int); if (!left) pad(&o, width - 1, ' '); put(&o, c); if (left) pad(&o, width - 1, ' '); break; }
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; int len = 0; while (s[len] && (prec < 0 || len < prec)) len++; if (!left) pad(&o, width - len, ' '); for (int i = 0; i < len; i++) put(&o, s[i]); if (left) pad(&o, width - len, ' '); break; }
        case '%': put(&o, '%'); break;
        /* unsupported conversions still take their argument, so the ones after them print
         * correctly; %n is never written through */
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': (void)va_arg(ap, double); put(&o, '%'); put(&o, *fmt); break;
        default: (void)va_arg(ap, unsigned long); put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}
int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
static int vdprintf(int fd, const char *fmt, va_list ap) { char buf[2048]; int n = vsnprintf(buf, sizeof buf, fmt, ap); if (n > (int)sizeof buf - 1) n = sizeof buf - 1; write(fd, buf, n); return n; }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vdprintf(1, fmt, ap); va_end(ap); return r; }
int fprintf(int fd, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vdprintf(fd, fmt, ap); va_end(ap); return r; }
int dprintf(int fd, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vdprintf(fd, fmt, ap); va_end(ap); return r; }
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }
int getchar(void) { unsigned char c; return read(0, &c, 1) == 1 ? c : EOF; }
char *fgets(char *buf, int n, int fd) { if (n <= 0) return NULL; if (n == 1) { buf[0] = 0; return buf; } int i = 0; while (i < n - 1) { char c; long r = read(fd, &c, 1); if (r <= 0) break; buf[i++] = c; if (c == '\n') break; } buf[i] = 0; return i ? buf : NULL; }
void perror(const char *s) { fprintf(2, "%s: %s\n", s, strerror(errno)); }
