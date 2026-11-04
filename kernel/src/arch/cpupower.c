/*
 * Processor power management.
 *
 * Idle: a CPU with nothing to run waits in MWAIT at the deepest C-state
 * CPUID leaf 5 lists, when the local APIC timer keeps running in deep
 * states (ARAT) -- the scheduler tick depends on it; otherwise in HLT (C1).
 * Any interrupt, including the reschedule IPI, ends the wait.
 *
 * Frequency: on Intel processors with hardware P-states (HWP, Skylake and
 * later) the hardware picks frequencies itself, given the full range and a
 * balanced energy preference. Older ones with Enhanced SpeedStep (Haswell)
 * get a small governor: every 50 ms each CPU sets its own P-state from how
 * busy it was, the top ratio (turbo, when allowed) above 80 %, the lowest
 * below 20 %, in between proportionally. Every MSR access goes through
 * rdmsr_safe/wrmsr_safe, so a processor missing one disables the feature
 * instead of faulting. Nothing changes under a hypervisor.
 */
#include "arch.h"
#include "io.h"
#include "power.h"
#include "percpu.h"
#include "string.h"

#define MSR_PLATFORM_INFO   0xCE
#define MSR_PERF_CTL        0x199
#define MSR_PERF_STATUS     0x198
#define MSR_MISC_ENABLE     0x1A0
#define MSR_TURBO_RATIO     0x1AD
#define MSR_PM_ENABLE       0x770
#define MSR_HWP_CAPABILITIES 0x771
#define MSR_HWP_REQUEST     0x774

static bool use_mwait;
static uint32_t mwait_hint;
static enum { GOV_NONE, GOV_HWP, GOV_EIST } gov;
static int ratio_min, ratio_max;
static char desc[96] = "C1 (hlt)";
static struct { uint64_t busy_mark, ticks_mark; int ratio; } pcpu[MAX_CPUS];

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

/* the BSP decides; every CPU (BSP included) then applies the per-CPU parts */
static void decide(void) {
    uint32_t a, b, c, d, max;
    cpuid(0, 0, &max, &b, &c, &d);
    bool intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;   /* GenuineIntel */
    cpuid(1, 0, &a, &b, &c, &d);
    bool hypervisor = c & (1u << 31), has_mwait = c & (1u << 3), eist = c & (1u << 7);
    if (hypervisor) { strlcpy(desc, "C1 (hlt), frequency left to the hypervisor", sizeof desc); return; }
    bool arat = false, hwp = false;
    if (max >= 6) { cpuid(6, 0, &a, &b, &c, &d); arat = a & (1u << 2); hwp = a & (1u << 7); }
    if (has_mwait && arat && max >= 5) {
        cpuid(5, 0, &a, &b, &c, &d);
        /* EDX: sub-state counts per C-state, 4 bits each, C0 first; MWAIT hint = (state << 4) | (substate) */
        uint32_t subs = d >> 4;
        int best = -1, best_sub = 0;
        for (int i = 0; i < 7 && subs; i++, subs >>= 4) if (subs & 0xF) { best = i; best_sub = subs & 0xF; }
        if ((c & 1) && best >= 0) { use_mwait = true; mwait_hint = ((uint32_t)best << 4) | (uint32_t)(best_sub - 1); }
    }
    uint64_t v;
    if (intel && hwp && rdmsr_safe(MSR_HWP_CAPABILITIES, &v) == 0) gov = GOV_HWP;
    else if (intel && eist && rdmsr_safe(MSR_PLATFORM_INFO, &v) == 0) {
        ratio_max = (int)((v >> 8) & 0xFF);
        ratio_min = (int)((v >> 40) & 0xFF);
        uint64_t misc, turbo;
        if (rdmsr_safe(MSR_MISC_ENABLE, &misc) == 0 && !(misc & (1ULL << 38)) && rdmsr_safe(MSR_TURBO_RATIO, &turbo) == 0 && (int)(turbo & 0xFF) > ratio_max)
            ratio_max = (int)(turbo & 0xFF);
        if (ratio_min && ratio_max > ratio_min) gov = GOV_EIST;
    }
    snprintf(desc, sizeof desc, "%s%s", use_mwait ? "deep C-states (mwait)" : "C1 (hlt)",
             gov == GOV_HWP ? ", hardware P-states (balanced)" : gov == GOV_EIST ? ", SpeedStep governor" : "");
    if (gov == GOV_EIST) snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " %d-%d00 MHz", ratio_min, ratio_max);
}

void cpupower_init(void) {
    struct cpu *cpu = this_cpu();
    if (cpu->id == 0) { decide(); kprintf("cpupower: %s\n", desc); }
    if (gov == GOV_HWP) {
        uint64_t caps;
        if (wrmsr_safe(MSR_PM_ENABLE, 1) < 0 || rdmsr_safe(MSR_HWP_CAPABILITIES, &caps) < 0) { gov = GOV_NONE; return; }
        uint64_t highest = caps & 0xFF, lowest = (caps >> 24) & 0xFF;
        /* min, max, desired 0 (autonomous), energy preference 0x80 (balanced) */
        if (wrmsr_safe(MSR_HWP_REQUEST, lowest | (highest << 8) | (0x80ULL << 24)) < 0) gov = GOV_NONE;
    } else if (gov == GOV_EIST) {
        uint64_t misc;
        if (rdmsr_safe(MSR_MISC_ENABLE, &misc) == 0 && !(misc & (1ULL << 16))) wrmsr_safe(MSR_MISC_ENABLE, misc | (1ULL << 16));   /* enable SpeedStep */
        pcpu[cpu->id].ratio = 0;
    }
}

const char *cpupower_describe(void) { return desc; }

/* every timer tick on every CPU */
void cpupower_tick(void) {
    if (gov != GOV_EIST) return;
    struct cpu *c = this_cpu();
    if (c->ticks - pcpu[c->id].ticks_mark < 50) return;
    uint64_t span = c->ticks - pcpu[c->id].ticks_mark;
    uint64_t busy = (c->ticks - c->idle_ticks) - pcpu[c->id].busy_mark;
    pcpu[c->id].ticks_mark = c->ticks; pcpu[c->id].busy_mark = c->ticks - c->idle_ticks;
    int load = (int)(busy * 100 / (span ? span : 1));
    int ratio = load >= 80 ? ratio_max : load <= 20 ? ratio_min : ratio_min + (ratio_max - ratio_min) * (load - 20) / 60;
    if (ratio == pcpu[c->id].ratio) return;
    if (wrmsr_safe(MSR_PERF_CTL, (uint64_t)ratio << 8) == 0) pcpu[c->id].ratio = ratio;
    else gov = GOV_NONE;   /* the governor stops for good on a processor that refuses */
}

/* interrupts are off on entry: armed and waiting without a window where a wakeup is lost */
void cpu_idle_wait(void) {
    if (use_mwait) {
        static volatile uint64_t monitor_line[MAX_CPUS * 8] ALIGNED(64);
        volatile void *addr = &monitor_line[this_cpu()->id * 8];
        __asm__ volatile("monitor" :: "a"(addr), "c"(0), "d"(0));
        /* ECX bit 0: an interrupt ends the wait even with IF clear; it is taken at the sti */
        __asm__ volatile("mwait" :: "a"(mwait_hint), "c"(1));
        sti();
        return;
    }
    __asm__ volatile("sti; hlt");
}
