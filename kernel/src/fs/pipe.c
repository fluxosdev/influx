#include "tty.h"
#include "mm.h"
#include "string.h"
#include "sched.h"

#define PIPE_SIZE 65536
struct pipe { char *buf; int head, tail, count; spinlock_t lock; struct waitq rd_wq, wr_wq; int readers, writers; struct vnode vn_r, vn_w;
               struct vnode *fifo; /* the named pipe this buffer belongs to (holds a reference), or NULL */
               uint64_t r_opens, w_opens; /* FIFO opens so far: an opener waits for the other side's count to move */ };

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

/* ---- named pipes (mkfifo) ----
 * The filesystem node's priv points to a struct fifo; the first open creates the pipe and the last
 * close frees it (the pipe holds a reference to the node meanwhile). Every open gets an end of its
 * own, which counts as a reader, a writer or both (O_RDWR), so each close is accounted for. */
struct fifo { struct mutex lock; struct pipe *p; };
struct fifo_end { struct vnode vn; struct pipe *p; bool rd, wr; };

static void fifo_end_release(struct vnode *v) {
    struct fifo_end *e = (struct fifo_end *)v;
    struct pipe *p = e->p;
    struct vnode *node = p->fifo;
    struct fifo *f = node->priv;
    mutex_lock(&f->lock);
    spin_lock(&p->lock);
    if (e->rd) p->readers--;
    if (e->wr) p->writers--;
    bool dead = p->readers == 0 && p->writers == 0;
    waitq_wake_all(&p->rd_wq); waitq_wake_all(&p->wr_wq);
    spin_unlock(&p->lock);
    if (dead) f->p = NULL;
    mutex_unlock(&f->lock);
    poll_notify();
    if (dead) { kfree(p->buf); kfree(p); vnode_unref(node); }
    kfree(e);
}
static int fifo_poll_rw(struct vnode *v, int ev) {
    struct fifo_end *e = (struct fifo_end *)v;
    return (e->rd ? pipe_poll_r(v, ev) : 0) | (e->wr ? pipe_poll_w(v, ev) : 0);
}
static const struct vnode_ops fifo_r_ops = { pipe_read, NULL, NULL, NULL, NULL, NULL, NULL, pipe_ioctl, fifo_end_release, NULL, NULL, fifo_poll_rw };
static const struct vnode_ops fifo_w_ops = { NULL, pipe_write, NULL, NULL, NULL, NULL, NULL, pipe_ioctl, fifo_end_release, NULL, NULL, fifo_poll_rw };
static const struct vnode_ops fifo_rw_ops = { pipe_read, pipe_write, NULL, NULL, NULL, NULL, NULL, pipe_ioctl, fifo_end_release, NULL, NULL, fifo_poll_rw };

/* opening blocks until the other side is there, as POSIX says: a reader waits for a writer and a
 * writer for a reader; O_NONBLOCK readers don't wait and O_NONBLOCK writers fail with ENXIO */
int fifo_open(struct vnode *node, int flags, struct file **out) {
    int acc = flags & O_ACCMODE;
    bool rd = acc == O_RDONLY || acc == O_RDWR, wr = acc == O_WRONLY || acc == O_RDWR;
    static struct mutex create_lock = MUTEX_INIT;
    mutex_lock(&create_lock);
    if (!node->priv) { struct fifo *nf = kzalloc(sizeof *nf); mutex_init(&nf->lock); node->priv = nf; }
    mutex_unlock(&create_lock);
    struct fifo *f = node->priv;
    mutex_lock(&f->lock);
    struct pipe *p = f->p;
    if (!p) {
        p = kzalloc(sizeof *p);
        p->buf = kmalloc(PIPE_SIZE);
        p->lock = (spinlock_t)SPINLOCK_INIT;
        waitq_init(&p->rd_wq); waitq_init(&p->wr_wq);
        p->fifo = node; vnode_ref(node);
        f->p = p;
    }
    spin_lock(&p->lock);
    if (wr && !rd && (flags & O_NONBLOCK) && p->readers == 0) { spin_unlock(&p->lock); bool dead = p->writers == 0;
        if (dead) { f->p = NULL; mutex_unlock(&f->lock); kfree(p->buf); kfree(p); vnode_unref(node); } else mutex_unlock(&f->lock);
        return -6; /* ENXIO */ }
    if (rd) { p->readers++; p->r_opens++; }
    if (wr) { p->writers++; p->w_opens++; }
    /* the other side's opens so far: once that count moves (or someone is there already), this open completes,
     * even if that other end has closed again by the time this thread runs */
    uint64_t seen = rd ? p->w_opens : p->r_opens;
    bool other_present = (rd ? p->writers : p->readers) > 0;
    waitq_wake_all(&p->rd_wq); waitq_wake_all(&p->wr_wq);
    spin_unlock(&p->lock);
    struct fifo_end *e = kzalloc(sizeof *e);
    strcpy(e->vn.name, "fifo"); e->vn.type = VN_DEV; e->vn.priv = p; e->vn.refcount = 1;
    e->vn.ops = acc == O_RDWR ? &fifo_rw_ops : rd ? &fifo_r_ops : &fifo_w_ops;
    e->p = p; e->rd = rd; e->wr = wr;
    mutex_unlock(&f->lock);
    poll_notify();
    struct file *fl = kzalloc(sizeof *fl);
    fl->vn = &e->vn; fl->flags = flags; fl->refcount = 1;
    /* wait for the other side (not for O_RDWR, which is both) */
    if (acc != O_RDWR && !(rd && (flags & O_NONBLOCK)) && !other_present) {
        spin_lock(&p->lock);
        while ((rd ? p->w_opens : p->r_opens) == seen && !signal_interrupted()) waitq_wait(rd ? &p->rd_wq : &p->wr_wq, &p->lock);
        bool ok = (rd ? p->w_opens : p->r_opens) != seen;
        spin_unlock(&p->lock);
        if (!ok) { vfs_close(fl); return -EINTR; }
    }
    *out = fl;
    return 0;
}
/* the node itself goes (its last reference): no pipe can be open, it would hold one */
void fifo_release_node(struct vnode *v) { kfree(v->priv); v->priv = NULL; }
