#include "arch.h"
#include "io.h"
#include "string.h"

static uint64_t tsc_hz;

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
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
