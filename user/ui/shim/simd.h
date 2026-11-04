/* user-space build of kernel sources: vector code is always allowed in user mode */
#pragma once
#include "kernel.h"
struct cpu_features { bool sse3, ssse3, sse41, sse42, popcnt, xsave, xsaveopt, avx, avx2, fma, f16c, avx512f; };
extern struct cpu_features cpu_feat;
static inline bool simd_ok(void) { return true; }
#define SIMD_MIN_BYTES        256
#define SIMD_BLUR_MAX_RADIUS  128
void simd_init(void);
void simd_memcpy(void *dst, const void *src, size_t n);
void simd_memset(void *dst, int c, size_t n);
void simd_memset32(uint32_t *dst, uint32_t v, size_t count);
void simd_blend_span(uint32_t *d, const uint32_t *s, size_t n);
void simd_blend_solid(uint32_t *d, uint32_t col, uint32_t a, size_t n);
void simd_blend_tint(uint32_t *d, const uint32_t *s, uint32_t tint, uint32_t ta, size_t n);
void simd_blend_mask8(uint32_t *d, const uint8_t *m, uint32_t col, uint32_t ca, size_t n);
void simd_blur_h(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius);
void simd_blur_v(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius, void *scratch);
/* the kernel spreads big blurs over its CPU workers; a user program does them in the calling thread */
static inline int smp_workers(void) { return 1; }
static inline void smp_parallel_for(int n, void (*fn)(void *ctx, int i), void *ctx) { for (int i = 0; i < n; i++) fn(ctx, i); }
