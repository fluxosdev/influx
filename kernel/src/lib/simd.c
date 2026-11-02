/* SIMD versions of the kernel's hot loops: memory copy/fill, alpha compositing
 * and the box blur. This file is built with AVX enabled (kmain refuses to boot
 * without it, see the Makefile); the AVX2 variants use a target attribute and
 * are picked at run time from cpu_feat. The 128-bit "_128" routines serve as
 * the fallback on AVX-only CPUs and handle leftovers.
 * Callers gate every entry point with simd_ok(). */
#include "simd.h"
#include "gfx.h"
#include <immintrin.h>

#define TARGET_AVX2 __attribute__((target("avx2")))

static bool use_avx2;

void simd_init(void) {
    use_avx2 = cpu_feat.avx2;
}

/* ---- copy / fill ----
 * Unaligned head and tail vectors, aligned stores in between. */

static void copy_avx(uint8_t *d, const uint8_t *s, size_t n) {
    __m256i head = _mm256_loadu_si256((const __m256i *)s);
    __m256i tail = _mm256_loadu_si256((const __m256i *)(s + n - 32));
    _mm256_storeu_si256((__m256i *)d, head);
    size_t i = 32 - ((uintptr_t)d & 31);
    for (; i + 128 <= n; i += 128) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(s + i + 32));
        __m256i c = _mm256_loadu_si256((const __m256i *)(s + i + 64));
        __m256i e = _mm256_loadu_si256((const __m256i *)(s + i + 96));
        _mm256_store_si256((__m256i *)(d + i), a);
        _mm256_store_si256((__m256i *)(d + i + 32), b);
        _mm256_store_si256((__m256i *)(d + i + 64), c);
        _mm256_store_si256((__m256i *)(d + i + 96), e);
    }
    for (; i + 32 <= n; i += 32) _mm256_store_si256((__m256i *)(d + i), _mm256_loadu_si256((const __m256i *)(s + i)));
    _mm256_storeu_si256((__m256i *)(d + n - 32), tail);
}

/* 'pat' repeats every 4 bytes; d is 4-byte aligned or the pattern is one byte,
 * so every store lands in phase. n >= 32. */
static void fill_avx(uint8_t *d, uint32_t pat, size_t n) {
    __m256i v = _mm256_set1_epi32((int)pat);
    _mm256_storeu_si256((__m256i *)d, v);
    size_t i = 32 - ((uintptr_t)d & 31);
    for (; i + 128 <= n; i += 128) {
        _mm256_store_si256((__m256i *)(d + i), v);
        _mm256_store_si256((__m256i *)(d + i + 32), v);
        _mm256_store_si256((__m256i *)(d + i + 64), v);
        _mm256_store_si256((__m256i *)(d + i + 96), v);
    }
    for (; i + 32 <= n; i += 32) _mm256_store_si256((__m256i *)(d + i), v);
    _mm256_storeu_si256((__m256i *)(d + n - 32), v);
}

void simd_memcpy(void *dst, const void *src, size_t n) { copy_avx(dst, src, n); }
void simd_memset(void *dst, int c, size_t n) { fill_avx(dst, 0x01010101u * (uint8_t)c, n); }
void simd_memset32(uint32_t *dst, uint32_t v, size_t count) { fill_avx((uint8_t *)dst, v, count * 4); }

/* ---- compositing ----
 * Pixels are widened to 16-bit lanes so  (c * a + d * (255 - a)) >> 8  fits
 * without overflow. The colour's alpha lane is set to 256, which turns the
 * same expression into  a + ((da * (255 - a)) >> 8)  for the output alpha.
 * a == 0 and a == 255 are special-cased by blend_px, so they are selected
 * separately here too. 'av' carries the per-pixel alpha in its top byte. */

static inline __m128i sel_128(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }

static inline __m128i blend_half_128(__m128i d16, __m128i c16, __m128i av16) {
    __m128i a = _mm_shufflehi_epi16(_mm_shufflelo_epi16(av16, 0xFF), 0xFF);   /* broadcast each pixel's alpha */
    __m128i ia = _mm_sub_epi16(_mm_set1_epi16(255), a);
    return _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c16, a), _mm_mullo_epi16(d16, ia)), 8);
}

