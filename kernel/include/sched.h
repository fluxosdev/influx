#pragma once
#ifdef INFLUX_USERSPACE
/* kernel sources compiled into user programs (user/ui): threads and mutexes are pthreads there */
#include "../../user/ui/shim/sched.h"
#else
#include "kernel.h"
#include "list.h"
#include "spinlock.h"
#include "arch.h"
#include "signal.h"

#define MAX_FDS 64
#define KSTACK_SIZE (128 * 1024)   /* the shell's command path alone needs about 90 KiB */

enum thread_state { THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_SLEEPING, THREAD_ZOMBIE };

struct process;
struct file;
struct waitq { struct list_head waiters; };

struct thread {
    uint64_t rsp;                 /* saved kernel stack pointer */
    uint64_t kstack_base, kstack_top;
    int tid;
    char name[32];
    enum thread_state state;
    struct process *proc;
    void (*entry)(void *);
    void *arg;
    uint64_t wake_at;
    int slice;
    uint64_t cpu_ticks;           /* ms of CPU consumed */
    uint64_t cpu_sample;          /* ticks at last task-manager sample */
    int cpu_percent;
    struct list_head sched_link;  /* run queue / sleep list / wait queue */
    struct list_head proc_link;   /* process thread list */
    struct list_head all_link;
    uint8_t *fpu;                 /* x87/SSE/AVX state (XSAVE or FXSAVE image) */
    void *fpu_mem;                /* allocation backing 'fpu' */
    int irq_depth;                /* nested interrupt handlers running on this thread */
    uint64_t user_rip, user_rsp;  /* initial user-mode entry */
    int exit_code;
    struct waitq *waiting_on;
    bool kill_pending;
    bool is_user;
    bool on_cpu;                  /* running, or its CPU has not finished switching away (sched_lock) */
    bool wake_pending;            /* woken while not waiting: its next wait returns at once (sched_lock) */
    /* user threads */
    struct regs *start_regs;      /* first entry to user mode with this full context (kfree'd once used) */
    uint64_t fs_base;             /* TLS (arch_prctl ARCH_SET_FS) */
    uint64_t sigmask;             /* blocked signals (bit sig-1) */
    uint64_t sig_pending;         /* sent to this thread (tgkill, faults) */
    uint64_t clear_child_tid;     /* user address zeroed and futex-woken when the thread exits */
    uint64_t alt_sp, alt_size;    /* sigaltstack */
    bool alt_flags_disable;
    struct regs *uregs;           /* this thread's user frame on its kernel stack (in a syscall/interrupt from user mode) */
    bool iret_return;             /* the syscall return must restore the full frame (iretq) */
    int64_t sys_nr;               /* system call in progress (restart after a signal) */
    uint64_t saved_sigmask;       /* sigsuspend/ppoll: the mask to restore after the handler */
    bool restore_sigmask;
};

struct process {
    int pid;
    char name[32];
    uint64_t pml4;                /* 0 => kernel address space */
    bool is_user;
    struct list_head threads;
    struct list_head all_link;
    struct process *parent;       /* may dangle once the parent is reaped: compare ppid instead */
    int ppid;                     /* creator's pid (0: none) */
    struct file *fds[MAX_FDS];
    char cwd[256];
    uint64_t brk_start, brk;
    int exit_code;
    bool exited;                  /* every thread gone and resources released: ready to reap */
    bool reaping;                 /* a process_wait() has claimed it */
    bool orphan;                  /* its creator has exited: nobody will wait for it, so it frees itself (exit_lock) */
    int nthreads;                 /* threads not yet in thread_exit */
    struct waitq exit_wq;
    uint64_t heap_bytes;          /* kernel heap bytes owned by this process */
    uint64_t start_time;
    int app_id;                   /* desktop app id (-1 = none) */
    void *app_data;
    int exit_signal;
    struct window *uwin[8];       /* windows owned by a user process */
    /* user address space (uvm.h) */
    struct list_head vmas;
    spinlock_t vm_lock;
    /* POSIX */
    uint64_t fd_cloexec;          /* bit per descriptor: closed by execve */
    struct k_sigaction sigact[NSIG + 1];
    uint64_t sig_pending;         /* sent to the process: any thread not blocking it takes it */
    struct waitq child_wq;        /* a child exited or changed state (wait4) */
    int uid, gid, euid, egid;
    uint32_t umask;
    int pgid, sid;
    bool execed;                  /* has called execve (setpgid rules) */
    uint64_t child_gen;           /* bumped when a child exits (exit_lock) */
    uint64_t alarm_at_ms;         /* ITIMER_REAL: time_ms() when SIGALRM is due (0: not armed) */
    uint64_t alarm_interval_ms;   /* and the period after that (0: one shot) */
};
void process_close_windows(struct process *p);
static inline bool process_has_signal(struct process *p, struct thread *t) { return ((p->sig_pending | t->sig_pending) & ~t->sigmask) != 0; }

