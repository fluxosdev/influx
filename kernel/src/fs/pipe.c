#include "tty.h"
#include "mm.h"
#include "string.h"
#include "sched.h"

#define PIPE_SIZE 65536
struct pipe { char *buf; int head, tail, count; spinlock_t lock; struct waitq rd_wq, wr_wq; int readers, writers; struct vnode vn_r, vn_w; };

#include "signal.h"

static int64_t pipe_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    struct pipe *p = v->priv; char *out = buf; size_t got = 0;
    if (n == 0) return 0;   /* read(fd, buf, 0) must not block */
    spin_lock(&p->lock);
    while (p->count == 0 && p->writers > 0 && !signal_interrupted()) waitq_wait(&p->rd_wq, &p->lock);
    if (p->count == 0 && p->writers > 0) { spin_unlock(&p->lock); return -ERESTARTSYS; }
    while (got < n && p->count > 0) { out[got++] = p->buf[p->head]; p->head = (p->head + 1) % PIPE_SIZE; p->count--; }
    spin_unlock(&p->lock);
    waitq_wake_all(&p->wr_wq);
    poll_notify();
    return got;
}
static int64_t pipe_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    struct pipe *p = v->priv; const char *in = buf; size_t done = 0;
    while (done < n) {
        spin_lock(&p->lock);
        while (p->count == PIPE_SIZE && p->readers > 0 && !signal_interrupted()) waitq_wait(&p->wr_wq, &p->lock);
        if (p->readers == 0) {
            spin_unlock(&p->lock);
            struct thread *t = current_thread;
            if (t->is_user) signal_send_thread(t, SIGPIPE);
            return done ? (int64_t)done : -EPIPE;
        }
        if (p->count == PIPE_SIZE) { spin_unlock(&p->lock); return done ? (int64_t)done : -ERESTARTSYS; }
        while (done < n && p->count < PIPE_SIZE) { p->buf[p->tail] = in[done++]; p->tail = (p->tail + 1) % PIPE_SIZE; p->count++; }
        spin_unlock(&p->lock);
        waitq_wake_all(&p->rd_wq);
        poll_notify();
    }
    return done;
}
static void pipe_release_r(struct vnode *v) {
    struct pipe *p = v->priv;
    spin_lock(&p->lock); p->readers--; bool dead = p->readers == 0 && p->writers == 0;
    waitq_wake_all(&p->wr_wq);   /* under p->lock (it only takes sched_lock): once dropped, the other end may free p */
    spin_unlock(&p->lock);
    poll_notify();
    if (dead) { kfree(p->buf); kfree(p); }
}
static void pipe_release_w(struct vnode *v) {
    struct pipe *p = v->priv;
    spin_lock(&p->lock); p->writers--; bool dead = p->readers == 0 && p->writers == 0;
    waitq_wake_all(&p->rd_wq);   /* under p->lock (it only takes sched_lock): once dropped, the other end may free p */
    spin_unlock(&p->lock);
    poll_notify();
    if (dead) { kfree(p->buf); kfree(p); }
}
static int pipe_poll_r(struct vnode *v, int ev) {
    struct pipe *p = v->priv; int r = 0;
    spin_lock(&p->lock);
    if (p->count) r |= POLLIN | POLLRDNORM;
    if (!p->writers) r |= POLLHUP | (p->count ? 0 : POLLIN);
    spin_unlock(&p->lock);
    return r;
}
static int pipe_poll_w(struct vnode *v, int ev) {
    struct pipe *p = v->priv; int r = 0;
    spin_lock(&p->lock);
    if (p->count < PIPE_SIZE) r |= POLLOUT | POLLWRNORM;
    if (!p->readers) r |= POLLERR;
    spin_unlock(&p->lock);
    return r;
}
static int pipe_ioctl(struct vnode *v, int cmd, void *arg) {
    struct pipe *p = v->priv;
    if (cmd == 0x541B) { *(int *)arg = p->count; return 0; }   /* FIONREAD */
    return -ENOTTY_;
}
static const struct vnode_ops pipe_r_ops = { pipe_read, NULL, NULL, NULL, NULL, NULL, NULL, pipe_ioctl, pipe_release_r, NULL, NULL, pipe_poll_r };
static const struct vnode_ops pipe_w_ops = { NULL, pipe_write, NULL, NULL, NULL, NULL, NULL, pipe_ioctl, pipe_release_w, NULL, NULL, pipe_poll_w };

int pipe_create(struct file **rd, struct file **wr) {
    struct pipe *p = kzalloc(sizeof *p);
    p->buf = kmalloc(PIPE_SIZE);
    p->lock = (spinlock_t)SPINLOCK_INIT;
    waitq_init(&p->rd_wq); waitq_init(&p->wr_wq);
    p->readers = 1; p->writers = 1;
    strcpy(p->vn_r.name, "pipe"); p->vn_r.type = VN_DEV; p->vn_r.ops = &pipe_r_ops; p->vn_r.priv = p; p->vn_r.refcount = 1;
    strcpy(p->vn_w.name, "pipe"); p->vn_w.type = VN_DEV; p->vn_w.ops = &pipe_w_ops; p->vn_w.priv = p; p->vn_w.refcount = 1;
    struct file *r = kzalloc(sizeof *r), *w = kzalloc(sizeof *w);
    r->vn = &p->vn_r; r->flags = O_RDONLY; r->refcount = 1;
    w->vn = &p->vn_w; w->flags = O_WRONLY; w->refcount = 1;
    *rd = r; *wr = w;
    return 0;
}
