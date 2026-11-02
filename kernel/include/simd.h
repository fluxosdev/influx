#pragma once
#include "kernel.h"
#include "sched.h"

/* What the CPU (and our XCR0 setup) lets us use; filled in by cpu_init().
 * AVX is required: kmain halts at the boot splash when cpu_feat.avx is false. */
struct cpu_features {
    bool sse3, ssse3, sse41, sse42, popcnt;
    bool xsave, xsaveopt, avx, avx2, fma, f16c, avx512f;
};
extern struct cpu_features cpu_feat;
extern uint64_t cpu_xcr0;          /* enabled XSAVE state components */
extern uint32_t fpu_state_size;    /* bytes of x87/SSE/AVX state saved per thread */

void cpu_simd_describe(char *out, size_t n);   /* "SSE4.2, AVX2, FMA" style summary */

/* Per-thread extended register state (64-byte aligned for XSAVE) */
uint8_t *fpu_state_alloc(void **mem);   /* *mem is what to kfree() */
void fpu_save(uint8_t *area);
void fpu_restore(uint8_t *area);

/* Kernel code may only touch vector registers on a kernel thread outside
 * interrupt context: the scheduler saves them on every switch, but IRQ entry
 * and syscalls from user threads do not, so those would clobber live state. */
static inline bool simd_ok(void) {
    struct thread *t = current_thread;
    return t && !t->is_user && !t->irq_depth;
}

#define SIMD_MIN_BYTES        256   /* below this, memcpy/memset stay scalar */
#define SIMD_BLUR_MAX_RADIUS  128   /* the blur's float divide is exact up to here */

/* AVX baseline, AVX2 when available (kernel/src/lib/simd.c).
 * Only call these when simd_ok() is true. */
void simd_init(void);
void simd_memcpy(void *dst, const void *src, size_t n);          /* n >= 32, no overlap */
void simd_memset(void *dst, int c, size_t n);                    /* n >= 32 */
void simd_memset32(uint32_t *dst, uint32_t v, size_t count);     /* count >= 8, dst 4-byte aligned */
/* src-over compositing spans, bit-exact with blend_px() in gfx.h */
void simd_blend_span(uint32_t *d, const uint32_t *s, size_t n);                       /* alpha = ALPHA(s) */
void simd_blend_solid(uint32_t *d, uint32_t col, uint32_t a, size_t n);               /* constant alpha */
void simd_blend_tint(uint32_t *d, const uint32_t *s, uint32_t tint, uint32_t ta, size_t n); /* alpha = ALPHA(s) * ta / 255 */
void simd_blend_mask8(uint32_t *d, const uint8_t *m, uint32_t col, uint32_t ca, size_t n);  /* alpha = m * ca / 255 */
/* one box-blur pass (radius <= SIMD_BLUR_MAX_RADIUS); output alpha is 255 */
void simd_blur_h(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius);
void simd_blur_v(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius, void *scratch /* w * 16 bytes */);
