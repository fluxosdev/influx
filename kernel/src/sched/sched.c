#include "sched.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "fs.h"
#include "simd.h"

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void enter_user_mode(uint64_t rip, uint64_t rsp);

struct process *kernel_process;
uint64_t sched_context_switches;

static LIST_HEAD(run_queue);
static LIST_HEAD(sleep_list);
static LIST_HEAD(all_threads);
static LIST_HEAD(all_processes);
static int next_tid = 1, next_pid = 1;
/* sched_lock: the run queue, the sleep list, wait queues and thread state.
 * schedule() holds it across context_switch and the thread switched to drops
 * it (finish_switch), so no other CPU can pick a thread up before its
 * registers are saved. Taken with interrupts already off. */
static spinlock_t sched_lock = SPINLOCK_INIT;
static volatile uint64_t idle_cpus;   /* bit per CPU halted in its idle loop */
static spinlock_t exit_lock = SPINLOCK_INIT;   /* process->exited and its exit_wq */
#define TIME_SLICE 10
#define STACK_CANARY 0x57AC4CA9A2E57AC4ULL

/* ---- procs lock: the process and thread lists, and process lifetime ----
 * Recursive, since heap accounting looks processes up from inside walks that
 * allocate. Order: procs lock, then sched_lock. */
static spinlock_t procs_spin = SPINLOCK_INIT;
static int procs_owner = -1, procs_depth;

void procs_lock(void) {
    uint64_t f = irq_save();
    int me = this_cpu()->id;
    if (__atomic_load_n(&procs_owner, __ATOMIC_RELAXED) == me) { procs_depth++; return; }
    spin_acquire(&procs_spin);
    procs_spin.flags = f;
    procs_owner = me;
    procs_depth = 1;
}
void procs_unlock(void) {
    if (--procs_depth) return;
    procs_owner = -1;
    spin_unlock(&procs_spin);
}

/* Run whatever an interrupt woke right away instead of at the next tick: a
 * USB mass storage command waits for three completions, so this doubles its
 * throughput. sti; hlt cannot lose a wakeup: sti takes effect after the hlt.
 * The idle bit is set before looking at the run queue and make_ready() looks
 * at the bits after queueing, so either this CPU sees the work or gets an IPI. */
static void idle_loop(void *arg) {
    uint64_t bit = 1ULL << this_cpu()->id;
    for (;;) {
        cli();
        __atomic_fetch_or(&idle_cpus, bit, __ATOMIC_SEQ_CST);
        if (!list_empty(&run_queue)) {
            __atomic_fetch_and(&idle_cpus, ~bit, __ATOMIC_SEQ_CST);
            schedule();
            continue;
        }
        sti(); hlt();
        __atomic_fetch_and(&idle_cpus, ~bit, __ATOMIC_SEQ_CST);
    }
}

static void thread_free(struct thread *t) {
    if (t->kstack_base) kfree((void *)t->kstack_base);
    kfree(t->fpu_mem);
    kfree(t);
}

/* Second half of a context switch, on the thread switched to: release the
 * previous thread to the other CPUs and free it if it exited. */
static void finish_switch(void) {
    struct cpu *c = this_cpu();
    struct thread *prev = c->prev, *dead = NULL;
    c->prev = NULL;
    if (prev) {
        prev->on_cpu = false;
        if (prev->state == THREAD_ZOMBIE) dead = prev;
    }
    spin_release(&sched_lock);
    if (dead) thread_free(dead);
}

static void thread_trampoline(void) {
    /* Runs on the new thread's stack after the first context switch. */
    finish_switch();
    struct thread *t = current_thread;
    sti();
    if (t->is_user) enter_user_mode(t->user_rip, t->user_rsp);
    t->entry(t->arg);
    thread_exit(0);
}

/* wake an idle CPU (not this one) for newly queued work; true if there was one */
static bool kick_idle_cpu(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the queued thread is visible before we read the bits */
    uint64_t m = idle_cpus & ~(1ULL << this_cpu()->id);
    if (!m) return false;
    int cpu = __builtin_ctzll(m);
    if (__atomic_fetch_and(&idle_cpus, ~(1ULL << cpu), __ATOMIC_SEQ_CST) & (1ULL << cpu)) smp_send_resched(cpu);
    return true;
}

/* sched_lock held: t may run. A thread still on its CPU, on its way into
 * schedule() after deciding to wait, only has its state flipped; schedule()
 * sees READY and carries on. True if an idle CPU was woken to run it. */
