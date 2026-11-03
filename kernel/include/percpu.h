#pragma once
#include "kernel.h"

/* Per-CPU state, reached through the GS base. Kernel code always runs with
 * GS pointing at its CPU's struct cpu; the entry stubs swapgs on the way in
 * from (and out to) user mode, which keeps user GS at 0 in KERNEL_GS_BASE. */
#define MAX_CPUS 64

struct thread;
struct cpu {
    struct cpu *self;             /* %gs:0  */
    struct thread *current;       /* %gs:8  */
    uint64_t syscall_rsp;         /* %gs:16 kernel stack for the syscall entry stub */
    uint64_t user_rsp;            /* %gs:24 scratch while the syscall stub switches stacks */
    int id;                       /* index into cpus[]; 0 is the BSP */
    uint32_t lapic_id;
    struct thread *idle;
    struct thread *prev;          /* thread this CPU just switched away from (finished by the next one) */
    volatile bool online;
    uint64_t ticks;               /* timer ticks taken on this CPU */
    uint64_t idle_ticks;
    int load_percent;             /* busy share over the last sample (task manager) */
    uint64_t sample_ticks, sample_idle;
} ALIGNED(64);

#define CPU_OFF_CURRENT     8
#define CPU_OFF_SYSCALL_RSP 16
#define CPU_OFF_USER_RSP    24

extern struct cpu cpus[MAX_CPUS];
extern int cpu_count;             /* CPUs brought online */

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(c));
    return c;
}
/* a single GS-relative load, so preemption and migration cannot split it */
static inline struct thread *get_current_thread(void) {
    struct thread *t;
    __asm__ volatile("movq %%gs:8, %0" : "=r"(t));
    return t;
}
#define current_thread get_current_thread()

void percpu_init(struct cpu *c, int id);   /* fill in c and load it into GS on the calling CPU */