static inline __m128i blend4_128(__m128i d, __m128i col, __m128i av) {
    const __m128i z = _mm_setzero_si128();
    const __m128i a256 = _mm_set1_epi64x(0x0100000000000000LL);
    __m128i c = _mm_and_si128(col, _mm_set1_epi32(0x00FFFFFF));
    __m128i lo = blend_half_128(_mm_unpacklo_epi8(d, z), _mm_or_si128(_mm_unpacklo_epi8(c, z), a256), _mm_unpacklo_epi8(av, z));
    __m128i hi = blend_half_128(_mm_unpackhi_epi8(d, z), _mm_or_si128(_mm_unpackhi_epi8(c, z), a256), _mm_unpackhi_epi8(av, z));
    __m128i r = _mm_packus_epi16(lo, hi);
    __m128i a = _mm_srli_epi32(av, 24);
    r = sel_128(_mm_cmpeq_epi32(a, _mm_set1_epi32(255)), _mm_or_si128(col, _mm_set1_epi32((int)0xFF000000)), r);
    return sel_128(_mm_cmpeq_epi32(a, z), d, r);
}

/* x / 255 for x <= 255 * 255, in 32-bit lanes */
static inline __m128i div255_128(__m128i x) {
    return _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(x, _mm_set1_epi32(1)), _mm_srli_epi32(x, 8)), 8);
}
/* (a * k) / 255 shifted into the alpha byte; a and k are 0..255 in 32-bit lanes */
static inline __m128i scale_alpha_128(__m128i a, __m128i k) {
    return _mm_slli_epi32(div255_128(_mm_mullo_epi16(a, k)), 24);
}

static void blend_span_128(uint32_t *d, const uint32_t *s, size_t n) {
    const __m128i amask = _mm_set1_epi32((int)0xFF000000), z = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i sv = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i sa = _mm_and_si128(sv, amask);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(sa, z)) == 0xFFFF) continue;
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(sa, amask)) == 0xFFFF) { _mm_storeu_si128((__m128i *)(d + i), sv); continue; }
        __m128i dv = _mm_loadu_si128((const __m128i *)(d + i));
        _mm_storeu_si128((__m128i *)(d + i), blend4_128(dv, sv, sv));
    }
    for (; i < n; i++) d[i] = blend_px(d[i], s[i], ALPHA(s[i]));
}

static void blend_solid_128(uint32_t *d, uint32_t col, uint32_t a, size_t n) {
    const __m128i cv = _mm_set1_epi32((int)col), av = _mm_set1_epi32((int)(a << 24));
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i dv = _mm_loadu_si128((const __m128i *)(d + i));
        _mm_storeu_si128((__m128i *)(d + i), blend4_128(dv, cv, av));
    }
    for (; i < n; i++) d[i] = blend_px(d[i], col, a);
}

static void blend_tint_128(uint32_t *d, const uint32_t *s, uint32_t tint, uint32_t ta, size_t n) {
    const __m128i cv = _mm_set1_epi32((int)tint), kv = _mm_set1_epi32((int)ta), z = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i sa = _mm_srli_epi32(_mm_loadu_si128((const __m128i *)(s + i)), 24);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(sa, z)) == 0xFFFF) continue;
        __m128i dv = _mm_loadu_si128((const __m128i *)(d + i));
        _mm_storeu_si128((__m128i *)(d + i), blend4_128(dv, cv, scale_alpha_128(sa, kv)));
    }
    for (; i < n; i++) d[i] = blend_px(d[i], tint, ALPHA(s[i]) * ta / 255);
}