static bool make_ready(struct thread *t) {
    t->state = THREAD_READY;
    if (t->on_cpu) return false;
    list_add_tail(&t->sched_link, &run_queue);
    return kick_idle_cpu();
}

static struct thread *alloc_thread(struct process *p, const char *name) {
    struct thread *t = kzalloc(sizeof *t);
    t->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    strlcpy(t->name, name, sizeof t->name);
    t->proc = p;
    t->kstack_base = (uint64_t)kmalloc(KSTACK_SIZE);
    t->kstack_top = (t->kstack_base + KSTACK_SIZE) & ~0xFULL;
    *(uint64_t *)t->kstack_base = STACK_CANARY;
    t->state = THREAD_READY;
    t->slice = TIME_SLICE;
    /* initial frame consumed by context_switch: 6 callee-saved regs + return address.
     * Start 8 bytes below the top so the trampoline runs with the SysV stack
     * alignment a 'call' would have produced (SSE code relies on it). */
    uint64_t *sp = (uint64_t *)(t->kstack_top - 8);
    *--sp = (uint64_t)thread_trampoline;
    for (int i = 0; i < 6; i++) *--sp = 0;
    t->rsp = (uint64_t)sp;
    t->fpu = fpu_state_alloc(&t->fpu_mem);     /* sane initial FPU/SSE/AVX state */
    list_init(&t->sched_link);
    procs_lock();
    list_add_tail(&t->all_link, &all_threads);
    list_add_tail(&t->proc_link, &p->threads);
    p->nthreads++;
    procs_unlock();
    return t;
}

static void start_thread(struct thread *t) {
    uint64_t f = irq_save();
    spin_acquire(&sched_lock);
    make_ready(t);
    spin_release(&sched_lock);
    irq_restore(f);
}

struct thread *thread_create(struct process *p, const char *name, void (*entry)(void *), void *arg) {
    struct thread *t = alloc_thread(p ? p : kernel_process, name);
    t->entry = entry; t->arg = arg;
    start_thread(t);
    return t;
}

struct thread *thread_create_user(struct process *p, const char *name, uint64_t rip, uint64_t rsp) {
    struct thread *t = alloc_thread(p, name);
    t->is_user = true; t->user_rip = rip; t->user_rsp = rsp;
    start_thread(t);
    return t;
}

struct process *process_create(const char *name, bool user) {
    struct process *p = kzalloc(sizeof *p);
    p->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
    strlcpy(p->name, name, sizeof p->name);
    p->is_user = user;
    p->pml4 = user ? vmm_new_pml4() : 0;
    p->app_id = -1;
    p->start_time = system_ticks;
    strcpy(p->cwd, "/");
    list_init(&p->threads);
    waitq_init(&p->exit_wq);
    p->parent = current_thread ? current_thread->proc : NULL;
    procs_lock();
    list_add_tail(&p->all_link, &all_processes);
    procs_unlock();
    return p;
}

void sched_init(void) {
    kernel_process = process_create("kernel", false);
    /* Bootstrap: the currently running code becomes the "init" thread */
    struct thread *boot = kzalloc(sizeof *boot);
    boot->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED);
    strlcpy(boot->name, "init", sizeof boot->name);
    boot->proc = kernel_process;
    boot->state = THREAD_RUNNING;
    boot->on_cpu = true;
    boot->slice = TIME_SLICE;
    boot->kstack_base = 0;
    boot->fpu = fpu_state_alloc(&boot->fpu_mem);
    list_init(&boot->sched_link);
    list_add_tail(&boot->all_link, &all_threads);
    list_add_tail(&boot->proc_link, &kernel_process->threads);
    kernel_process->nthreads++;
    struct cpu *c = this_cpu();
    c->current = boot;
    sched_create_idle(c);
    kprintf("sched: initialised (init tid %d, idle tid %d)\n", boot->tid, c->idle->tid);
}

void sched_create_idle(struct cpu *c) {
    char name[16]; snprintf(name, sizeof name, "idle%d", c->id);
    c->idle = alloc_thread(kernel_process, name);
    c->idle->entry = idle_loop;
}

