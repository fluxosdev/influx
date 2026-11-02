#include "arch.h"
#include "io.h"
#include "string.h"
#include "mm.h"
#include "simd.h"

static uint64_t tsc_hz;
struct cpu_features cpu_feat;
uint64_t cpu_xcr0;
uint32_t fpu_state_size;

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline void xsetbv(uint32_t reg, uint64_t v) { __asm__ volatile("xsetbv" :: "c"(reg), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }

#define XCR0_X87     (1ULL << 0)
#define XCR0_SSE     (1ULL << 1)
#define XCR0_AVX     (1ULL << 2)
#define XCR0_AVX512  (7ULL << 5)    /* opmask, ZMM0-15 upper halves, ZMM16-31 */

/* Detect SSE3..AVX-512 and turn on the XSAVE state components the CPU has, so
 * AVX code works and the scheduler saves the full YMM/ZMM registers. AVX is
 * required: without it cpu_feat.avx stays false and kmain halts at the splash. */
static void simd_setup(void) {
    uint32_t a, b, c, d, max;
    cpuid(0, 0, &max, &b, &c, &d);
    cpuid(1, 0, &a, &b, &c, &d);
    cpu_feat.sse3 = c & (1 << 0); cpu_feat.ssse3 = c & (1 << 9); cpu_feat.sse41 = c & (1 << 19);
    cpu_feat.sse42 = c & (1 << 20); cpu_feat.popcnt = c & (1 << 23); cpu_feat.xsave = c & (1 << 26);
    bool avx = c & (1 << 28), fma = c & (1 << 12), f16c = c & (1 << 29), avx2 = false, avx512f = false;
    if (max >= 7) { cpuid(7, 0, &a, &b, &c, &d); avx2 = b & (1 << 5); avx512f = b & (1 << 16); }
    if (!cpu_feat.xsave || !avx || max < 0xD) return;
    cpuid(0xD, 0, &a, &b, &c, &d);
    uint64_t supported = a | ((uint64_t)d << 32);
    if (!(supported & XCR0_AVX)) return;
    write_cr4(read_cr4() | (1ULL << 18));   /* OSXSAVE */
    uint64_t xcr0 = XCR0_X87 | XCR0_SSE | XCR0_AVX;
    if (avx512f && (supported & XCR0_AVX512) == XCR0_AVX512) xcr0 |= XCR0_AVX512;
    xsetbv(0, xcr0);
    cpu_xcr0 = xcr0;
    cpuid(0xD, 0, &a, &b, &c, &d);
    fpu_state_size = b;                       /* XSAVE area for the components now enabled */
    cpuid(0xD, 1, &a, &b, &c, &d);
    cpu_feat.xsaveopt = a & 1;
    cpu_feat.avx = true;
    cpu_feat.avx2 = avx2;
    cpu_feat.fma = fma;
    cpu_feat.f16c = f16c;
    cpu_feat.avx512f = avx512f && (cpu_xcr0 & XCR0_AVX512) == XCR0_AVX512;
    simd_init();
}

void cpu_simd_describe(char *out, size_t n) {
    const char *base = cpu_feat.sse42 ? "SSE4.2" : cpu_feat.sse41 ? "SSE4.1" : cpu_feat.ssse3 ? "SSSE3" : cpu_feat.sse3 ? "SSE3" : "SSE2";
    snprintf(out, n, "%s, %s%s%s", base,
             cpu_feat.avx512f ? "AVX-512" : cpu_feat.avx2 ? "AVX2" : "AVX",
             cpu_feat.fma ? ", FMA" : "", cpu_feat.f16c ? ", F16C" : "");
}

/* ---- per-thread x87/SSE/AVX state ---- */

uint8_t *fpu_state_alloc(void **mem) {
    void *m = kmalloc(fpu_state_size + 63);
    uint8_t *p = (uint8_t *)(((uintptr_t)m + 63) & ~63ULL);
    memset(p, 0, fpu_state_size);
    *(uint16_t *)p = 0x37F;                  /* FCW */
    *(uint32_t *)(p + 24) = 0x1F80;          /* MXCSR */
    *(uint64_t *)(p + 512) = XCR0_X87 | XCR0_SSE;   /* XSTATE_BV: the rest starts in init state */
    *mem = m;
    return p;
}

void fpu_save(uint8_t *area) {
    if (cpu_feat.xsaveopt) __asm__ volatile("xsaveopt64 (%0)" :: "r"(area), "a"(-1), "d"(-1) : "memory");
    else __asm__ volatile("xsave64 (%0)" :: "r"(area), "a"(-1), "d"(-1) : "memory");
}

void fpu_restore(uint8_t *area) {
    __asm__ volatile("xrstor64 (%0)" :: "r"(area), "a"(-1), "d"(-1) : "memory");
}

void cpu_init(void) {
    /* Enable SSE/FXSR so user programs (and fxsave/fxrstor) work */
    uint64_t cr0 = read_cr0();
    cr0 &= ~(1ULL << 2);   /* EM */
    cr0 |= (1ULL << 1);    /* MP */
    cr0 |= (1ULL << 16);   /* WP: enforce write protection in ring 0 */
    write_cr0(cr0);
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 9) | (1ULL << 10);   /* OSFXSR, OSXMMEXCPT */
    write_cr4(cr4);
    __asm__ volatile("fninit");
    simd_setup();

    /* Enable NX (EFER.NXE) and SCE for syscall/sysret */
    uint64_t efer = rdmsr(0xC0000080);
    efer |= (1 << 11) | (1 << 0);
    wrmsr(0xC0000080, efer);

    /* PAT: make entry 1 write-combining (used for the framebuffer via PWT) */
    uint64_t pat = rdmsr(0x277);
    pat &= ~(0xFFULL << 8);
    pat |= (0x01ULL << 8);
    wrmsr(0x277, pat);
}

void cpu_get_brand(char *out, size_t n) {
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) { strlcpy(out, "Unknown x86-64 CPU", n); return; }
    char brand[49]; uint32_t *p = (uint32_t *)brand;
    for (uint32_t i = 0; i < 3; i++) { cpuid(0x80000002 + i, 0, &a, &b, &c, &d); p[i * 4] = a; p[i * 4 + 1] = b; p[i * 4 + 2] = c; p[i * 4 + 3] = d; }
    brand[48] = 0;
    const char *s = brand; while (*s == ' ') s++;
    strlcpy(out, s, n);
}

void cpu_set_tsc_hz(uint64_t hz) { tsc_hz = hz; }
uint64_t cpu_tsc_hz(void) { return tsc_hz; }
