#pragma once
#include "io.h"

/* The kernel runs on one CPU; a spinlock therefore only needs to mask
 * interrupts, which keeps it safe against IRQ handlers and preemption. */
typedef struct spinlock { volatile int locked; uint64_t flags; } spinlock_t;
#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock(spinlock_t *l) {
    uint64_t f = irq_save();
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE)) pause();
    l->flags = f;
}
static inline void spin_unlock(spinlock_t *l) {
    uint64_t f = l->flags;
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
    irq_restore(f);
}