/* An AP's boot context is dropped here: it becomes this CPU's idle thread. */
void sched_ap_enter(void) {
    struct cpu *c = this_cpu();
    struct thread *idle = c->idle;
    cli();
    spin_acquire(&sched_lock);
    idle->state = THREAD_RUNNING;
    idle->on_cpu = true;
    c->current = idle;
    c->prev = NULL;
    tss_set_kernel_stack(idle->kstack_top);
    c->syscall_rsp = idle->kstack_top;
    fpu_restore(idle->fpu);
    uint64_t discard;
    context_switch(&discard, idle->rsp);   /* to thread_trampoline, which drops sched_lock */
    __builtin_unreachable();
}

static struct thread *pick_next(struct cpu *c) {
    while (!list_empty(&run_queue)) {
        struct thread *t = list_first_entry(&run_queue, struct thread, sched_link);
        list_del(&t->sched_link);
        if (t->state == THREAD_READY) return t;
    }
    return c->idle;
}

/* Must be called with interrupts disabled. */
void schedule(void) {
    struct cpu *c = this_cpu();
    struct thread *prev = c->current;
    /* a thread that ran off the bottom of its stack has been overwriting the
     * allocation below it: stop here rather than let that spread */
    if (prev->kstack_base && *(uint64_t *)prev->kstack_base != STACK_CANARY)
        panic("kernel stack overflow in thread '%s' (%d KiB stack)", prev->name, (int)(KSTACK_SIZE / 1024));
    spin_acquire(&sched_lock);
    if (prev->state == THREAD_READY) {   /* woken between deciding to wait and getting here */
        prev->state = THREAD_RUNNING;
        spin_release(&sched_lock);
        return;
    }
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        if (prev != c->idle) list_add_tail(&prev->sched_link, &run_queue);
    }
    struct thread *next = pick_next(c);
    next->state = THREAD_RUNNING;
    next->slice = TIME_SLICE;
    if (next == prev) { spin_release(&sched_lock); return; }
    next->on_cpu = true;
    /* save the vector registers before current_thread changes, so nothing in
     * between can mistake them for the next thread's (see simd_ok) */
    fpu_save(prev->fpu);
    __atomic_fetch_add(&sched_context_switches, 1, __ATOMIC_RELAXED);
    c->current = next;
    c->prev = prev;
    tss_set_kernel_stack(next->kstack_top);
    c->syscall_rsp = next->kstack_top;
    vmm_switch(next->proc->pml4 ? next->proc->pml4 : vmm_kernel_pml4());
    fpu_restore(next->fpu);
    context_switch(&prev->rsp, next->rsp);
    /* back on 'prev' stack, possibly on another CPU */
    finish_switch();
}

void thread_yield(void) { uint64_t f = irq_save(); schedule(); irq_restore(f); }

/* sched_lock held: a wakeup aimed at t while it was not waiting (a kill
 * racing with its last check) is kept for its next wait, which then returns
 * at once instead of sleeping through it. One-shot, like a spurious wakeup. */
static bool take_wakeup(struct thread *t) {
    if (!t->wake_pending) return false;
    t->wake_pending = false;
    return true;
}

void thread_block(void) {
    uint64_t f = irq_save();
    spin_acquire(&sched_lock);
    if (!take_wakeup(current_thread)) current_thread->state = THREAD_BLOCKED;
    spin_release(&sched_lock);
    schedule();
    irq_restore(f);
}

/* sched_lock held. A blocked thread may sit on a wait queue and a sleeping
 * one on the sleep list: take it off before queueing it to run (list_del
 * leaves an unlinked entry pointing at itself, so this is safe either way).
 * A timed waiter keeps waiting_on so waitq_wait_timeout reports a timeout. */
static void unblock_locked(struct thread *t) {
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        list_del(&t->sched_link);
        if (t->state == THREAD_BLOCKED) t->waiting_on = NULL;
        make_ready(t);
    } else if (t->state != THREAD_ZOMBIE) t->wake_pending = true;
}

void thread_unblock(struct thread *t) {
    uint64_t f = irq_save();
    spin_acquire(&sched_lock);
    unblock_locked(t);
    spin_release(&sched_lock);
    irq_restore(f);
}

/* sched_lock held; keeps sleep_list sorted by wake time */
static void sleep_insert(struct thread *t) {
    struct thread *pos;
    list_for_each_entry(pos, &sleep_list, sched_link)
        if (pos->wake_at > t->wake_at) { list_add_tail(&t->sched_link, &pos->sched_link); return; }
    list_add_tail(&t->sched_link, &sleep_list);
}

