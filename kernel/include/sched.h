#pragma once
#include "kernel.h"
#include "list.h"
#include "spinlock.h"
#include "arch.h"

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
};

struct process {
    int pid;
    char name[32];
    uint64_t pml4;                /* 0 => kernel address space */
    bool is_user;
    struct list_head threads;
    struct list_head all_link;
    struct process *parent;
    struct file *fds[MAX_FDS];
    char cwd[256];
    uint64_t brk_start, brk;
    int exit_code;
    bool exited;                  /* every thread gone and resources released: ready to reap */
    bool reaping;                 /* a process_wait() has claimed it */
    int nthreads;                 /* threads not yet in thread_exit */
    struct waitq exit_wq;
    uint64_t heap_bytes;          /* kernel heap bytes owned by this process */
    uint64_t start_time;
    int app_id;                   /* desktop app id (-1 = none) */
    void *app_data;
    int exit_signal;
    struct window *uwin[8];       /* windows owned by a user process */
};
void process_close_windows(struct process *p);

extern struct thread *current_thread;
extern struct process *kernel_process;

void sched_init(void);
void sched_start(void) NORETURN;
struct thread *thread_create(struct process *p, const char *name, void (*entry)(void *), void *arg);
struct thread *thread_create_user(struct process *p, const char *name, uint64_t rip, uint64_t rsp);
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
int process_wait(int pid, int *status);
bool process_kill(int pid, int sig);
int process_list(struct process **out, int max);
int thread_list(struct thread **out, int max);
void sched_sample_cpu(void);          /* refresh cpu_percent fields (task manager) */
int sched_idle_percent(void);
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
bool sem_trywait(struct semaphore *s);
void sem_post(struct semaphore *s);

/* user process execution (exec.c) */
int process_spawn(const char *path, int argc, char **argv, struct file *stdio[3], const char *cwd);
void syscall_init(void);
