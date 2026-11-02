#include "string.h"
#include "mm.h"
#include "simd.h"

static void *memcpy_scalar(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    if (n >= 8 && !(((uintptr_t)d | (uintptr_t)s) & 7)) {
        uint64_t *d8 = (uint64_t *)d; const uint64_t *s8 = (const uint64_t *)s;
        while (n >= 8) { *d8++ = *s8++; n -= 8; }
        d = (uint8_t *)d8; s = (const uint8_t *)s8;
    }
    while (n--) *d++ = *s++;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t n) {
    if (n >= SIMD_MIN_BYTES && simd_ok()) { simd_memcpy(dst, src, n); return dst; }
    return memcpy_scalar(dst, src, n);
}
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    if (d == s || n == 0) return dst;
    if (d >= s + n || s >= d + n) return memcpy(dst, src, n);
    if (d < s) return memcpy_scalar(dst, src, n);   /* forward copy is safe; the SIMD one is not */
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    if (n >= SIMD_MIN_BYTES && simd_ok()) { simd_memset(dst, c, n); return dst; }
    uint8_t *d = dst; uint8_t v = (uint8_t)c;
    if (n >= 8 && !((uintptr_t)d & 7)) {
        uint64_t v8 = 0x0101010101010101ULL * v;
        uint64_t *d8 = (uint64_t *)d;
        while (n >= 8) { *d8++ = v8; n -= 8; }
        d = (uint8_t *)d8;
    }
    while (n--) *d++ = v;
    return dst;
}
void memset32(uint32_t *dst, uint32_t v, size_t count) {
    if (count >= 16 && !((uintptr_t)dst & 3) && simd_ok()) { simd_memset32(dst, v, count); return; }
    while (count--) *dst++ = v;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (size_t i = 0; i < n; i++) if (p[i] == (uint8_t)c) return (void *)(p + i);
    return NULL;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i]; if (!a[i]) return 0; }
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
char *strcpy(char *dst, const char *src) { char *d = dst; while ((*d++ = *src++)); return dst; }
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t c = n >= size ? size - 1 : n; memcpy(dst, src, c); dst[c] = 0; }
    return n;
}
size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dl = strnlen(dst, size);
    if (dl == size) return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}
char *strcat(char *dst, const char *src) { strcpy(dst + strlen(dst), src); return dst; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strrchr(const char *s, int c) { const char *r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, nl)) return (char *)h;
    return NULL;
}
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = kmalloc(n); if (d) memcpy(d, s, n); return d; }
char *strndup(const char *s, size_t n) { size_t l = strnlen(s, n); char *d = kmalloc(l + 1); if (d) { memcpy(d, s, l); d[l] = 0; } return d; }
char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *save = s; return NULL; }
    char *start = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) *s++ = 0;
    *save = s;
    return start;
}
unsigned long strtoul(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    if (base == 0) { if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; } else if (s[0] == '0') base = 8; else base = 10; }
    else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long v = 0;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (isalpha(*s)) d = tolower(*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return v;
}
long strtol(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
    long v = (long)strtoul(s, end, base);
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
bool isdigit(int c) { return c >= '0' && c <= '9'; }
bool isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isalnum(int c) { return isalpha(c) || isdigit(c); }
bool isupper(int c) { return c >= 'A' && c <= 'Z'; }
bool islower(int c) { return c >= 'a' && c <= 'z'; }
bool isprint(int c) { return c >= 32 && c < 127; }
bool isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
