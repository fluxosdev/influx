#include "sched.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "fs.h"
#include "simd.h"

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void enter_user_mode(uint64_t rip, uint64_t rsp);

struct thread *current_thread;
struct process *kernel_process;
uint64_t sched_context_switches;
uint64_t kernel_rsp_for_syscall;   /* read by the syscall entry stub */

static LIST_HEAD(run_queue);
static LIST_HEAD(sleep_list);
static LIST_HEAD(all_threads);
static LIST_HEAD(all_processes);
static struct thread *idle_thread;
static struct thread *reap_thread;
static int next_tid = 1, next_pid = 1;
static uint64_t idle_ticks, idle_sample;
static spinlock_t sched_lock = SPINLOCK_INIT;   /* protects the lists */
#define TIME_SLICE 10

static void idle_loop(void *arg) { for (;;) { sti(); hlt(); } }

static void thread_free(struct thread *t) {
    if (t->kstack_base) kfree((void *)t->kstack_base);
    kfree(t->fpu_mem);
    kfree(t);
}

static void thread_trampoline(void) {
    /* Runs on the new thread's stack after the first context switch. */
    struct thread *t = current_thread;
    if (reap_thread) { struct thread *r = reap_thread; reap_thread = NULL; thread_free(r); }
    sti();
    if (t->is_user) enter_user_mode(t->user_rip, t->user_rsp);
    t->entry(t->arg);
    thread_exit(0);
}

static struct thread *alloc_thread(struct process *p, const char *name) {
    struct thread *t = kzalloc(sizeof *t);
    t->tid = next_tid++;
    strlcpy(t->name, name, sizeof t->name);
    t->proc = p;
    t->kstack_base = (uint64_t)kmalloc(KSTACK_SIZE);
    t->kstack_top = (t->kstack_base + KSTACK_SIZE) & ~0xFULL;
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
    spin_lock(&sched_lock);
    list_add_tail(&t->all_link, &all_threads);
    list_add_tail(&t->proc_link, &p->threads);
    spin_unlock(&sched_lock);
    return t;
}

struct thread *thread_create(struct process *p, const char *name, void (*entry)(void *), void *arg) {
    struct thread *t = alloc_thread(p ? p : kernel_process, name);
    t->entry = entry; t->arg = arg;
    spin_lock(&sched_lock);
    list_add_tail(&t->sched_link, &run_queue);
    spin_unlock(&sched_lock);
    return t;
}

struct thread *thread_create_user(struct process *p, const char *name, uint64_t rip, uint64_t rsp) {
    struct thread *t = alloc_thread(p, name);
    t->is_user = true; t->user_rip = rip; t->user_rsp = rsp;
    spin_lock(&sched_lock);
    list_add_tail(&t->sched_link, &run_queue);
    spin_unlock(&sched_lock);
    return t;
}

struct process *process_create(const char *name, bool user) {
    struct process *p = kzalloc(sizeof *p);
    p->pid = next_pid++;
    strlcpy(p->name, name, sizeof p->name);
    p->is_user = user;
    p->pml4 = user ? vmm_new_pml4() : 0;
    p->app_id = -1;
    p->start_time = system_ticks;
    strcpy(p->cwd, "/");
    list_init(&p->threads);
    waitq_init(&p->exit_wq);
    p->parent = current_thread ? current_thread->proc : NULL;
    spin_lock(&sched_lock);
    list_add_tail(&p->all_link, &all_processes);
    spin_unlock(&sched_lock);
    return p;
}

void sched_init(void) {
    kernel_process = process_create("kernel", false);
    /* Bootstrap: the currently running code becomes the "init" thread */
    struct thread *boot = kzalloc(sizeof *boot);
    boot->tid = next_tid++;
    strlcpy(boot->name, "init", sizeof boot->name);
    boot->proc = kernel_process;
    boot->state = THREAD_RUNNING;
    boot->slice = TIME_SLICE;
    boot->kstack_base = 0;
    boot->fpu = fpu_state_alloc(&boot->fpu_mem);
    list_init(&boot->sched_link);
    list_add_tail(&boot->all_link, &all_threads);
    list_add_tail(&boot->proc_link, &kernel_process->threads);
    current_thread = boot;
    idle_thread = alloc_thread(kernel_process, "idle");
    idle_thread->entry = idle_loop;
    kprintf("sched: initialised (init tid %d, idle tid %d)\n", boot->tid, idle_thread->tid);
}