static void blend_mask8_128(uint32_t *d, const uint8_t *m, uint32_t col, uint32_t ca, size_t n) {
    const __m128i cv = _mm_set1_epi32((int)col), kv = _mm_set1_epi32((int)ca), z = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w; __builtin_memcpy(&w, m + i, 4);
        if (!w) continue;
        __m128i ma = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)w), z), z);
        __m128i dv = _mm_loadu_si128((const __m128i *)(d + i));
        _mm_storeu_si128((__m128i *)(d + i), blend4_128(dv, cv, scale_alpha_128(ma, kv)));
    }
    for (; i < n; i++) d[i] = blend_px(d[i], col, m[i] * ca / 255);
}

/* AVX2: the same arithmetic on 8 pixels; unpack/pack/shuffle work per 128-bit
 * lane, which keeps the pixels in order. Leftovers go to the 128-bit versions. */

TARGET_AVX2 static inline __m256i sel_avx2(__m256i m, __m256i a, __m256i b) { return _mm256_blendv_epi8(b, a, m); }

TARGET_AVX2 static inline __m256i blend_half_avx2(__m256i d16, __m256i c16, __m256i av16) {
    __m256i a = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(av16, 0xFF), 0xFF);
    __m256i ia = _mm256_sub_epi16(_mm256_set1_epi16(255), a);
    return _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(c16, a), _mm256_mullo_epi16(d16, ia)), 8);
}

TARGET_AVX2 static inline __m256i blend8_avx2(__m256i d, __m256i col, __m256i av) {
    const __m256i z = _mm256_setzero_si256();
    const __m256i a256 = _mm256_set1_epi64x(0x0100000000000000LL);
    __m256i c = _mm256_and_si256(col, _mm256_set1_epi32(0x00FFFFFF));
    __m256i lo = blend_half_avx2(_mm256_unpacklo_epi8(d, z), _mm256_or_si256(_mm256_unpacklo_epi8(c, z), a256), _mm256_unpacklo_epi8(av, z));
    __m256i hi = blend_half_avx2(_mm256_unpackhi_epi8(d, z), _mm256_or_si256(_mm256_unpackhi_epi8(c, z), a256), _mm256_unpackhi_epi8(av, z));
    __m256i r = _mm256_packus_epi16(lo, hi);
    __m256i a = _mm256_srli_epi32(av, 24);
    r = sel_avx2(_mm256_cmpeq_epi32(a, _mm256_set1_epi32(255)), _mm256_or_si256(col, _mm256_set1_epi32((int)0xFF000000)), r);
    return sel_avx2(_mm256_cmpeq_epi32(a, z), d, r);
}

TARGET_AVX2 static inline __m256i scale_alpha_avx2(__m256i a, __m256i k) {
    __m256i x = _mm256_mullo_epi16(a, k);
    x = _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(x, _mm256_set1_epi32(1)), _mm256_srli_epi32(x, 8)), 8);
    return _mm256_slli_epi32(x, 24);
}

TARGET_AVX2 static void blend_span_avx2(uint32_t *d, const uint32_t *s, size_t n) {
    const __m256i amask = _mm256_set1_epi32((int)0xFF000000), z = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i sv = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i sa = _mm256_and_si256(sv, amask);
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(sa, z)) == -1) continue;
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(sa, amask)) == -1) { _mm256_storeu_si256((__m256i *)(d + i), sv); continue; }
        __m256i dv = _mm256_loadu_si256((const __m256i *)(d + i));
        _mm256_storeu_si256((__m256i *)(d + i), blend8_avx2(dv, sv, sv));
    }
    if (i < n) blend_span_128(d + i, s + i, n - i);
}

TARGET_AVX2 static void blend_solid_avx2(uint32_t *d, uint32_t col, uint32_t a, size_t n) {
    const __m256i cv = _mm256_set1_epi32((int)col), av = _mm256_set1_epi32((int)(a << 24));
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i dv = _mm256_loadu_si256((const __m256i *)(d + i));
        _mm256_storeu_si256((__m256i *)(d + i), blend8_avx2(dv, cv, av));
    }
    if (i < n) blend_solid_128(d + i, col, a, n - i);
}

