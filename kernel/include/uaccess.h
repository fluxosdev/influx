#pragma once
/* Kernel access to user memory. Every system call goes through these: a bad
 * pointer, or memory another thread unmaps meanwhile, gives -EFAULT instead
 * of a kernel fault, and pages not touched yet are faulted in on the way. */
#include "kernel.h"

#define USER_MAX 0x0000800000000000ULL
#ifndef EFAULT
#define EFAULT 14
#endif

long __copy_user(void *dst, const void *src, size_t n);
long __strncpy_from_user(char *dst, const char *src, size_t max);
long __clear_user(void *dst, size_t n);

static inline bool user_range(uint64_t addr, size_t n) { return addr < USER_MAX && n <= USER_MAX - addr; }

static inline long copy_from_user(void *dst, uint64_t usrc, size_t n) {
    if (!n) return 0;
    if (!user_range(usrc, n)) return -EFAULT;
    return __copy_user(dst, (const void *)usrc, n);
}
static inline long copy_to_user(uint64_t udst, const void *src, size_t n) {
    if (!n) return 0;
    if (!user_range(udst, n)) return -EFAULT;
    return __copy_user((void *)udst, src, n);
}
static inline long clear_user(uint64_t udst, size_t n) {
    if (!n) return 0;
    if (!user_range(udst, n)) return -EFAULT;
    return __clear_user((void *)udst, n);
}
/* copies a NUL-terminated string of at most max-1 characters; returns its length or -errno */
static inline long strncpy_from_user(char *dst, uint64_t usrc, size_t max) {
    if (usrc >= USER_MAX) return -EFAULT;
    size_t lim = MIN((uint64_t)max, USER_MAX - usrc);
    long r = __strncpy_from_user(dst, (const char *)usrc, lim);
    if (r == -36 && lim < max) return -EFAULT;   /* ran into the end of user space */
    return r;
}
#define get_user(var, uaddr) copy_from_user(&(var), (uint64_t)(uaddr), sizeof(var))
#define put_user(val, uaddr) ({ __typeof__(val) __v = (val); copy_to_user((uint64_t)(uaddr), &__v, sizeof(__v)); })