void thread_sleep_ms(uint64_t ms) {
    if (ms == 0) { thread_yield(); return; }
    uint64_t f = irq_save();
    struct thread *t = current_thread;
    spin_acquire(&sched_lock);
    if (!take_wakeup(t)) {
        t->wake_at = system_ticks + ms;
        t->state = THREAD_SLEEPING;
        sleep_insert(t);
    }
    spin_release(&sched_lock);
    schedule();
    irq_restore(f);
}

bool thread_should_exit(void) { return current_thread->kill_pending; }

/* The last thread out releases the process's resources (which may block),
 * leaves its address space and only then, with every other thread already
 * unlinked, marks it exited: from there the reaper on another CPU may free
 * the page tables and the process. */
void thread_exit(int code) {
    struct thread *t = current_thread;
    struct process *p = t->proc;
    t->exit_code = code;
    procs_lock();
    bool last = --p->nthreads == 0 && p != kernel_process;
    if (!last) { list_del(&t->all_link); list_del(&t->proc_link); }
    procs_unlock();
    if (last) {
        if (!p->exit_code) p->exit_code = code;
        for (int i = 0; i < MAX_FDS; i++) if (p->fds[i]) { vfs_close(p->fds[i]); p->fds[i] = NULL; }
        process_close_windows(p);
        cli();
        vmm_switch(vmm_kernel_pml4());
        procs_lock();
        list_del(&t->all_link);
        list_del(&t->proc_link);
        procs_unlock();
        spin_lock(&exit_lock);
        p->exited = true;
        waitq_wake_all(&p->exit_wq);
        spin_unlock(&exit_lock);
    }
    cli();
    spin_acquire(&sched_lock);
    t->state = THREAD_ZOMBIE;
    spin_release(&sched_lock);
    schedule();
    panic("thread_exit returned");
}

void thread_kill(struct thread *t) {
    t->kill_pending = true;
    thread_unblock(t);
}

/* procs lock held: mark every thread of p (but the caller) killed and wake it */
static void kill_threads(struct process *p) {
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link) if (t != current_thread) {
        t->kill_pending = true;
        spin_acquire(&sched_lock);
        unblock_locked(t);
        spin_release(&sched_lock);
    }
}

void process_exit(int code) {
    struct process *p = current_thread->proc;
    p->exit_code = code;
    /* kill sibling threads; they exit when they next run */
    procs_lock();
    kill_threads(p);
    procs_unlock();
    thread_exit(code);
}

static struct process *find_locked(int pid) {
    struct process *p;
    list_for_each_entry(p, &all_processes, all_link) if (p->pid == pid) return p;
    return NULL;
}
struct process *process_find(int pid) {
    procs_lock();
    struct process *p = find_locked(pid);
    procs_unlock();
    return p;
}

struct thread *thread_find(int tid) {
    struct thread *t, *r = NULL;
    procs_lock();
    list_for_each_entry(t, &all_threads, all_link) if (t->tid == tid) { r = t; break; }
    procs_unlock();
    return r;
}

/* waits for anyone walking the process list (they hold the procs lock) */
static void process_free(struct process *p) {
    procs_lock();
    list_del(&p->all_link);
    procs_unlock();
    if (p->pml4) vmm_free_pml4(p->pml4);
    kfree(p);
}

/* Only one caller may claim (and free) a process; user processes may only reap user processes. */
int process_wait(int pid, int *status) {
    struct process *self = current_thread->proc;
    procs_lock();
    struct process *p = find_locked(pid);
    bool ok = p && p != kernel_process && p != self && !p->reaping && (!self->is_user || p->is_user);
    if (ok) p->reaping = true;
    procs_unlock();
    if (!ok) return -1;
    spin_lock(&exit_lock);
    while (!p->exited && !current_thread->kill_pending) waitq_wait(&p->exit_wq, &exit_lock);
    bool done = p->exited;
    if (!done) p->reaping = false;
    spin_unlock(&exit_lock);
    if (!done) return -1;
    if (status) *status = p->exit_code;
    process_free(p);
    return pid;
}

bool process_kill(int pid, int sig) {
    procs_lock();
    struct process *p = find_locked(pid);
    if (!p || p == kernel_process) { procs_unlock(); return false; }
    if (!p->exited) {
        p->exit_signal = sig;
        p->exit_code = 128 + sig;
        kill_threads(p);
    }
    procs_unlock();
    return true;
}