static struct thread *pick_next(void) {
    while (!list_empty(&run_queue)) {
        struct thread *t = list_first_entry(&run_queue, struct thread, sched_link);
        list_del(&t->sched_link);
        if (t->state == THREAD_READY) return t;
    }
    return idle_thread;
}

/* Must be called with interrupts disabled. */
void schedule(void) {
    struct thread *prev = current_thread;
    spin_lock(&sched_lock);
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        if (prev != idle_thread) list_add_tail(&prev->sched_link, &run_queue);
    }
    struct thread *next = pick_next();
    next->state = THREAD_RUNNING;
    next->slice = TIME_SLICE;
    spin_unlock(&sched_lock);
    if (next == prev) return;
    /* save the vector registers before current_thread changes, so nothing in
     * between can mistake them for the next thread's (see simd_ok) */
    fpu_save(prev->fpu);
    sched_context_switches++;
    current_thread = next;
    tss_set_kernel_stack(next->kstack_top);
    kernel_rsp_for_syscall = next->kstack_top;
    vmm_switch(next->proc->pml4 ? next->proc->pml4 : vmm_kernel_pml4());
    fpu_restore(next->fpu);
    if (prev->state == THREAD_ZOMBIE) reap_thread = prev;
    context_switch(&prev->rsp, next->rsp);
    /* back on 'prev' stack */
    if (reap_thread && reap_thread != current_thread) {
        struct thread *r = reap_thread; reap_thread = NULL;
        thread_free(r);
    }
}

void thread_yield(void) { uint64_t f = irq_save(); schedule(); irq_restore(f); }

void thread_block(void) {
    uint64_t f = irq_save();
    current_thread->state = THREAD_BLOCKED;
    schedule();
    irq_restore(f);
}

void thread_unblock(struct thread *t) {
    spin_lock(&sched_lock);
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        if (t->state == THREAD_SLEEPING) list_del(&t->sched_link);
        t->state = THREAD_READY;
        list_add_tail(&t->sched_link, &run_queue);
    }
    spin_unlock(&sched_lock);
}

void thread_sleep_ms(uint64_t ms) {
    if (ms == 0) { thread_yield(); return; }
    uint64_t f = irq_save();
    struct thread *t = current_thread;
    t->wake_at = system_ticks + ms;
    t->state = THREAD_SLEEPING;
    spin_lock(&sched_lock);
    /* keep sleep_list sorted by wake time */
    struct thread *pos; bool inserted = false;
    list_for_each_entry(pos, &sleep_list, sched_link) {
        if (pos->wake_at > t->wake_at) { list_add_tail(&t->sched_link, &pos->sched_link); inserted = true; break; }
    }
    if (!inserted) list_add_tail(&t->sched_link, &sleep_list);
    spin_unlock(&sched_lock);
    schedule();
    irq_restore(f);
}

bool thread_should_exit(void) { return current_thread->kill_pending; }

void thread_exit(int code) {
    cli();
    struct thread *t = current_thread;
    t->exit_code = code;
    spin_lock(&sched_lock);
    list_del(&t->all_link);
    list_del(&t->proc_link);
    bool last = list_empty(&t->proc->threads);
    spin_unlock(&sched_lock);
    if (last && t->proc != kernel_process) {
        struct process *p = t->proc;
        p->exited = true;
        if (!p->exit_code) p->exit_code = code;
        for (int i = 0; i < MAX_FDS; i++) if (p->fds[i]) { vfs_close(p->fds[i]); p->fds[i] = NULL; }
        process_close_windows(p);
        waitq_wake_all(&p->exit_wq);
    }
    t->state = THREAD_ZOMBIE;
    schedule();
    panic("thread_exit returned");
}