TARGET_AVX2 static void blend_tint_avx2(uint32_t *d, const uint32_t *s, uint32_t tint, uint32_t ta, size_t n) {
    const __m256i cv = _mm256_set1_epi32((int)tint), kv = _mm256_set1_epi32((int)ta), z = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i sa = _mm256_srli_epi32(_mm256_loadu_si256((const __m256i *)(s + i)), 24);
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(sa, z)) == -1) continue;
        __m256i dv = _mm256_loadu_si256((const __m256i *)(d + i));
        _mm256_storeu_si256((__m256i *)(d + i), blend8_avx2(dv, cv, scale_alpha_avx2(sa, kv)));
    }
    if (i < n) blend_tint_128(d + i, s + i, tint, ta, n - i);
}

TARGET_AVX2 static void blend_mask8_avx2(uint32_t *d, const uint8_t *m, uint32_t col, uint32_t ca, size_t n) {
    const __m256i cv = _mm256_set1_epi32((int)col), kv = _mm256_set1_epi32((int)ca);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w; __builtin_memcpy(&w, m + i, 8);
        if (!w) continue;
        __m256i ma = _mm256_cvtepu8_epi32(_mm_cvtsi64_si128((long long)w));
        __m256i dv = _mm256_loadu_si256((const __m256i *)(d + i));
        _mm256_storeu_si256((__m256i *)(d + i), blend8_avx2(dv, cv, scale_alpha_avx2(ma, kv)));
    }
    if (i < n) blend_mask8_128(d + i, m + i, col, ca, n - i);
}

void simd_blend_span(uint32_t *d, const uint32_t *s, size_t n) {
    if (use_avx2) blend_span_avx2(d, s, n); else blend_span_128(d, s, n);
}
void simd_blend_solid(uint32_t *d, uint32_t col, uint32_t a, size_t n) {
    if (use_avx2) blend_solid_avx2(d, col, a, n); else blend_solid_128(d, col, a, n);
}
void simd_blend_tint(uint32_t *d, const uint32_t *s, uint32_t tint, uint32_t ta, size_t n) {
    if (use_avx2) blend_tint_avx2(d, s, tint, ta, n); else blend_tint_128(d, s, tint, ta, n);
}
void simd_blend_mask8(uint32_t *d, const uint8_t *m, uint32_t col, uint32_t ca, size_t n) {
    if (use_avx2) blend_mask8_avx2(d, m, col, ca, n); else blend_mask8_128(d, m, col, ca, n);
}

/* ---- box blur ----
 * One pixel's four channels sit in a vector of 32-bit sums. The average is
 * (sum + 0.5) * (1 / n) truncated, which equals sum / n exactly for
 * n <= 2 * SIMD_BLUR_MAX_RADIUS + 1. */

static inline __m128i px_widen(uint32_t p) {
    const __m128i z = _mm_setzero_si128();
    return _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p), z), z);
}
static inline uint32_t px_average(__m128i sum, __m128 inv) {
    __m128i q = _mm_cvttps_epi32(_mm_mul_ps(_mm_add_ps(_mm_cvtepi32_ps(sum), _mm_set1_ps(0.5f)), inv));
    q = _mm_packs_epi32(q, q);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(q, q)) | 0xFF000000u;
}

void simd_blur_h(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius) {
    __m128 inv = _mm_set1_ps(1.0f / (float)(2 * radius + 1));
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + (size_t)y * sstride;
        uint32_t *out = dst + (size_t)y * dstride;
        __m128i sum = _mm_setzero_si128();
        for (int i = -radius; i <= radius; i++) sum = _mm_add_epi32(sum, px_widen(row[CLAMP(i, 0, w - 1)]));
        for (int x = 0; x < w; x++) {
            out[x] = px_average(sum, inv);
            sum = _mm_add_epi32(sum, _mm_sub_epi32(px_widen(row[CLAMP(x + radius + 1, 0, w - 1)]), px_widen(row[CLAMP(x - radius, 0, w - 1)])));
        }
    }
}

/* The vertical pass walks rows in memory order, keeping a running sum per
 * column, instead of striding down one column at a time. */
