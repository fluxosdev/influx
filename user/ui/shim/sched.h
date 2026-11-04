/* user-space build of kernel sources: threads and mutexes are pthreads */
#pragma once
#include "kernel.h"
#include "list.h"
#include <pthread.h>
#include <unistd.h>

struct mutex { pthread_mutex_t m; };
#define MUTEX_INIT { PTHREAD_MUTEX_INITIALIZER }
static inline void mutex_init(struct mutex *m) { pthread_mutex_init(&m->m, NULL); }
static inline void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->m); }
static inline void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->m); }
static inline bool mutex_trylock(struct mutex *m) { return pthread_mutex_trylock(&m->m) == 0; }

struct process;
struct thread { struct process *proc; };
extern struct thread ui_current_thread;
#define current_thread (&ui_current_thread)
/* a detached worker thread; p is ignored (threads belong to the calling process) */
struct thread *thread_create(struct process *p, const char *name, void (*entry)(void *), void *arg);
static inline void thread_sleep_ms(uint64_t ms) { usleep((useconds_t)(ms * 1000)); }
static inline bool thread_should_exit(void) { return false; }