/* current_thread is per CPU (percpu.h) */
extern struct process *kernel_process;

void sched_init(void);
void sched_start(void) NORETURN;
void sched_create_idle(struct cpu *c);
void sched_ap_enter(void) NORETURN;   /* an AP, set up, starts scheduling */
struct thread *thread_create(struct process *p, const char *name, void (*entry)(void *), void *arg);
struct thread *thread_create_user(struct process *p, const char *name, uint64_t rip, uint64_t rsp);
struct thread *thread_create_user_regs(struct process *p, const char *name, const struct regs *r, uint64_t fs_base, uint64_t sigmask, const uint8_t *fpu);
int process_wait_child(int pid, int *status, bool nohang);   /* wait4 */
void thread_exit(int code) NORETURN;
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
void thread_block(void);
void thread_unblock(struct thread *t);
bool thread_should_exit(void);
void schedule(void);
void sched_tick(struct regs *r);
struct thread *thread_find(int tid);
void thread_kill(struct thread *t);

struct process *process_create(const char *name, bool user);
void process_exit(int code) NORETURN;
struct process *process_find(int pid);
/* Hold off process and thread creation, exit and freeing: a walk of the
 * process list, or of a process's threads, is safe between these. Recursive;
 * interrupts stay off, so nothing in between may sleep. */
void procs_lock(void);
void procs_unlock(void);
int process_wait(int pid, int *status);
void process_mark_exited(struct process *p);   /* every thread gone: ready to reap */
int process_user_count(void);         /* user processes not yet reaped */
bool process_kill(int pid, int sig);
int process_list(struct process **out, int max);
int thread_list(struct thread **out, int max);
void sched_sample_cpu(void);          /* refresh cpu_percent fields (task manager) */
int sched_idle_percent(void);         /* averaged over the CPUs */
int sched_cpu_load(int cpu);          /* busy percent of one CPU over the last sample */
extern uint64_t sched_context_switches;

/* wait queues */
void waitq_init(struct waitq *q);
void waitq_wait(struct waitq *q, spinlock_t *held);
bool waitq_wait_timeout(struct waitq *q, spinlock_t *held, uint64_t ms);   /* false on timeout */
void waitq_wake_one(struct waitq *q);
void waitq_wake_all(struct waitq *q);

/* mutex */
struct mutex { spinlock_t lock; bool locked; struct waitq wq; struct thread *owner; };
#define MUTEX_INIT { SPINLOCK_INIT, false, { { 0, 0 } }, NULL }
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_trylock(struct mutex *m);

/* counting semaphore */
struct semaphore { spinlock_t lock; int count; struct waitq wq; };
void sem_init(struct semaphore *s, int count);
void sem_wait(struct semaphore *s);
bool sem_wait_timeout(struct semaphore *s, uint64_t ms);
bool sem_wait_killable(struct semaphore *s, int64_t ms);   /* ms < 0: no limit; false on timeout or when the thread is killed */
bool sem_trywait(struct semaphore *s);
void sem_post(struct semaphore *s);

/* user process execution (exec.c) */
int process_spawn(const char *path, int argc, char **argv, struct file *stdio[3], const char *cwd);
int process_spawn_env(const char *path, int argc, char **argv, char *const *envp, struct file *stdio[3], const char *cwd);
int process_spawn_app(const char *path, int argc, char **argv, const char *cwd, int app_id);
void syscall_init(void);

#endif /* INFLUX_USERSPACE */