void thread_kill(struct thread *t) {
    t->kill_pending = true;
    thread_unblock(t);
}

void process_exit(int code) {
    struct process *p = current_thread->proc;
    p->exit_code = code;
    /* kill sibling threads; they exit when they next run */
    struct thread *t;
    spin_lock(&sched_lock);
    list_for_each_entry(t, &p->threads, proc_link) if (t != current_thread) t->kill_pending = true;
    spin_unlock(&sched_lock);
    list_for_each_entry(t, &p->threads, proc_link) if (t != current_thread) thread_unblock(t);
    thread_exit(code);
}

struct process *process_find(int pid) {
    struct process *p;
    list_for_each_entry(p, &all_processes, all_link) if (p->pid == pid) return p;
    return NULL;
}

struct thread *thread_find(int tid) {
    struct thread *t;
    list_for_each_entry(t, &all_threads, all_link) if (t->tid == tid) return t;
    return NULL;
}

static void process_free(struct process *p) {
    spin_lock(&sched_lock);
    list_del(&p->all_link);
    spin_unlock(&sched_lock);
    if (p->pml4) vmm_free_pml4(p->pml4);
    kfree(p);
}

int process_wait(int pid, int *status) {
    struct process *p = process_find(pid);
    if (!p) return -1;
    static spinlock_t wl = SPINLOCK_INIT;
    spin_lock(&wl);
    while (!p->exited) waitq_wait(&p->exit_wq, &wl);
    spin_unlock(&wl);
    /* wait until every thread has actually been reaped */
    while (!list_empty(&p->threads)) thread_yield();
    if (status) *status = p->exit_code;
    process_free(p);
    return pid;
}

bool process_kill(int pid, int sig) {
    struct process *p = process_find(pid);
    if (!p || p == kernel_process) return false;
    p->exit_signal = sig;
    p->exit_code = 128 + sig;
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link) t->kill_pending = true;
    list_for_each_entry(t, &p->threads, proc_link) thread_unblock(t);
    return true;
}

int process_list(struct process **out, int max) {
    int n = 0; struct process *p;
    spin_lock(&sched_lock);
    list_for_each_entry(p, &all_processes, all_link) if (n < max) out[n++] = p;
    spin_unlock(&sched_lock);
    return n;
}
int thread_list(struct thread **out, int max) {
    int n = 0; struct thread *t;
    spin_lock(&sched_lock);
    list_for_each_entry(t, &all_threads, all_link) if (n < max) out[n++] = t;
    spin_unlock(&sched_lock);
    return n;
}

static uint64_t last_sample_ticks;
void sched_sample_cpu(void) {
    uint64_t now = system_ticks, span = now - last_sample_ticks;
    if (span == 0) span = 1;
    struct thread *t;
    spin_lock(&sched_lock);
    list_for_each_entry(t, &all_threads, all_link) {
        t->cpu_percent = (int)((t->cpu_ticks - t->cpu_sample) * 100 / span);
        t->cpu_sample = t->cpu_ticks;
    }
    spin_unlock(&sched_lock);
    idle_sample = idle_ticks;
    last_sample_ticks = now;
}
int sched_idle_percent(void) { return idle_thread ? idle_thread->cpu_percent : 100; }

/* Called from the timer interrupt every millisecond. */
void sched_tick(struct regs *r) {
    struct thread *cur = current_thread;
    cur->cpu_ticks++;
    if (cur == idle_thread) idle_ticks++;
    bool resched = false;
    spin_lock(&sched_lock);
    while (!list_empty(&sleep_list)) {
        struct thread *t = list_first_entry(&sleep_list, struct thread, sched_link);
        if (t->wake_at > system_ticks) break;
        list_del(&t->sched_link);
        t->state = THREAD_READY;
        list_add_tail(&t->sched_link, &run_queue);
        resched = true;
    }
    spin_unlock(&sched_lock);
    if (--cur->slice <= 0) resched = true;
    /* A user thread with a pending kill dies here before returning to user mode */
    if (cur->kill_pending && cur->is_user && (r->cs & 3)) { lapic_eoi(); sti(); process_exit(cur->proc->exit_code); }
    if (resched || cur == idle_thread) { lapic_eoi(); schedule(); return; }
    lapic_eoi();
}