int process_list(struct process **out, int max) {
    int n = 0; struct process *p;
    procs_lock();
    list_for_each_entry(p, &all_processes, all_link) if (n < max) out[n++] = p;
    procs_unlock();
    return n;
}
int thread_list(struct thread **out, int max) {
    int n = 0; struct thread *t;
    procs_lock();
    list_for_each_entry(t, &all_threads, all_link) if (n < max) out[n++] = t;
    procs_unlock();
    return n;
}

static uint64_t last_sample_ticks;
void sched_sample_cpu(void) {
    struct thread *t;
    procs_lock();
    uint64_t now = system_ticks, span = now - last_sample_ticks;
    if (span == 0) span = 1;
    list_for_each_entry(t, &all_threads, all_link) {
        t->cpu_percent = (int)((t->cpu_ticks - t->cpu_sample) * 100 / span);
        t->cpu_sample = t->cpu_ticks;
    }
    for (int i = 0; i < cpu_count; i++) {
        struct cpu *c = &cpus[i];
        uint64_t ticks = c->ticks - c->sample_ticks, idle = c->idle_ticks - c->sample_idle;
        c->load_percent = ticks ? (int)(100 - MIN(idle, ticks) * 100 / ticks) : 0;
        c->sample_ticks += ticks; c->sample_idle += idle;
    }
    last_sample_ticks = now;
    procs_unlock();
}
/* averaged over the CPUs, so 100 - this is the machine's overall load */
int sched_idle_percent(void) {
    int sum = 0;
    for (int i = 0; i < cpu_count; i++) sum += 100 - cpus[i].load_percent;
    return cpu_count ? sum / cpu_count : 100;
}
int sched_cpu_load(int cpu) { return cpu >= 0 && cpu < cpu_count ? cpus[cpu].load_percent : 0; }

/* Called from each CPU's timer interrupt every millisecond. */
void sched_tick(struct regs *r) {
    struct cpu *c = this_cpu();
    struct thread *cur = c->current;
    cur->cpu_ticks++;
    c->ticks++;
    if (cur == c->idle) c->idle_ticks++;
    bool resched = --cur->slice <= 0;
    if (c->id == 0) {   /* the BSP wakes sleepers for everyone */
        bool unclaimed = false;
        spin_acquire(&sched_lock);
        while (!list_empty(&sleep_list)) {
            struct thread *t = list_first_entry(&sleep_list, struct thread, sched_link);
            if (t->wake_at > system_ticks) break;
            list_del(&t->sched_link);
            if (!make_ready(t)) unclaimed = true;
        }
        spin_release(&sched_lock);
        if (unclaimed) resched = true;   /* no idle CPU took it: run it here now */
    }
    /* A user thread with a pending kill dies here before returning to user mode */
    if (cur->kill_pending && cur->is_user && (r->cs & 3)) { lapic_eoi(); sti(); process_exit(cur->proc->exit_code); }
    lapic_eoi();
    /* the idle loop looks at the run queue itself once the interrupt returns */
    if (resched && cur != c->idle) schedule();
}

void sched_start(void) {
    /* The init thread simply becomes a normal thread; nothing else to do. */
    for (;;) { thread_sleep_ms(1000); }
}

/* ---- wait queues ----
 * A waiter checks its condition under 'held' and queues itself before
 * dropping it; a waker changes the condition under the same lock, so the wake
 * (inside or after it) always finds the waiter queued. */
void waitq_init(struct waitq *q) { list_init(&q->waiters); }
static inline void waitq_lazy(struct waitq *q) { if (!q->waiters.next) list_init(&q->waiters); }

/* drop 'held', switch away, take it back (with the interrupt state its holder saved) */
static void wait_and_relock(spinlock_t *held) {
    uint64_t hf = held->flags;
    spin_release(held);
    schedule();
    spin_acquire(held);
    held->flags = hf;
}

void waitq_wait(struct waitq *q, spinlock_t *held) {
    uint64_t f = irq_save();
    struct thread *t = current_thread;
    spin_acquire(&sched_lock);
    waitq_lazy(q);
    if (take_wakeup(t)) { spin_release(&sched_lock); irq_restore(f); return; }
    t->state = THREAD_BLOCKED;
    t->waiting_on = q;
    list_add_tail(&t->sched_link, &q->waiters);
    spin_release(&sched_lock);
    wait_and_relock(held);
    t->waiting_on = NULL;
    irq_restore(f);
}

