/* user-space build of kernel sources: the kernel heap is malloc */
#pragma once
#include <stdlib.h>
#include <string.h>
static inline void *kmalloc(size_t n) { return malloc(n ? n : 1); }
static inline void *kzalloc(size_t n) { return calloc(1, n ? n : 1); }
static inline void *kcalloc(size_t a, size_t b) { return calloc(a ? a : 1, b ? b : 1); }
static inline void *krealloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
static inline void kfree(void *p) { free(p); }
static inline void *kmalloc_try(size_t n) { return malloc(n ? n : 1); }
static inline void *kzalloc_try(size_t n) { return calloc(1, n ? n : 1); }
static inline void *krealloc_try(void *p, size_t n) { return realloc(p, n ? n : 1); }
static inline char *kstrdup(const char *s) { return strdup(s); }
