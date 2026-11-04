/*
 * Futexes: user-space locks sleep here only when contended. A waiter is keyed
 * by (address space, user address); FUTEX_WAIT checks the word under the
 * futex lock before sleeping, so a wake that follows a change of the word is
 * never lost. Each waiter sleeps on its own queue, so a wake takes exactly
 * the waiters it counts.
 */
#include "sched.h"
#include "uaccess.h"
#include "signal.h"
#include "string.h"
#include "fs.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define EAGAIN 11
#define EINTR 4

struct futex_waiter { struct list_head link; uint64_t mm, addr; uint32_t bitset; bool woken; struct waitq wq; };
static LIST_HEAD(waiters);
static spinlock_t futex_lock = SPINLOCK_INIT;

static uint64_t mm_key(void) { return current_thread->proc->pml4; }

/* timeout_ns < 0: none */
static int64_t futex_wait(uint64_t uaddr, uint32_t val, int64_t timeout_ns, uint32_t bitset) {
    if (uaddr & 3) return -EINVAL;
    if (!bitset) return -EINVAL;
    struct futex_waiter w;
    memset(&w, 0, sizeof w);
    w.mm = mm_key(); w.addr = uaddr; w.bitset = bitset;
    waitq_init(&w.wq);
    uint32_t cur;
    /* fault the word in first: the check below runs with interrupts off */
    if (get_user(cur, uaddr) < 0) return -EFAULT;
    spin_lock(&futex_lock);
    if (get_user(cur, uaddr) < 0) { spin_unlock(&futex_lock); return -EFAULT; }
    if (cur != val) { spin_unlock(&futex_lock); return -EAGAIN; }
    list_add_tail(&w.link, &waiters);
    uint64_t deadline = timeout_ns >= 0 ? time_ns() + (uint64_t)timeout_ns : 0;
    int64_t r = 0;
    while (!w.woken) {
        if (signal_interrupted()) { r = -EINTR; break; }
        if (timeout_ns >= 0) {
            uint64_t now = time_ns();
            if (now >= deadline) { r = -ETIMEDOUT; break; }
            uint64_t ms = (deadline - now + 999999) / 1000000;
            waitq_wait_timeout(&w.wq, &futex_lock, ms ? ms : 1);
        } else waitq_wait(&w.wq, &futex_lock);
    }
    if (w.woken) r = 0;
    else list_del(&w.link);
    spin_unlock(&futex_lock);
    return r;
}

/* futex_lock held */
static int wake_locked(uint64_t mm, uint64_t uaddr, int n, uint32_t bitset) {
    int woken = 0;
    struct futex_waiter *w, *tmp;
    list_for_each_entry_safe(w, tmp, &waiters, link) {
        if (woken >= n) break;
        if (w->mm != mm || w->addr != uaddr || !(w->bitset & bitset)) continue;
        list_del(&w->link);
        w->woken = true;
        waitq_wake_all(&w->wq);
        woken++;
    }
    return woken;
}

int futex_wake(uint64_t uaddr, int n) {
    spin_lock(&futex_lock);
    int r = wake_locked(mm_key(), uaddr, n, 0xFFFFFFFF);
    spin_unlock(&futex_lock);
    return r;
}

static int64_t futex_requeue(uint64_t uaddr, int nwake, int nreq, uint64_t uaddr2, bool cmp, uint32_t val) {
    uint64_t mm = mm_key();
    uint32_t cur;
    if (cmp && get_user(cur, uaddr) < 0) return -EFAULT;
    spin_lock(&futex_lock);
    if (cmp) {
        if (get_user(cur, uaddr) < 0) { spin_unlock(&futex_lock); return -EFAULT; }
        if (cur != val) { spin_unlock(&futex_lock); return -EAGAIN; }
    }
    int r = wake_locked(mm, uaddr, nwake, 0xFFFFFFFF);
    struct futex_waiter *w;
    int moved = 0;
    list_for_each_entry(w, &waiters, link) {
        if (moved >= nreq) break;
        if (w->mm == mm && w->addr == uaddr) { w->addr = uaddr2; moved++; }
    }
    spin_unlock(&futex_lock);
    return r + moved;
}

struct kts { int64_t sec, nsec; };

int64_t sys_futex(uint64_t uaddr, int op, uint32_t val, uint64_t utimeout, uint64_t uaddr2, uint32_t val3) {
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    switch (cmd) {
    case FUTEX_WAIT: case FUTEX_WAIT_BITSET: {
        int64_t timeout = -1;
        if (utimeout) {
            struct kts ts;
            if (copy_from_user(&ts, utimeout, sizeof ts) < 0) return -EFAULT;
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) return -EINVAL;
            int64_t t = ts.sec * 1000000000 + ts.nsec;
            if (cmd == FUTEX_WAIT_BITSET) {   /* absolute */
                int64_t now = (op & FUTEX_CLOCK_REALTIME) ? time_realtime_ns() : (int64_t)time_ns();
                t = t > now ? t - now : 0;
            }
            timeout = t;
        }
        return futex_wait(uaddr, val, timeout, cmd == FUTEX_WAIT ? 0xFFFFFFFF : val3);
    }
    case FUTEX_WAKE: return futex_wake(uaddr, (int)val);
    case FUTEX_WAKE_BITSET: {
        spin_lock(&futex_lock);
        int r = wake_locked(mm_key(), uaddr, (int)val, val3);
        spin_unlock(&futex_lock);
        return r;
    }
    case FUTEX_REQUEUE: return futex_requeue(uaddr, (int)val, (int)utimeout, uaddr2, false, 0);
    case FUTEX_CMP_REQUEUE: return futex_requeue(uaddr, (int)val, (int)utimeout, uaddr2, true, val3);
    }
    return -ENOSYS;
}