static void blur_v_cols_128(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int x0, int w, int h, int radius, uint8_t *sums) {
    __m128 inv = _mm_set1_ps(1.0f / (float)(2 * radius + 1));
    for (int x = x0; x < w; x++) _mm_storeu_si128((__m128i *)(sums + x * 16), _mm_setzero_si128());
    for (int i = -radius; i <= radius; i++) {
        const uint32_t *row = src + (size_t)CLAMP(i, 0, h - 1) * sstride;
        for (int x = x0; x < w; x++) {
            __m128i *p = (__m128i *)(sums + x * 16);
            _mm_storeu_si128(p, _mm_add_epi32(_mm_loadu_si128(p), px_widen(row[x])));
        }
    }
    for (int y = 0; y < h; y++) {
        const uint32_t *in = src + (size_t)CLAMP(y + radius + 1, 0, h - 1) * sstride;
        const uint32_t *old = src + (size_t)CLAMP(y - radius, 0, h - 1) * sstride;
        uint32_t *out = dst + (size_t)y * dstride;
        for (int x = x0; x < w; x++) {
            __m128i *p = (__m128i *)(sums + x * 16);
            __m128i sum = _mm_loadu_si128(p);
            out[x] = px_average(sum, inv);
            _mm_storeu_si128(p, _mm_add_epi32(sum, _mm_sub_epi32(px_widen(in[x]), px_widen(old[x]))));
        }
    }
}

/* AVX2: two neighbouring pixels (eight channel sums) per vector */
TARGET_AVX2 static inline __m256i px2_widen(const uint32_t *p) {
    return _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)p));
}

TARGET_AVX2 static void blur_v_avx2(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius, uint8_t *sums) {
    __m256 inv = _mm256_set1_ps(1.0f / (float)(2 * radius + 1));
    const __m128i opaque = _mm_set1_epi32((int)0xFF000000);
    int w2 = w & ~1;
    for (int x = 0; x < w2; x += 2) _mm256_storeu_si256((__m256i *)(sums + x * 16), _mm256_setzero_si256());
    for (int i = -radius; i <= radius; i++) {
        const uint32_t *row = src + (size_t)CLAMP(i, 0, h - 1) * sstride;
        for (int x = 0; x < w2; x += 2) {
            __m256i *p = (__m256i *)(sums + x * 16);
            _mm256_storeu_si256(p, _mm256_add_epi32(_mm256_loadu_si256(p), px2_widen(row + x)));
        }
    }
    for (int y = 0; y < h; y++) {
        const uint32_t *in = src + (size_t)CLAMP(y + radius + 1, 0, h - 1) * sstride;
        const uint32_t *old = src + (size_t)CLAMP(y - radius, 0, h - 1) * sstride;
        uint32_t *out = dst + (size_t)y * dstride;
        for (int x = 0; x < w2; x += 2) {
            __m256i *p = (__m256i *)(sums + x * 16);
            __m256i sum = _mm256_loadu_si256(p);
            __m256i q = _mm256_cvttps_epi32(_mm256_mul_ps(_mm256_add_ps(_mm256_cvtepi32_ps(sum), _mm256_set1_ps(0.5f)), inv));
            __m128i w16 = _mm_packs_epi32(_mm256_castsi256_si128(q), _mm256_extracti128_si256(q, 1));
            _mm_storel_epi64((__m128i *)(out + x), _mm_or_si128(_mm_packus_epi16(w16, w16), opaque));
            _mm256_storeu_si256(p, _mm256_add_epi32(sum, _mm256_sub_epi32(px2_widen(in + x), px2_widen(old + x))));
        }
    }
    if (w2 < w) blur_v_cols_128(src, sstride, dst, dstride, w2, w, h, radius, sums);
}

void simd_blur_v(const uint32_t *src, int sstride, uint32_t *dst, int dstride, int w, int h, int radius, void *scratch) {
    if (use_avx2) blur_v_avx2(src, sstride, dst, dstride, w, h, radius, scratch);
    else blur_v_cols_128(src, sstride, dst, dstride, 0, w, h, radius, scratch);
}
