#include "arch.h"
#include "power.h"
#include "sched.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "simd.h"
#include <limine.h>

/* Symmetric multiprocessing: Limine starts the APs and parks them; smp_init()
 * hands each one a struct cpu, and ap_entry() gives it this kernel's page
 * tables, descriptor tables, CPU features, LAPIC timer and syscall MSRs before
 * it drops into its idle thread and takes threads from the shared run queue. */

__attribute__((used, section(".limine_requests"))) static volatile struct limine_smp_request smp_request = { .id = LIMINE_SMP_REQUEST, .revision = 0, .flags = 0 };
__attribute__((used, section(".limine_requests"))) static volatile struct limine_executable_cmdline_request cmdline_request = { .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0 };

struct cpu cpus[MAX_CPUS];
int cpu_count = 1;
static volatile uint64_t online_mask = 1;
static volatile int ap_claimed, ap_online;   /* ap_claimed < 0: smp_init stopped waiting */

void percpu_init(struct cpu *c, int id) {
    c->self = c;
    c->id = id;
    wrmsr(0xC0000101, (uint64_t)c);   /* GS_BASE */
    wrmsr(0xC0000102, 0);             /* KERNEL_GS_BASE: user GS, swapped in on the way out */
}

/* ---- IPIs ---- */

static void resched_irq(struct regs *r, void *ctx) { lapic_eoi(); }   /* waking from hlt is the point */

void smp_send_resched(int cpu) { lapic_send_ipi(cpus[cpu].lapic_id, VEC_RESCHED); }

static void halt_irq(struct regs *r, void *ctx) { for (;;) { cli(); hlt(); } }

static void ipi_mask(uint64_t m, uint8_t vector) {
    for (; m; m &= m - 1) lapic_send_ipi(cpus[__builtin_ctzll(m)].lapic_id, vector);
}

void smp_halt_others(void) { ipi_mask(online_mask & ~(1ULL << this_cpu()->id), VEC_HALT); }

/* ---- TLB shootdown ----
 * One request at a time (tlb_lock). Targets answer from the IPI or, when
 * spinning on a lock with interrupts off, from spin_acquire(). */
volatile uint64_t tlb_pending_cpus;
static struct { uint64_t virt, pages; } tlb_req;
static spinlock_t tlb_lock = SPINLOCK_INIT;

void tlb_flush_local(uint64_t virt, uint64_t pages) {
    if (pages <= 64) { for (uint64_t i = 0; i < pages; i++) invlpg(virt + i * PAGE_SIZE); return; }
    uint64_t cr4 = read_cr4();
    if (cr4 & (1 << 7)) { write_cr4(cr4 & ~(1ULL << 7)); write_cr4(cr4); }   /* toggling PGE drops global entries too */
    else write_cr3(read_cr3());
}

void tlb_service(void) {
    uint64_t bit = 1ULL << this_cpu()->id;
    if (!(__atomic_load_n(&tlb_pending_cpus, __ATOMIC_ACQUIRE) & bit)) return;
    tlb_flush_local(tlb_req.virt, tlb_req.pages);
    __atomic_fetch_and(&tlb_pending_cpus, ~bit, __ATOMIC_RELEASE);
}

static void tlb_irq(struct regs *r, void *ctx) { tlb_service(); lapic_eoi(); }

void tlb_shootdown(uint64_t virt, uint64_t pages) {
    uint64_t f = irq_save();
    tlb_flush_local(virt, pages);
    uint64_t me = 1ULL << this_cpu()->id, targets = online_mask & ~me;
    if (targets) {
        spin_acquire(&tlb_lock);
        tlb_req.virt = virt; tlb_req.pages = pages;
        __atomic_store_n(&tlb_pending_cpus, targets, __ATOMIC_SEQ_CST);
        ipi_mask(targets, VEC_TLB);
        while (__atomic_load_n(&tlb_pending_cpus, __ATOMIC_ACQUIRE)) pause();
        spin_release(&tlb_lock);
    }
    irq_restore(f);
}

/* ---- AP bring-up ---- */

void syscall_init_cpu(void);

static void ap_entry(struct limine_smp_info *info) {
    /* claim the next struct cpu, unless smp_init has given up on us */
    int v = __atomic_load_n(&ap_claimed, __ATOMIC_ACQUIRE);
    do { if (v < 0) for (;;) { cli(); hlt(); } }
    while (!__atomic_compare_exchange_n(&ap_claimed, &v, v + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE));
    struct cpu *c = &cpus[v + 1];
    c->lapic_id = info->lapic_id;
    write_cr3(vmm_kernel_pml4());   /* Limine's page tables lack our later mappings */
    tlb_flush_local(0, ~0ULL);      /* and its global entries survive a CR3 load */
    percpu_init(c, v + 1);
    gdt_init();
    idt_load();
    cpu_init_ap();
    lapic_init_ap();
    syscall_init_cpu();
    lapic_timer_start(1000);
    cpupower_init();
    __atomic_fetch_or(&online_mask, 1ULL << c->id, __ATOMIC_SEQ_CST);
    c->online = true;
    __atomic_fetch_add(&ap_online, 1, __ATOMIC_SEQ_CST);
    sched_ap_enter();
}