void sched_start(void) {
    /* The init thread simply becomes a normal thread; nothing else to do. */
    for (;;) { thread_sleep_ms(1000); }
}

/* ---- wait queues ---- */
void waitq_init(struct waitq *q) { list_init(&q->waiters); }
static inline void waitq_lazy(struct waitq *q) { if (!q->waiters.next) list_init(&q->waiters); }

void waitq_wait(struct waitq *q, spinlock_t *held) {
    uint64_t f = irq_save();
    waitq_lazy(q);
    struct thread *t = current_thread;
    t->state = THREAD_BLOCKED;
    t->waiting_on = q;
    spin_lock(&sched_lock);
    list_add_tail(&t->sched_link, &q->waiters);
    spin_unlock(&sched_lock);
    uint64_t hf = held->flags;
    __atomic_clear(&held->locked, __ATOMIC_RELEASE);
    schedule();
    while (__atomic_test_and_set(&held->locked, __ATOMIC_ACQUIRE)) pause();
    held->flags = hf;
    t->waiting_on = NULL;
    irq_restore(f);
}

bool waitq_wait_timeout(struct waitq *q, spinlock_t *held, uint64_t ms) {
    uint64_t f = irq_save();
    waitq_lazy(q);
    struct thread *t = current_thread;
    t->wake_at = system_ticks + ms;
    t->state = THREAD_SLEEPING;
    t->waiting_on = q;
    spin_lock(&sched_lock);
    /* a sleeping waiter lives on the sleep list; the wait queue remembers it via waiting_on */
    struct thread *pos; bool inserted = false;
    list_for_each_entry(pos, &sleep_list, sched_link) {
        if (pos->wake_at > t->wake_at) { list_add_tail(&t->sched_link, &pos->sched_link); inserted = true; break; }
    }
    if (!inserted) list_add_tail(&t->sched_link, &sleep_list);
    spin_unlock(&sched_lock);
    uint64_t hf = held->flags;
    __atomic_clear(&held->locked, __ATOMIC_RELEASE);
    schedule();
    while (__atomic_test_and_set(&held->locked, __ATOMIC_ACQUIRE)) pause();
    held->flags = hf;
    bool woken = t->waiting_on == NULL;   /* cleared by waker */
    t->waiting_on = NULL;
    irq_restore(f);
    return woken;
}

static void waitq_wake_sleepers(struct waitq *q, bool all) {
    /* wake timed waiters (which sit on the sleep list) */
    struct thread *t, *n;
    list_for_each_entry_safe(t, n, &sleep_list, sched_link) {
        if (t->waiting_on == q) {
            t->waiting_on = NULL;
            list_del(&t->sched_link);
            t->state = THREAD_READY;
            list_add_tail(&t->sched_link, &run_queue);
            if (!all) return;
        }
    }
}

void waitq_wake_one(struct waitq *q) {
    waitq_lazy(q);
    spin_lock(&sched_lock);
    if (!list_empty(&q->waiters)) {
        struct thread *t = list_first_entry(&q->waiters, struct thread, sched_link);
        list_del(&t->sched_link);
        t->waiting_on = NULL;
        t->state = THREAD_READY;
        list_add_tail(&t->sched_link, &run_queue);
    } else waitq_wake_sleepers(q, false);
    spin_unlock(&sched_lock);
}

void waitq_wake_all(struct waitq *q) {
    waitq_lazy(q);
    spin_lock(&sched_lock);
    while (!list_empty(&q->waiters)) {
        struct thread *t = list_first_entry(&q->waiters, struct thread, sched_link);
        list_del(&t->sched_link);
        t->waiting_on = NULL;
        t->state = THREAD_READY;
        list_add_tail(&t->sched_link, &run_queue);
    }
    waitq_wake_sleepers(q, true);
    spin_unlock(&sched_lock);
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
