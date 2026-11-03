#pragma once
#include "mm.h"
#include "string.h"
#include "sched.h"
#include "percpu.h"
#include "spinlock.h"
/* sizes come from callers (window sizes, texture uploads): fail with GL_OUT_OF_MEMORY rather than panic */
#define GL_MALLOC(n) kmalloc_try(n)
#define GL_FREE(p) kfree(p)
#define GL_MEMSET(p, v, n) memset(p, v, n)
#define GL_MEMCPY(d, s, n) memcpy(d, s, n)
#define GL_STRLEN(s) strlen(s)

/* The current context is per kernel thread: every OpenGL Demo window renders from its own thread, on any CPU.
 * A small table keyed by thread (and tid, so a recycled struct thread never inherits a dead thread's slot).
 * Only the owning thread puts its own entry in, so lookups are lock-free; every change takes the lock.
 * Included by gl.c alone, so the table is a single instance. */
#define GL_MAX_CURRENT 64
static struct gl_cur_slot { struct thread *t; int tid; void *ctx; } gl_cur_slots[GL_MAX_CURRENT];
static spinlock_t gl_cur_lock = SPINLOCK_INIT;
static inline void *gl_port_get_current(void) {
    struct thread *t = current_thread;
    for (int i = 0; i < GL_MAX_CURRENT; i++)
        if (__atomic_load_n(&gl_cur_slots[i].t, __ATOMIC_ACQUIRE) == t && gl_cur_slots[i].tid == t->tid) return gl_cur_slots[i].ctx;
    return 0;
}
/* 0 when the table is full: the thread is left with no current context (GL calls become no-ops) */
static inline int gl_port_set_current(void *ctx) {
    struct thread *t = current_thread; int ok = !ctx, free_slot = -1;
    spin_lock(&gl_cur_lock);
    for (int i = 0; i < GL_MAX_CURRENT; i++) {
        struct gl_cur_slot *s = &gl_cur_slots[i];
        if (s->t == t) { __atomic_store_n(&s->t, (struct thread *)0, __ATOMIC_RELEASE); s->ctx = 0; }   /* ours, or stale from a dead thread at this address */
        if (!s->t && free_slot < 0) free_slot = i;
    }
    if (ctx && free_slot >= 0) {
        struct gl_cur_slot *s = &gl_cur_slots[free_slot];
        s->ctx = ctx; s->tid = t->tid; __atomic_store_n(&s->t, t, __ATOMIC_RELEASE); ok = 1;
    }
    spin_unlock(&gl_cur_lock);
    return ok;
}
/* a context is being destroyed: no thread may keep it current */
static inline void gl_port_forget(void *ctx) {
    spin_lock(&gl_cur_lock);
    for (int i = 0; i < GL_MAX_CURRENT; i++) if (gl_cur_slots[i].t && gl_cur_slots[i].ctx == ctx) { __atomic_store_n(&gl_cur_slots[i].t, (struct thread *)0, __ATOMIC_RELEASE); gl_cur_slots[i].ctx = 0; }
    spin_unlock(&gl_cur_lock);
}
#define GL_GET_CURRENT() gl_port_get_current()
#define GL_SET_CURRENT(c) gl_port_set_current(c)
#define GL_FORGET_CONTEXT(c) gl_port_forget(c)