bool kernel_cmdline_has(const char *word) {
    struct limine_executable_cmdline_response *r = cmdline_request.response;
    if (!r || !r->cmdline) return false;
    size_t n = strlen(word);
    for (const char *p = r->cmdline; (p = strstr(p, word)); p += n)
        if ((p == r->cmdline || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}

static void pool_init(void);

void smp_init(void) {
    struct cpu *bsp = this_cpu();
    bsp->lapic_id = lapic_id();
    bsp->online = true;
    irq_register(VEC_RESCHED, resched_irq, NULL);
    irq_register(VEC_TLB, tlb_irq, NULL);
    irq_register(VEC_HALT, halt_irq, NULL);
    struct limine_smp_response *r = smp_request.response;
    if (!r || r->cpu_count < 2) { kprintf("smp: single processor\n"); return; }
    if (kernel_cmdline_has("nosmp")) { kprintf("smp: %lu CPUs, 'nosmp' given: using one\n", r->cpu_count); return; }
    int want = 0;
    for (uint64_t i = 0; i < r->cpu_count && 1 + want < MAX_CPUS; i++) {
        if (r->cpus[i]->lapic_id == r->bsp_lapic_id) continue;
        cpus[1 + want].id = 1 + want;
        sched_create_idle(&cpus[1 + want]);
        want++;
    }
    if (1 + want < (int)r->cpu_count) kprintf("smp: only %d CPUs supported\n", MAX_CPUS);
    for (uint64_t i = 0, n = 0; i < r->cpu_count && n < (uint64_t)want; i++) {
        if (r->cpus[i]->lapic_id == r->bsp_lapic_id) continue;
        __atomic_store_n(&r->cpus[i]->goto_address, ap_entry, __ATOMIC_SEQ_CST);   /* the AP leaves Limine's spin loop */
        n++;
    }
    uint64_t deadline = rdtsc() + cpu_tsc_hz();   /* one second */
    while (__atomic_load_n(&ap_claimed, __ATOMIC_SEQ_CST) < want && rdtsc() < deadline) pause();
    int up = __atomic_exchange_n(&ap_claimed, -1, __ATOMIC_SEQ_CST);
    /* shootdowns from here on must reach every CPU running kernel code */
    while (__atomic_load_n(&ap_online, __ATOMIC_SEQ_CST) < up) pause();
    cpu_count = 1 + up;
    if (up < want) kprintf("smp: %d of %d application processors did not start\n", want - up, want);
    kprintf("smp: %d CPUs online (BSP lapic %u)\n", cpu_count, bsp->lapic_id);
    pool_init();
}

/* ---- parallel for ----
 * One worker thread per AP; the caller takes part too. One job at a time:
 * a caller that finds the pool busy (including a nested call) runs inline. */
struct pjob { void (*fn)(void *, int); void *ctx; int n, next, active; struct waitq wq; };
static spinlock_t pool_lock = SPINLOCK_INIT;
static struct pjob *pool_job;
static struct semaphore pool_sem;
static struct mutex pool_busy = MUTEX_INIT;
static int nworkers;

static void run_items(struct pjob *j) {
    int i;
    while ((i = __atomic_fetch_add(&j->next, 1, __ATOMIC_RELAXED)) < j->n) j->fn(j->ctx, i);
}

static void worker_main(void *arg) {
    for (;;) {
        sem_wait(&pool_sem);
        spin_lock(&pool_lock);
        struct pjob *j = pool_job;
        if (j) j->active++;
        spin_unlock(&pool_lock);
        if (!j) continue;   /* a late wakeup: that job is done already */
        run_items(j);
        spin_lock(&pool_lock);
        if (--j->active == 0) waitq_wake_all(&j->wq);
        spin_unlock(&pool_lock);
    }
}

static void pool_init(void) {
    sem_init(&pool_sem, 0);
    for (int i = 1; i < cpu_count; i++) {
        char name[16]; snprintf(name, sizeof name, "worker%d", i);
        thread_create(kernel_process, name, worker_main, NULL);
    }
    nworkers = cpu_count - 1;
}

int smp_workers(void) { return nworkers + 1; }

void smp_parallel_for(int n, void (*fn)(void *ctx, int i), void *ctx) {
    if (n <= 0) return;
    if (n == 1 || !nworkers || !mutex_trylock(&pool_busy)) { for (int i = 0; i < n; i++) fn(ctx, i); return; }
    struct pjob j = { fn, ctx, n, 0, 0, { { 0, 0 } } };
    waitq_init(&j.wq);
    spin_lock(&pool_lock);
    pool_job = &j;
    spin_unlock(&pool_lock);
    for (int k = MIN(nworkers, n - 1); k > 0; k--) sem_post(&pool_sem);
    run_items(&j);
    spin_lock(&pool_lock);
    pool_job = NULL;   /* no one joins from here; wait out those who did */
    while (j.active) waitq_wait(&j.wq, &pool_lock);
    spin_unlock(&pool_lock);
    mutex_unlock(&pool_busy);
}
