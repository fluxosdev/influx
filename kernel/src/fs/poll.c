/* poll() support: one global wait queue woken whenever any pollable object
 * changes state; pollers re-check their descriptors each time. Simple, and
 * cheap at the scale of a desktop. */
#include "fs.h"
#include "signal.h"

static spinlock_t lock = SPINLOCK_INIT;
static struct waitq wq;
static volatile uint64_t gen;

int vfs_poll(struct vnode *v, int events) {
    if (v->ops->poll) return v->ops->poll(v, events);
    return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

uint64_t poll_generation(void) { return __atomic_load_n(&gen, __ATOMIC_ACQUIRE); }

void poll_notify(void) {
    spin_lock(&lock);
    gen++;
    waitq_wake_all(&wq);
    spin_unlock(&lock);
}

bool poll_wait(uint64_t g, int64_t ms) {
    spin_lock(&lock);
    if (!wq.waiters.next) waitq_init(&wq);
    uint64_t deadline = ms >= 0 ? time_ms() + (uint64_t)ms : 0;
    bool changed = false;
    for (;;) {
        if (gen != g) { changed = true; break; }
        if (signal_interrupted()) break;
        if (ms < 0) waitq_wait(&wq, &lock);
        else {
            uint64_t now = time_ms();
            if (now >= deadline) break;
            waitq_wait_timeout(&wq, &lock, deadline - now);
        }
    }
    spin_unlock(&lock);
    return changed;
}