bool waitq_wait_timeout(struct waitq *q, spinlock_t *held, uint64_t ms) {
    uint64_t f = irq_save();
    struct thread *t = current_thread;
    spin_acquire(&sched_lock);
    waitq_lazy(q);
    if (take_wakeup(t)) { spin_release(&sched_lock); irq_restore(f); return true; }
    t->wake_at = system_ticks + ms;
    t->state = THREAD_SLEEPING;
    t->waiting_on = q;
    /* a sleeping waiter lives on the sleep list; the wait queue remembers it via waiting_on */
    sleep_insert(t);
    spin_release(&sched_lock);
    wait_and_relock(held);
    bool woken = t->waiting_on == NULL;   /* cleared by waker */
    t->waiting_on = NULL;
    irq_restore(f);
    return woken;
}

/* sched_lock held */
static void waitq_wake_sleepers(struct waitq *q, bool all) {
    /* wake timed waiters (which sit on the sleep list) */
    struct thread *t, *n;
    list_for_each_entry_safe(t, n, &sleep_list, sched_link) {
        if (t->waiting_on == q) {
            t->waiting_on = NULL;
            list_del(&t->sched_link);
            make_ready(t);
            if (!all) return;
        }
    }
}

void waitq_wake_one(struct waitq *q) {
    uint64_t f = irq_save();
    spin_acquire(&sched_lock);
    waitq_lazy(q);
    if (!list_empty(&q->waiters)) {
        struct thread *t = list_first_entry(&q->waiters, struct thread, sched_link);
        list_del(&t->sched_link);
        t->waiting_on = NULL;
        make_ready(t);
    } else waitq_wake_sleepers(q, false);
    spin_release(&sched_lock);
    irq_restore(f);
}

void waitq_wake_all(struct waitq *q) {
    uint64_t f = irq_save();
    spin_acquire(&sched_lock);
    waitq_lazy(q);
    while (!list_empty(&q->waiters)) {
        struct thread *t = list_first_entry(&q->waiters, struct thread, sched_link);
        list_del(&t->sched_link);
        t->waiting_on = NULL;
        make_ready(t);
    }
    waitq_wake_sleepers(q, true);
    spin_release(&sched_lock);
    irq_restore(f);
}

/* ---- mutex ---- */
void mutex_init(struct mutex *m) { m->lock = (spinlock_t)SPINLOCK_INIT; m->locked = false; m->owner = NULL; waitq_init(&m->wq); }
void mutex_lock(struct mutex *m) {
    spin_lock(&m->lock);
    while (m->locked) waitq_wait(&m->wq, &m->lock);
    m->locked = true; m->owner = current_thread;
    spin_unlock(&m->lock);
}
bool mutex_trylock(struct mutex *m) {
    spin_lock(&m->lock);
    bool ok = !m->locked;
    if (ok) { m->locked = true; m->owner = current_thread; }
    spin_unlock(&m->lock);
    return ok;
}
void mutex_unlock(struct mutex *m) {
    spin_lock(&m->lock);
    m->locked = false; m->owner = NULL;
    waitq_wake_one(&m->wq);
    spin_unlock(&m->lock);
}

/* ---- semaphore ---- */
void sem_init(struct semaphore *s, int count) { s->lock = (spinlock_t)SPINLOCK_INIT; s->count = count; waitq_init(&s->wq); }
void sem_wait(struct semaphore *s) {
    spin_lock(&s->lock);
    while (s->count <= 0) waitq_wait(&s->wq, &s->lock);
    s->count--;
    spin_unlock(&s->lock);
}
bool sem_wait_timeout(struct semaphore *s, uint64_t ms) {
    uint64_t deadline = system_ticks + ms;
    spin_lock(&s->lock);
    while (s->count <= 0) {
        if (system_ticks >= deadline) { spin_unlock(&s->lock); return false; }
        waitq_wait_timeout(&s->wq, &s->lock, deadline - system_ticks);
    }
    s->count--;
    spin_unlock(&s->lock);
    return true;
}
bool sem_trywait(struct semaphore *s) {
    spin_lock(&s->lock);
    bool ok = s->count > 0;
    if (ok) s->count--;
    spin_unlock(&s->lock);
    return ok;
}
void sem_post(struct semaphore *s) {
    spin_lock(&s->lock);
    s->count++;
    waitq_wake_one(&s->wq);
    spin_unlock(&s->lock);
}
