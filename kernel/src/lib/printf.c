#include "kernel.h"
#include "string.h"
#include "arch.h"
#include "spinlock.h"

/* Minimal printf: %d %i %u %x %X %p %s %c %% %ld %lu %lx %lld %llu %llx %zu %zx with
 * width, '-', '0', and precision for strings/ints. No floating point. */

struct out { char *buf; size_t n, pos; };
static void put(struct out *o, char c) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; o->pos++; }

static void pad(struct out *o, int count, char c) { while (count-- > 0) put(o, c); }

static void fmt_num(struct out *o, uint64_t v, bool neg, int base, bool upper, int width, int prec, bool left, bool zero, bool alt) {
    char tmp[32]; int len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[len++] = '0';
    while (v) { tmp[len++] = digits[v % base]; v /= base; }
    while (len < prec && len < (int)sizeof tmp) tmp[len++] = '0';
    int total = len + (neg ? 1 : 0) + (alt && base == 16 ? 2 : 0);
    if (!left && !zero) pad(o, width - total, ' ');
    if (neg) put(o, '-');
    if (alt && base == 16) { put(o, '0'); put(o, upper ? 'X' : 'x'); }
    if (!left && zero) pad(o, width - total, '0');
    while (len) put(o, tmp[--len]);
    if (left) pad(o, width - total, ' ');
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out o = { buf, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool left = false, zero = false, alt = false, plus = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true; else if (*fmt == '0') zero = true;
            else if (*fmt == '#') alt = true; else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') {} else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; } else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; } else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 2; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        if (!*fmt) break;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0; uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            if (!neg && plus) put(&o, '+');
            fmt_num(&o, u, neg, 10, false, width, prec, left, zero, false);
            break; }
        case 'u': { uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); fmt_num(&o, v, false, 10, false, width, prec, left, zero, false); break; }
        case 'x': case 'X': { uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); fmt_num(&o, v, false, 16, *fmt == 'X', width, prec, left, zero, alt); break; }
        case 'o': { uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); fmt_num(&o, v, false, 8, false, width, prec, left, zero, false); break; }
        case 'p': { uint64_t v = (uint64_t)va_arg(ap, void *); fmt_num(&o, v, false, 16, false, width, 16, left, false, true); break; }
        case 'c': { char c = (char)va_arg(ap, int); if (!left) pad(&o, width - 1, ' '); put(&o, c); if (left) pad(&o, width - 1, ' '); break; }
        case 's': {
            const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, prec) : strlen(s));
            if (!left) pad(&o, width - len, ' ');
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) pad(&o, width - len, ' ');
            break; }
        case '%': put(&o, '%'); break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r;
}

static spinlock_t print_lock = SPINLOCK_INIT;
void kvprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    spin_lock(&print_lock);
    serial_write(buf, n);
    spin_unlock(&print_lock);
}
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); kvprintf(fmt, ap); va_end(ap); }

void NORETURN panic(const char *fmt, ...) {
    cli();
    kprintf("\n\033[31;1m*** KERNEL PANIC ***\033[0m\n");
    va_list ap; va_start(ap, fmt); kvprintf(fmt, ap); va_end(ap);
    kprintf("\n");
    for (;;) hlt();
}
