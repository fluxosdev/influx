#pragma once
#include "io.h"

/* A spinlock masks interrupts on the holding CPU (so IRQ handlers and
 * preemption cannot deadlock against it) and spins on an atomic flag against
 * the other CPUs. A CPU spinning with interrupts off still answers TLB
 * shootdowns, so a holder that remaps kernel memory cannot deadlock with it. */
typedef struct spinlock { volatile int locked; uint64_t flags; } spinlock_t;
#define SPINLOCK_INIT { 0, 0 }

extern volatile uint64_t tlb_pending_cpus;   /* arch/smp.c */
void tlb_service(void);

/* take the flag only; the caller has interrupts off */
static inline void spin_acquire(spinlock_t *l) {
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE)) {
        do {
            pause();
            if (__builtin_expect(tlb_pending_cpus != 0, 0)) tlb_service();
        } while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED));
    }
}
static inline void spin_release(spinlock_t *l) { __atomic_clear(&l->locked, __ATOMIC_RELEASE); }

static inline void spin_lock(spinlock_t *l) {
    uint64_t f = irq_save();
    spin_acquire(l);
    l->flags = f;
}
static inline void spin_unlock(spinlock_t *l) {
    uint64_t f = l->flags;
    spin_release(l);
    irq_restore(f);
}
