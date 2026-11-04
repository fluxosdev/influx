/*
 * System calls. Numbers, arguments, structures and error codes follow the
 * Linux x86-64 ABI, so C libraries written for Linux (and the programs built
 * with them) run on inFlux; the inFlux-specific calls (desktop windows,
 * process and disk listings, spawn) live at 0x1000 and up.
 *
 * Every access to user memory goes through uaccess.h: data is copied in and
 * out through kernel buffers, so a bad pointer, or memory another thread
 * unmaps meanwhile, is -EFAULT and never a kernel fault.
 */
#include "syscall.h"
#include "uvm.h"
#include "signal.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "tty.h"
#include "blockdev.h"
#include "ui.h"
#include "simd.h"
#include "random.h"
#include "net.h"

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
#define WNOHANG 1
#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_VFORK 0x4000
#define CLONE_THREAD 0x10000
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID 0x01000000
#define USER_PROCS_MAX 64
#define E2BIG_ 7
#define USER_WIN_BYTES_MAX (64ULL << 20)

/* ---- descriptors ---- */
struct file *fd_get(struct process *p, int fd) { return fd >= 0 && fd < MAX_FDS ? p->fds[fd] : NULL; }

int fd_install(struct process *p, struct file *f, bool cloexec, int min) {
    if (!f) return -ENOMEM;
    for (int i = MAX(min, 0); i < MAX_FDS; i++) if (!p->fds[i]) {
        p->fds[i] = f;
        if (cloexec) p->fd_cloexec |= 1ULL << i; else p->fd_cloexec &= ~(1ULL << i);
        return i;
    }
    vfs_close(f);
    return -EMFILE;
}
static int fd_close(struct process *p, int fd) {
    struct file *f = fd_get(p, fd);
    if (!f) return -EBADF;
    p->fds[fd] = NULL;
    p->fd_cloexec &= ~(1ULL << fd);
    return vfs_close(f);
}

/* ---- paths ---- */
static int get_path(uint64_t u, char *out) {
    long r = strncpy_from_user(out, u, PATH_MAX);
    if (r < 0) return (int)r;
    if (r == 0) return -ENOENT;
    return 0;
}
/* absolute path for a user path relative to dirfd (or the cwd) */
static int resolve_at(struct process *p, int dirfd, uint64_t upath, char *out, bool empty_ok) {
    char path[PATH_MAX];
    long r = strncpy_from_user(path, upath, PATH_MAX);
    if (r < 0) return (int)r;
    const char *base = p->cwd;
    if (path[0] != '/' && dirfd != AT_FDCWD) {
        struct file *f = fd_get(p, dirfd);
        if (!f) return -EBADF;
        if (!path[0] && empty_ok) { if (!f->path) return -EBADF; strlcpy(out, f->path, PATH_MAX); return 0; }
        if (f->vn->type != VN_DIR || !f->path) return -ENOTDIR;
        base = f->path;
    }
    if (!path[0]) return -ENOENT;
    return vfs_normalize(base, path, out, PATH_MAX);
}

/* ---- stat ---- */
struct linux_stat {
    uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad0; uint64_t rdev; int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; int64_t reserved[3];
};
static uint32_t mode_of(int type, uint32_t perm) {
    uint32_t fmt = type == VN_DIR ? 0040000 : type == VN_DEV ? 0020000 : type == VN_BLK ? 0060000 : type == VN_LNK ? 0120000 : 0100000;
    if (perm & 0170000) return perm;   /* already carries a type (sockets) */
    return fmt | (perm & 07777 ? perm & 07777 : (type == VN_DIR ? 0755 : 0644));
}
static void fill_stat(struct linux_stat *ls, struct vnode *v) {
    memset(ls, 0, sizeof *ls);
    ls->dev = (uint64_t)(uintptr_t)v->ops & 0xFFFF; ls->ino = v->ino ? v->ino : (uint64_t)(uintptr_t)v >> 4;
    ls->nlink = v->type == VN_DIR ? 2 : 1;
    ls->mode = mode_of(v->type, v->mode);
    ls->size = (int64_t)v->size; ls->blksize = 4096; ls->blocks = (int64_t)((v->size + 511) / 512);
    ls->atime = ls->mtime = ls->ctime = (int64_t)v->mtime;
}
static int64_t stat_path(struct process *p, const char *abs, uint64_t ust, bool follow) {
    struct vnode *v = follow ? vfs_lookup(abs) : vfs_lookup_nofollow(abs);
    if (!v) return -ENOENT;
    struct linux_stat ls; fill_stat(&ls, v);
    vnode_unref(v);
    return copy_to_user(ust, &ls, sizeof ls) < 0 ? -EFAULT : 0;
}

/* ---- read / write ---- */
static bool would_block(struct file *f, int ev) { return (f->flags & O_NONBLOCK) && f->vn->ops->poll && !(vfs_poll(f->vn, ev) & (ev | POLLHUP | POLLERR)); }

static int64_t do_read(struct file *f, uint64_t ubuf, size_t n, int64_t off) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->vn->type == VN_DIR) return -EISDIR;
    if (!n) return 0;
    if (would_block(f, POLLIN)) return -EAGAIN;
    bool stream = f->vn->type != VN_FILE && f->vn->type != VN_BLK;
    size_t chunk = MIN(n, (size_t)XFER_CHUNK);
    uint8_t *k = kmalloc(chunk);
    if (!k) return -ENOMEM;
    size_t done = 0;
    while (done < n) {
        size_t want = MIN(n - done, chunk);
        int64_t r = off >= 0 ? vfs_pread(f->vn, (uint64_t)off + done, k, want) : vfs_read(f, k, want);
        if (r < 0) { kfree(k); return done ? (int64_t)done : r; }
        if (r > 0 && copy_to_user(ubuf + done, k, (size_t)r) < 0) { kfree(k); return done ? (int64_t)done : -EFAULT; }
        done += (size_t)r;
        if ((size_t)r < want || stream) break;
    }
    kfree(k);
    return (int64_t)done;
}

static int64_t do_write(struct file *f, uint64_t ubuf, size_t n, int64_t off) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!n) return 0;
    if (would_block(f, POLLOUT)) return -EAGAIN;
    size_t chunk = MIN(n, (size_t)XFER_CHUNK);
    uint8_t *k = kmalloc(chunk);
    if (!k) return -ENOMEM;
    size_t done = 0;
    while (done < n) {
        size_t want = MIN(n - done, chunk);
        if (copy_from_user(k, ubuf + done, want) < 0) { kfree(k); return done ? (int64_t)done : -EFAULT; }
        int64_t r = off >= 0 ? vfs_pwrite(f->vn, (uint64_t)off + done, k, want) : vfs_write(f, k, want);
        if (r < 0) { kfree(k); return done ? (int64_t)done : r; }
        done += (size_t)r;
        if ((size_t)r < want) break;
    }
    kfree(k);
    return (int64_t)done;
}

struct kiovec { uint64_t base, len; };
static int64_t do_rwv(struct file *f, uint64_t uiov, int cnt, bool write, int64_t off) {
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        struct kiovec v;
        if (copy_from_user(&v, uiov + (uint64_t)i * sizeof v, sizeof v) < 0) return total ? total : -EFAULT;
        if (!v.len) continue;
        int64_t r = write ? do_write(f, v.base, v.len, off >= 0 ? off + total : -1) : do_read(f, v.base, v.len, off >= 0 ? off + total : -1);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < v.len) break;
    }
    return total;
}

/* ---- directories ---- */
static int64_t sys_getdents64(struct process *p, int fd, uint64_t ubuf, size_t len) {
    struct file *f = fd_get(p, fd);
    if (!f) return -EBADF;
    if (f->vn->type != VN_DIR) return -ENOTDIR;
    size_t o = 0;
    uint8_t rec[NAME_MAX + 32];
    for (;;) {
        struct dirent d;
        int r = vfs_readdir(f->vn, (int)f->pos, &d);
        if (r < 0) break;
        size_t nl = strlen(d.name);
        size_t reclen = ALIGN_UP(19 + nl + 1, 8);
        if (o + reclen > len) { if (!o) return -EINVAL; break; }
        memset(rec, 0, reclen);
        uint64_t ino = d.ino ? d.ino : f->pos + 1, next = f->pos + 1;
        memcpy(rec, &ino, 8); memcpy(rec + 8, &next, 8);
        uint16_t rl = (uint16_t)reclen; memcpy(rec + 16, &rl, 2);
        rec[18] = d.type == VN_DIR ? 4 : d.type == VN_DEV ? 2 : d.type == VN_BLK ? 6 : d.type == VN_LNK ? 10 : 8;
        memcpy(rec + 19, d.name, nl);
        if (copy_to_user(ubuf + o, rec, reclen) < 0) return o ? (int64_t)o : -EFAULT;
        o += reclen;
        f->pos++;
    }
    return (int64_t)o;
}

/* ---- poll ---- */
struct kpollfd { int32_t fd; int16_t events, revents; };
/* timeout_ms < 0: forever */
static int64_t do_poll(struct process *p, uint64_t ufds, uint64_t nfds, int64_t timeout_ms) {
    if (nfds > 256) return -EINVAL;
    struct kpollfd *fds = nfds ? kmalloc(nfds * sizeof *fds) : NULL;
    if (nfds && !fds) return -ENOMEM;
    if (nfds && copy_from_user(fds, ufds, nfds * sizeof *fds) < 0) { kfree(fds); return -EFAULT; }
    uint64_t deadline = timeout_ms > 0 ? time_ms() + (uint64_t)timeout_ms : 0;
    int64_t ready;
    for (;;) {
        uint64_t gen = poll_generation();
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            struct file *f = fd_get(p, fds[i].fd);
            if (!f) { fds[i].revents = POLLNVAL; ready++; continue; }
            int want = fds[i].events | POLLERR | POLLHUP;
            int ev = vfs_poll(f->vn, fds[i].events) & want;
            if (ev & POLLIN) ev |= fds[i].events & POLLRDNORM;
            if (ev & POLLOUT) ev |= fds[i].events & POLLWRNORM;
            fds[i].revents = (int16_t)ev;
            if (ev) ready++;
        }
        if (ready || timeout_ms == 0) break;
        if (signal_interrupted()) { kfree(fds); return -EINTR; }
        int64_t left = -1;
        if (timeout_ms > 0) { uint64_t now = time_ms(); if (now >= deadline) break; left = (int64_t)(deadline - now); }
        poll_wait(gen, left);
    }
    if (nfds && copy_to_user(ufds, fds, nfds * sizeof *fds) < 0) ready = -EFAULT;
    kfree(fds);
    return ready;
}

/* select() over fd_set bitmaps, through the same readiness checks */
static int64_t do_select(struct process *p, int n, uint64_t ur, uint64_t uw, uint64_t ue, int64_t timeout_ms) {
    if (n < 0) return -EINVAL;
    if (n > MAX_FDS) n = MAX_FDS;
    uint64_t in[3] = { 0 }, out[3];
    uint64_t ptrs[3] = { ur, uw, ue };
    size_t bytes = (size_t)(n + 7) / 8;
    for (int k = 0; k < 3; k++) if (ptrs[k] && copy_from_user(&in[k], ptrs[k], bytes) < 0) return -EFAULT;
    uint64_t mask = n >= 64 ? ~0ULL : (1ULL << n) - 1;
    for (int k = 0; k < 3; k++) in[k] &= mask;
    uint64_t deadline = timeout_ms > 0 ? time_ms() + (uint64_t)timeout_ms : 0;
    int64_t ready;
    for (;;) {
        uint64_t gen = poll_generation();
        ready = 0; out[0] = out[1] = out[2] = 0;
        for (int fd = 0; fd < n; fd++) {
            uint64_t b = 1ULL << fd;
            if (!((in[0] | in[1] | in[2]) & b)) continue;
            struct file *f = fd_get(p, fd);
            if (!f) return -EBADF;
            int ev = vfs_poll(f->vn, POLLIN | POLLOUT);
            if ((in[0] & b) && (ev & (POLLIN | POLLHUP | POLLERR))) { out[0] |= b; ready++; }
            if ((in[1] & b) && (ev & (POLLOUT | POLLERR))) { out[1] |= b; ready++; }
            if ((in[2] & b) && (ev & POLLPRI)) { out[2] |= b; ready++; }
        }
        if (ready || timeout_ms == 0) break;
        if (signal_interrupted()) return -EINTR;
        int64_t left = -1;
        if (timeout_ms > 0) { uint64_t now = time_ms(); if (now >= deadline) break; left = (int64_t)(deadline - now); }
        poll_wait(gen, left);
    }
    for (int k = 0; k < 3; k++) if (ptrs[k] && copy_to_user(ptrs[k], &out[k], bytes) < 0) return -EFAULT;
    return ready;
}

/* ---- time ---- */
struct kts { int64_t sec, nsec; };
static int64_t clock_ns(int clk) {
    switch (clk) {
    case 0: case 5: return time_realtime_ns();                 /* REALTIME, REALTIME_COARSE */
    case 1: case 4: case 6: case 7: return (int64_t)time_ns(); /* MONOTONIC, _RAW, _COARSE, BOOTTIME */
    case 2: { int64_t ms = 0; struct thread *t; procs_lock(); list_for_each_entry(t, &current_thread->proc->threads, proc_link) ms += (int64_t)t->cpu_ticks; procs_unlock(); return ms * 1000000; }
    case 3: return (int64_t)current_thread->cpu_ticks * 1000000;
    }
    return -1;
}
/* sleeps; returns 0, or -EINTR with the time left in *left_ns */
static int sleep_ns(int64_t ns, int64_t *left_ns) {
    uint64_t end = time_ns() + (uint64_t)MAX(ns, 0);
    for (;;) {
        uint64_t now = time_ns();
        if (now >= end) return 0;
        if (signal_interrupted()) { if (left_ns) *left_ns = (int64_t)(end - now); return -EINTR; }
        uint64_t rem = end - now;
        if (rem < 1000000) { while (time_ns() < end && !signal_interrupted()) pause(); continue; }   /* sub-millisecond: spin */
        thread_sleep_ms(rem / 1000000);
    }
}

/* ---- processes ---- */
static int64_t do_fork(struct regs *r, uint64_t flags, uint64_t newsp, uint64_t uptid, uint64_t uctid, uint64_t tls) {
    struct thread *t = current_thread;
    struct process *p = t->proc;
    if (process_user_count() >= USER_PROCS_MAX) return -EAGAIN;
    struct process *c = process_create(p->name, true);
    strlcpy(c->cwd, p->cwd, sizeof c->cwd);
    c->umask = p->umask;
    memcpy(c->sigact, p->sigact, sizeof c->sigact);
    c->execed = false;
    c->app_id = -1;
    procs_lock();
    for (int i = 0; i < MAX_FDS; i++) c->fds[i] = p->fds[i] ? vfs_dup(p->fds[i]) : NULL;
    c->fd_cloexec = p->fd_cloexec;
    procs_unlock();
    if (uvm_fork(p, c) < 0) {
        for (int i = 0; i < MAX_FDS; i++) if (c->fds[i]) { vfs_close(c->fds[i]); c->fds[i] = NULL; }
        process_mark_exited(c);
        process_wait(c->pid, NULL);
        return -ENOMEM;
    }
    struct regs cr = *r;
    cr.rax = 0;
    if (newsp) cr.rsp = newsp;
    uint64_t fl = irq_save(); fpu_save(t->fpu); irq_restore(fl);
    struct thread *ct = thread_create_user_regs(c, t->name, &cr, (flags & CLONE_SETTLS) ? tls : t->fs_base, t->sigmask, t->fpu);
    if (!ct) return -ENOMEM;
    if (flags & CLONE_CHILD_CLEARTID) ct->clear_child_tid = uctid;
    int pid = c->pid;
    if (flags & CLONE_PARENT_SETTID) put_user((int32_t)pid, uptid);
    /* vfork: the parent waits until the child has exec'd or exited */
    if (flags & CLONE_VFORK) {
        for (;;) {
            procs_lock();
            struct process *cc = process_find(pid);
            bool done = !cc || cc->exited || cc->execed;
            procs_unlock();
            if (done || t->kill_pending) break;
            thread_sleep_ms(1);
        }
    }
    return pid;
}

static int64_t do_clone_thread(struct regs *r, uint64_t flags, uint64_t newsp, uint64_t uptid, uint64_t uctid, uint64_t tls) {
    struct thread *t = current_thread;
    struct process *p = t->proc;
    if (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND) || !(flags & CLONE_FILES)) return -EINVAL;
    if (p->nthreads >= 256) return -EAGAIN;
    struct regs cr = *r;
    cr.rax = 0;
    if (newsp) cr.rsp = newsp;
    uint64_t fl = irq_save(); fpu_save(t->fpu); irq_restore(fl);
    struct thread *nt = thread_create_user_regs(p, t->name, &cr, (flags & CLONE_SETTLS) ? tls : t->fs_base, t->sigmask, t->fpu);
    if (!nt) return -ENOMEM;
    int tid = nt->tid;
    if (flags & CLONE_CHILD_CLEARTID) nt->clear_child_tid = uctid;
    if (flags & CLONE_PARENT_SETTID) put_user((int32_t)tid, uptid);
    if (flags & CLONE_CHILD_SETTID) put_user((int32_t)tid, uctid);
    return tid;
}

/* copies a NULL-terminated user array of strings into one kernel allocation */
static int copy_strv(uint64_t uv, char ***out, int *count, size_t *budget) {
    *out = NULL; *count = 0;
    if (!uv) { *out = kzalloc(sizeof(char *)); return *out ? 0 : -ENOMEM; }
    int n = 0;
    for (;; n++) {
        uint64_t ptr;
        if (n >= 4096) return -E2BIG_;
        if (copy_from_user(&ptr, uv + (uint64_t)n * 8, 8) < 0) return -EFAULT;
        if (!ptr) break;
    }
    char **v = kzalloc((size_t)(n + 1) * sizeof(char *));
    if (!v) return -ENOMEM;
    char *tmp = kmalloc(PATH_MAX * 4);
    if (!tmp) { kfree(v); return -ENOMEM; }
    for (int i = 0; i < n; i++) {
        uint64_t ptr;
        copy_from_user(&ptr, uv + (uint64_t)i * 8, 8);
        long l = strncpy_from_user(tmp, ptr, PATH_MAX * 4);
        if (l < 0 || (size_t)l + 1 > *budget) { for (int j = 0; j < i; j++) kfree(v[j]); kfree(v); kfree(tmp); return l < 0 && l != -36 ? (int)l : -E2BIG_; }
        *budget -= (size_t)l + 1;
        v[i] = strdup(tmp);
    }
    kfree(tmp);
    *out = v; *count = n;
    return 0;
}
static void free_strv(char **v, int n) { if (!v) return; for (int i = 0; i < n; i++) kfree(v[i]); kfree(v); }

static int64_t sys_execve(struct regs *r, struct process *p, int dirfd, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    char path[PATH_MAX];
    int e = resolve_at(p, dirfd, upath, path, false);
    if (e < 0) return e;
    size_t budget = 256 * 1024;
    char **argv, **envp; int argc, envc;
    if ((e = copy_strv(uargv, &argv, &argc, &budget)) < 0) return e;
    if ((e = copy_strv(uenvp, &envp, &envc, &budget)) < 0) { free_strv(argv, argc); return e; }
    struct regs nr;
    char *arg0[2] = { path, NULL };
    e = process_execve(path, argc ? argc : 1, argc ? argv : arg0, envc, envp, &nr);
    free_strv(argv, argc); free_strv(envp, envc);
    if (e < 0) return e;
    *r = nr;
    return 0;
}

/* user thread leaving: CLONE_CHILD_CLEARTID handshake (pthread_join waits on it) */
static void NORETURN thread_exit_user(int code) {
    struct thread *t = current_thread;
    if (t->clear_child_tid) { put_user((int32_t)0, t->clear_child_tid); futex_wake(t->clear_child_tid, 1); }
    if (t->proc->nthreads <= 1) process_exit(code);
    thread_exit(code);
}

/* ---- signals ---- */
static int64_t sys_rt_sigaction(struct process *p, int sig, uint64_t uact, uint64_t uold, size_t setsize) {
    if (sig < 1 || sig > NSIG || setsize != 8) return -EINVAL;
    struct k_sigaction old = p->sigact[sig];
    if (uact) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        struct k_sigaction a;
        if (copy_from_user(&a, uact, sizeof a) < 0) return -EFAULT;
        a.mask &= ~SIG_UNBLOCKABLE;
        p->sigact[sig] = a;
        /* setting SIG_IGN (or the default for an ignored signal) discards what is pending */
        if (a.handler == SIG_IGN || (a.handler == SIG_DFL && (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH)))
            __atomic_fetch_and(&p->sig_pending, ~SIGBIT(sig), __ATOMIC_SEQ_CST);
    }
    if (uold && copy_to_user(uold, &old, sizeof old) < 0) return -EFAULT;
    return 0;
}
static int64_t sys_rt_sigprocmask(struct thread *t, int how, uint64_t uset, uint64_t uold, size_t setsize) {
    if (setsize != 8) return -EINVAL;
    uint64_t old = t->sigmask;
    if (uset) {
        uint64_t s;
        if (copy_from_user(&s, uset, 8) < 0) return -EFAULT;
        if (how == SIG_BLOCK) t->sigmask |= s;
        else if (how == SIG_UNBLOCK) t->sigmask &= ~s;
        else if (how == SIG_SETMASK) t->sigmask = s;
        else return -EINVAL;
        t->sigmask &= ~SIG_UNBLOCKABLE;
    }
    if (uold && copy_to_user(uold, &old, 8) < 0) return -EFAULT;
    return 0;
}
static int64_t sys_kill(int pid, int sig) {
    struct process *self = current_thread->proc;
    if (sig < 0 || sig > NSIG) return -EINVAL;
    int sent = 0, found = 0;
    procs_lock();
    struct process *list[64];
    int n = process_list(list, 64);
    for (int i = 0; i < n; i++) {
        struct process *q = list[i];
        if (!q->is_user || q->exited) continue;
        bool match = pid > 0 ? q->pid == pid : pid == 0 ? q->pgid == self->pgid : pid == -1 ? q != self : q->pgid == -pid;
        if (!match) continue;
        found++;
        /* permission: the same user, or root */
        if (self->euid != 0 && self->uid != q->uid && self->euid != q->uid) continue;
        if (!sig || signal_send(q, sig)) sent++;
    }
    procs_unlock();
    if (!found) return -ESRCH;
    return sent ? 0 : -EPERM;
}
static int64_t sys_tgkill(int tgid, int tid, int sig) {
    if (sig < 0 || sig > NSIG) return -EINVAL;
    struct thread *t = thread_find(tid);
    if (!t || !t->is_user || (tgid > 0 && t->proc->pid != tgid)) return -ESRCH;
    if (sig) signal_send_thread(t, sig);
    return 0;
}

/* ---- identity ---- */
static int64_t set_ids(struct process *p, int r, int e, bool group) {
    int *ruid = group ? &p->gid : &p->uid, *eid = group ? &p->egid : &p->euid;
    bool root = p->euid == 0;
    if (r != -1 && !root && r != *ruid && r != *eid) return -EPERM;
    if (e != -1 && !root && e != *ruid && e != *eid) return -EPERM;
    if (r != -1) *ruid = r;
    if (e != -1) *eid = e;
    return 0;
}

/* ---- inFlux extensions ---- */
struct user_event { int32_t type, x, y, button, buttons, scroll, key; uint32_t ch; int32_t mods, cmd, w, h; };
struct user_procinfo { int32_t pid; char name[32]; int32_t cpu; uint64_t mem; int32_t threads; int32_t user; };
struct user_blkinfo { char name[16], model[48], fs[16]; uint64_t bytes; int32_t partition; };
struct user_isysinfo { uint64_t uptime_ms, total_mem, free_mem, used_mem; uint32_t procs, threads; };

void process_close_windows(struct process *p) { for (int i = 0; i < 8; i++) if (p->uwin[i]) { window_destroy(p->uwin[i]); p->uwin[i] = NULL; } }
static struct window *uwin_get(struct process *p, uint64_t id) { return id < 8 ? p->uwin[id] : NULL; }

static int64_t sys_spawn(struct process *p, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    char path[PATH_MAX];
    long l = strncpy_from_user(path, upath, sizeof path);
    if (l < 0) return l;
    char full[PATH_MAX];
    int nr = 0;
    if (strchr(path, '/')) nr = vfs_normalize(p->cwd, path, full, sizeof full); else snprintf(full, sizeof full, "/bin/%s", path);
    if (nr < 0) return nr;
    size_t budget = 256 * 1024;
    char **argv, **envp = NULL; int argc, envc = 0;
    int e = copy_strv(uargv, &argv, &argc, &budget);
    if (e < 0) return e;
    if (uenvp && (e = copy_strv(uenvp, &envp, &envc, &budget)) < 0) { free_strv(argv, argc); return e; }
    char *arg0[2] = { full, NULL };
    struct file *stdio[3] = { p->fds[0], p->fds[1], p->fds[2] };
    int r = process_spawn_env(full, argc ? argc : 1, argc ? argv : arg0, envp, stdio, p->cwd);
    if (r > 0) {
        /* a child of the caller, like fork+exec, so wait4 reaps it */
        procs_lock();
        struct process *c = process_find(r);
        if (c) { c->ppid = p->pid; c->parent = p; c->pgid = p->pgid; c->uid = p->uid; c->euid = p->euid; c->gid = p->gid; c->egid = p->egid; }
        procs_unlock();
    }
    free_strv(argv, argc); free_strv(envp, envc);
    return r;
}

/* a user program's menu bar: deep copy of its struct app_menus (same layout in user space) into one block */
static int64_t set_user_menus(struct window *win, uint64_t umenus, uint64_t count) {
    if (count > 16) return -EINVAL;
    struct menu m[16];
    if (count && copy_from_user(m, umenus, count * sizeof m[0]) < 0) return -EFAULT;
    size_t items = 0;
    for (uint64_t i = 0; i < count; i++) { if (m[i].count < 0 || m[i].count > 64) return -EINVAL; items += (size_t)m[i].count; }
    size_t strings = (count + items) * 64;
    uint8_t *blk = kzalloc(sizeof(struct app_menus) + count * sizeof(struct menu) + items * sizeof(struct menu_item) + strings);
    if (!blk) return -ENOMEM;
    struct app_menus *am = (struct app_menus *)blk;
    struct menu *km = (struct menu *)(am + 1);
    struct menu_item *ki = (struct menu_item *)(km + count);
    char *str = (char *)(ki + items);
    for (uint64_t i = 0; i < count; i++) {
        km[i].count = m[i].count; km[i].items = ki;
        km[i].title = str;
        if (strncpy_from_user(str, (uint64_t)m[i].title, 64) < 0) str[0] = 0;
        str += 64;
        if (m[i].count && copy_from_user(ki, (uint64_t)m[i].items, (size_t)m[i].count * sizeof *ki) < 0) { kfree(blk); return -EFAULT; }
        for (int k = 0; k < m[i].count; k++) {
            uint64_t ul = (uint64_t)ki[k].label;
            ki[k].label = NULL;
            if (!ki[k].separator && ul) { if (strncpy_from_user(str, ul, 64) < 0) str[0] = 0; ki[k].label = str; }
            str += 64;
        }
        ki += m[i].count;
    }
    am->menus = km; am->count = (int)count;
    void *old = win->owned_menus;
    win->owned_menus = blk;
    window_set_menus(win, am);
    kfree(old);
    return 0;
}

struct user_geom { int32_t x, y, w, h, top, flags; };
struct user_theme { int32_t dark, accent, clock_24h, pad; int64_t tz_offset_minutes; };
struct user_dialog { uint64_t title, message, extra, out; uint32_t outsz; int32_t destructive; };

static int64_t sys_win(struct process *p, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    case 0x1010: {   /* win_create(w, h, title, flags) */
        char title[128]; if (strncpy_from_user(title, a3, sizeof title) < 0) strlcpy(title, p->name, sizeof title);
        int w = (int)a1, h = (int)a2; if (w < 64 || h < 32 || w > 4096 || h > 4096) return -EINVAL;
        int flags = a4 ? (int)(a4 & (WIN_STANDARD | WIN_DIALOG | WIN_FULL_TITLE | WIN_UTILITY | WIN_NO_CTRL_ALIAS)) : WIN_STANDARD;
        int slot = -1; uint64_t used = 0, need = (uint64_t)w * (h + TITLEBAR_H) * 4;
        for (int i = 0; i < 8; i++) if (!p->uwin[i]) { if (slot < 0) slot = i; } else used += (uint64_t)MAX(p->uwin[i]->frame.w, 0) * MAX(p->uwin[i]->frame.h, 0) * 4;
        if (slot < 0 || used + need > USER_WIN_BYTES_MAX || need > pmm_free_bytes() / 4) return -ENOMEM;
        struct window *win = window_create_try(title, w, h, flags);
        if (!win) return -ENOMEM;
        if (!a4) win->flags |= WIN_NO_CTRL_ALIAS;   /* the original interface: raw keys for games and demos */
        p->uwin[slot] = win;
        return slot; }
    case 0x1015: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; window_show(win); return 0; }
    case 0x1011: {   /* win_present(id, pixels, w, h, dirty rect or 0) */
        struct window *win = uwin_get(p, a1); if (!win) return -EBADF;
        uint64_t sw = a3, sh = a4;
        if ((sw || sh) && (!sw || !sh || sw > 4096 || sh > 4096)) return -EINVAL;
        if (!(win->flags & WIN_VISIBLE)) window_show(win);
        struct canvas *cv = window_begin_draw(win);
        if (!sw) { sw = cv->w; sh = cv->h; }
        struct rect d = rect_make(0, 0, MIN((int)sw, cv->w), MIN((int)sh, cv->h));
        if (a5) { struct rect ur; if (copy_from_user(&ur, a5, sizeof ur) < 0) { window_end_draw(win, NULL); return -EFAULT; } d = rect_intersect(d, ur); }
        long e = 0;
        for (int y = d.y; y < d.y + d.h && !e; y++) e = copy_from_user(cv->px + (size_t)y * cv->stride + d.x, a2 + ((uint64_t)y * sw + (uint64_t)d.x) * 4, (size_t)d.w * 4);
        window_end_draw(win, a5 ? &d : NULL);
        return e; }
    case 0x1012: {   /* win_event(id, ev, timeout, path buffer, size) */
        struct window *win = uwin_get(p, a1); if (!win) return -EBADF;
        struct event ev; if (!window_wait_event(win, &ev, (int)(int64_t)a3)) return signal_interrupted() ? -EINTR : 0;
        struct user_event ue = { ev.type, ev.x, ev.y, ev.button, ev.buttons, ev.scroll, ev.key, ev.ch, ev.mods, ev.cmd, ev.w, ev.h };
        if (ev.type == EV_OPEN) {
            if (a4 && a5) { size_t l = MIN(strlen(ev.path) + 1, (size_t)a5); char z = 0; copy_to_user(a4, ev.path, l); copy_to_user(a4 + l - 1, &z, 1); }
            kfree(ev.path);
        }
        return copy_to_user(a2, &ue, sizeof ue) < 0 ? -EFAULT : 1; }
    case 0x1013: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; p->uwin[a1] = NULL; window_destroy(win); return 0; }
    case 0x1014: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; char t[128]; if (strncpy_from_user(t, a2, sizeof t) < 0) return -EFAULT; window_set_title(win, t); return 0; }
    case 0x1016: {   /* win_geometry(id, out) */
        struct window *win = uwin_get(p, a1); if (!win) return -EBADF;
        struct user_geom g = { win->frame.x, win->frame.y, win->frame.w, win->frame.h, window_content_top(win), win->flags };
        return copy_to_user(a2, &g, sizeof g) < 0 ? -EFAULT : 0; }
    case 0x1017: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; return set_user_menus(win, a2, a3); }
    case 0x1018: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; window_set_timer(win, (int)a2); return 0; }
    case 0x1019: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; window_set_min_size(win, (int)a2, (int)a3); return 0; }
    case 0x101A: {   /* win_post(id, type, cmd): wake the window's own event loop (worker threads) */
        struct window *win = uwin_get(p, a1); if (!win) return -EBADF;
        if (a2 != EV_USER && a2 != EV_MENU && a2 != EV_TIMER) return -EINVAL;
        struct event e = { .type = (int)a2, .cmd = (int)a3 };
        return window_post_event(win, &e) ? 0 : -EAGAIN; }
    case 0x101B: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; window_focus(win); return 0; }
    case 0x101C: { struct window *win = uwin_get(p, a1); if (!win) return -EBADF; window_resize(win, (int)a2, (int)a3); return 0; }
    }
    return -ENOSYS;
}

/* desktop services for user programs: theme, dialogs, clipboard, launching */
static int64_t sys_desktop(struct process *p, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    case 0x1020: { struct user_theme t = { theme.dark, theme.accent_index, clock_24h, 0, tz_offset_minutes }; return copy_to_user(a1, &t, sizeof t) < 0 ? -EFAULT : 0; }
    case 0x1021: {   /* dialog(kind, struct user_dialog *): 0 message, 1 confirm, 2 prompt; returns the answer */
        struct user_dialog d;
        if (copy_from_user(&d, a2, sizeof d) < 0) return -EFAULT;
        char title[128], extra[256] = "", *msg = kmalloc(2048);
        if (!msg) return -ENOMEM;
        if (strncpy_from_user(title, d.title, sizeof title) < 0 || strncpy_from_user(msg, d.message, 2048) < 0) { kfree(msg); return -EFAULT; }
        if (d.extra && strncpy_from_user(extra, d.extra, sizeof extra) < 0) extra[0] = 0;
        int64_t r = 0;
        if (a1 == 0) dialog_message(title, msg);
        else if (a1 == 1) r = dialog_confirm(title, msg, extra[0] ? extra : "OK", d.destructive != 0);
        else if (a1 == 2) {
            size_t n = CLAMP((size_t)d.outsz, 1, 1024);
            char *out = kzalloc(n);
            if (!out) { kfree(msg); return -ENOMEM; }
            r = dialog_prompt(title, msg, extra, out, n);
            if (r && copy_to_user(d.out, out, strlen(out) + 1) < 0) r = -EFAULT;
            kfree(out);
        } else r = -EINVAL;
        kfree(msg);
        return r; }
    case 0x1022: {   /* clipboard_set(text, n) */
        size_t n = MIN(a2, (uint64_t)(1 << 20));
        char *k = kmalloc(n + 1); if (!k) return -ENOMEM;
        if (copy_from_user(k, a1, n) < 0) { kfree(k); return -EFAULT; }
        clipboard_set(k, n); kfree(k); return 0; }
    case 0x1023: {   /* clipboard_get(buf, size): length (copies at most size-1 bytes and a NUL) */
        char *c = clipboard_get();
        if (!c) return 0;
        size_t l = strlen(c);
        if (a1 && a2) { size_t n = MIN(l, (size_t)a2 - 1); char z = 0; if (copy_to_user(a1, c, n) < 0 || copy_to_user(a1 + n, &z, 1) < 0) { kfree(c); return -EFAULT; } }
        kfree(c);
        return (int64_t)l; }
    case 0x1024: {   /* app_launch(id, arg) */
        char arg[PATH_MAX];
        if (a2 && strncpy_from_user(arg, a2, sizeof arg) < 0) return -EFAULT;
        return app_launch((int)a1, a2 ? arg : NULL); }
    case 0x1026: {   /* notify(title, body) */
        char t[64], b[200];
        if (strncpy_from_user(t, a1, sizeof t) < 0) return -EFAULT;
        if (a2 && strncpy_from_user(b, a2, sizeof b) < 0) return -EFAULT;
        notify_post(t, a2 ? b : "");
        return 0; }
    case 0x1025: { char path[PATH_MAX]; if (resolve_at(p, AT_FDCWD, a1, path, false) < 0) return -EFAULT; app_open_path(path); return 0; }
    }
    return -ENOSYS;
}

static int64_t sys_influx(struct process *p, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (nr) {
    case 0x1000: return sys_spawn(p, a1, a2, a3);
    case 0x1001: {   /* ps(out, max) */
        int max = (int)MIN(a2, 64ULL);
        struct user_procinfo *out = kzalloc(64 * sizeof *out);
        if (!out) return -ENOMEM;
        sched_sample_cpu();
        procs_lock();
        struct process *l[64]; int n = process_list(l, 64), k = 0;
        for (int i = 0; i < n && k < max; i++) {
            out[k].pid = l[i]->pid; strlcpy(out[k].name, l[i]->name, 32); out[k].user = l[i]->is_user;
            out[k].mem = l[i]->heap_bytes + uvm_resident_bytes(l[i]);
            int cpu = 0, nt = 0; struct thread *t;
            list_for_each_entry(t, &l[i]->threads, proc_link) { cpu += t->cpu_percent; nt++; }
            out[k].cpu = l[i]->pid == 1 ? 0 : cpu; out[k].threads = nt; k++;
        }
        procs_unlock();
        int64_t r = copy_to_user(a1, out, (size_t)k * sizeof *out) < 0 ? -EFAULT : k;
        kfree(out);
        return r; }
    case 0x1002: {   /* lsblk(out, max) */
        int max = (int)MIN(a2, 32ULL), n = 0;
        struct user_blkinfo *out = kzalloc(32 * sizeof *out);
        if (!out) return -ENOMEM;
        for (int i = 0; i < blockdev_count() && n < max; i++) {
            struct blockdev *b = blockdev_at(i); if (!b) continue;
            strlcpy(out[n].name, b->name, 16); strlcpy(out[n].model, b->parent ? b->parent->model : b->model, 48);
            strlcpy(out[n].fs, b->fs_type ? b->fs_type : "", 16); out[n].bytes = b->sectors * b->sector_size; out[n].partition = b->parent != NULL; n++;
        }
        int64_t r = copy_to_user(a1, out, (size_t)n * sizeof *out) < 0 ? -EFAULT : n;
        kfree(out);
        return r; }
    case 0x1004: {   /* netinfo(out): the default network adapter */
        struct { char name[8], driver[32]; uint32_t ip, netmask, gateway, dns; int32_t up, configured, present, pad; } ni;
        memset(&ni, 0, sizeof ni);
        struct netif *n = netif_default();
        if (n) { memcpy(ni.name, n->name, sizeof ni.name); memcpy(ni.driver, n->driver, sizeof ni.driver); ni.ip = n->ip; ni.netmask = n->netmask; ni.gateway = n->gateway; ni.dns = n->dns; ni.up = n->up; ni.configured = n->configured; ni.present = 1; }
        return copy_to_user(a1, &ni, sizeof ni) < 0 ? -EFAULT : 0; }
    case 0x1003: {   /* inFlux sysinfo */
        struct user_isysinfo si = { system_ticks, pmm_total_bytes(), pmm_free_bytes(), pmm_used_bytes(), 0, 0 };
        struct process *l[64]; si.procs = process_list(l, 64); struct thread *t[128]; si.threads = thread_list(t, 128);
        return copy_to_user(a1, &si, sizeof si) < 0 ? -EFAULT : 0; }
    }
    return -ENOSYS;
}

/* ---- the dispatcher ---- */
struct linux_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
struct linux_sysinfo { int64_t uptime; uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap; uint16_t procs, pad; uint32_t pad2; uint64_t totalhigh, freehigh; uint32_t mem_unit; char f[4]; };
struct linux_rlimit { uint64_t cur, max; };

static int64_t do_syscall(struct regs *r) {
    struct thread *t = current_thread;
    struct process *p = t->proc;
    uint64_t nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx, a4 = r->r10, a5 = r->r8, a6 = r->r9;
    char path[PATH_MAX], path2[PATH_MAX];
    int e;
    switch (nr) {
    /* files */
    case 0: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; return do_read(f, a2, a3, -1); }
    case 1: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; return do_write(f, a2, a3, -1); }
    case 2: case 257: case 85: {   /* open, openat, creat */
        int dirfd = nr == 257 ? (int)a1 : AT_FDCWD;
        uint64_t up = nr == 257 ? a2 : a1;
        int flags = nr == 257 ? (int)a3 : nr == 85 ? (O_CREAT | O_WRONLY | O_TRUNC) : (int)a2;
        if ((e = resolve_at(p, dirfd, up, path, false)) < 0) return e;
        struct file *f;
        if ((e = vfs_open_rel("/", path, flags & ~O_CLOEXEC, &f)) < 0) return e;
        return fd_install(p, f, flags & O_CLOEXEC, 0); }
    case 3: return fd_close(p, (int)a1);
    case 4: case 6: if ((e = resolve_at(p, AT_FDCWD, a1, path, false)) < 0) return e; return stat_path(p, path, a2, nr == 4);
    case 5: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; struct linux_stat ls; fill_stat(&ls, f->vn); return copy_to_user(a2, &ls, sizeof ls) < 0 ? -EFAULT : 0; }
    case 262: {   /* newfstatat */
        if ((e = resolve_at(p, (int)a1, a2, path, a4 & AT_EMPTY_PATH)) < 0) {
            if (e == -EBADF && (a4 & AT_EMPTY_PATH)) { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; struct linux_stat ls; fill_stat(&ls, f->vn); return copy_to_user(a3, &ls, sizeof ls) < 0 ? -EFAULT : 0; }
            return e;
        }
        return stat_path(p, path, a3, !(a4 & AT_SYMLINK_NOFOLLOW)); }
    case 8: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; if (f->vn->type == VN_DEV && !f->path) return -ESPIPE;
        if (f->vn->type == VN_DIR) { if ((int)a3 == SEEK_SET && (int64_t)a2 >= 0) { f->pos = a2; return (int64_t)a2; } return -EINVAL; }
        return vfs_seek(f, (int64_t)a2, (int)a3); }
    case 16: {   /* ioctl */
        struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF;
        if (a2 == 0x5421) { int on; if (copy_from_user(&on, a3, sizeof on) < 0) return -EFAULT; if (on) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK; return 0; }   /* FIONBIO */
        if (a2 == 0x5451) { p->fd_cloexec |= 1ULL << a1; return 0; }   /* FIOCLEX */
        if (a2 == 0x5450) { p->fd_cloexec &= ~(1ULL << a1); return 0; }   /* FIONCLEX */
        if (a2 < 0x100 || !f->vn->ops->ioctl) return -ENOTTY_;   /* the old numeric tty commands are kernel-only */
        return f->vn->ops->ioctl(f->vn, (int)a2, (void *)a3); }
    case 17: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; if ((int64_t)a4 < 0) return -EINVAL; return do_read(f, a2, a3, (int64_t)a4); }
    case 18: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; if ((int64_t)a4 < 0) return -EINVAL; return do_write(f, a2, a3, (int64_t)a4); }
    case 19: case 20: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; return do_rwv(f, a2, (int)a3, nr == 20, -1); }
    case 295: case 296: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; return do_rwv(f, a2, (int)a3, nr == 296, (int64_t)a4); }
    case 21: case 269: case 439: {   /* access, faccessat, faccessat2 */
        if ((e = resolve_at(p, nr == 21 ? AT_FDCWD : (int)a1, nr == 21 ? a1 : a2, path, false)) < 0) return e;
        struct vnode *v = vfs_lookup(path); if (!v) return -ENOENT; vnode_unref(v); return 0; }
    case 22: case 293: {   /* pipe, pipe2 */
        struct file *rd, *wr; pipe_create(&rd, &wr);
        int fl = nr == 293 ? (int)a2 : 0;
        if (fl & O_NONBLOCK) { rd->flags |= O_NONBLOCK; wr->flags |= O_NONBLOCK; }
        int fr = fd_install(p, rd, fl & O_CLOEXEC, 0); if (fr < 0) { vfs_close(wr); return fr; }
        int fw = fd_install(p, wr, fl & O_CLOEXEC, 0); if (fw < 0) { fd_close(p, fr); return fw; }
        int fds[2] = { fr, fw };
        if (copy_to_user(a1, fds, sizeof fds) < 0) { fd_close(p, fr); fd_close(p, fw); return -EFAULT; }
        return 0; }
    case 32: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; return fd_install(p, vfs_dup(f), false, 0); }
    case 33: case 292: {   /* dup2, dup3 */
        struct file *f = fd_get(p, (int)a1); if (!f || a2 >= MAX_FDS) return -EBADF;
        if (a1 == a2) return nr == 292 ? -EINVAL : (int64_t)a2;
        struct file *nf = vfs_dup(f);
        if (p->fds[a2]) vfs_close(p->fds[a2]);
        p->fds[a2] = nf;
        if (nr == 292 && (a3 & O_CLOEXEC)) p->fd_cloexec |= 1ULL << a2; else p->fd_cloexec &= ~(1ULL << a2);
        return (int64_t)a2; }
    case 72: {   /* fcntl */
        struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF;
        switch ((int)a2) {
        case F_DUPFD: case F_DUPFD_CLOEXEC: return fd_install(p, vfs_dup(f), a2 == F_DUPFD_CLOEXEC, (int)a3);
        case F_GETFD: return (p->fd_cloexec >> a1) & 1;
        case F_SETFD: if (a3 & FD_CLOEXEC) p->fd_cloexec |= 1ULL << a1; else p->fd_cloexec &= ~(1ULL << a1); return 0;
        case F_GETFL: return f->flags & ~O_CLOEXEC;
        case F_SETFL: f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) | ((int)a3 & (O_NONBLOCK | O_APPEND)); return 0;
        case F_GETLK: { struct { int16_t type, whence; int64_t start, len; int32_t pid; } fl; if (copy_from_user(&fl, a3, sizeof fl) < 0) return -EFAULT; fl.type = 2 /* F_UNLCK */; return copy_to_user(a3, &fl, sizeof fl) < 0 ? -EFAULT : 0; }
        case F_SETLK: case F_SETLKW: return 0;   /* advisory locks: nothing else locks, so every lock is granted */
        }
        return -EINVAL; }
    case 73: return 0;   /* flock */
    case 74: case 75: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; return f->vn->ops->sync ? f->vn->ops->sync(f->vn) : 0; }
    case 76: { if ((e = resolve_at(p, AT_FDCWD, a1, path, false)) < 0) return e; struct vnode *v = vfs_lookup(path); if (!v) return -ENOENT; e = vfs_truncate(v, a2); vnode_unref(v); return e; }
    case 77: { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; if ((f->flags & O_ACCMODE) == O_RDONLY) return -EINVAL; return vfs_truncate(f->vn, a2); }
    case 217: return sys_getdents64(p, (int)a1, a2, a3);
    case 79: { size_t l = strlen(p->cwd) + 1; if (a2 < l) return -ERANGE; return copy_to_user(a1, p->cwd, l) < 0 ? -EFAULT : (int64_t)l; }
    case 80: case 81: {   /* chdir, fchdir */
        if (nr == 81) { struct file *f = fd_get(p, (int)a1); if (!f) return -EBADF; if (f->vn->type != VN_DIR || !f->path) return -ENOTDIR; strlcpy(p->cwd, f->path, sizeof p->cwd); return 0; }
        if ((e = resolve_at(p, AT_FDCWD, a1, path, false)) < 0) return e;
        struct stat st; if (vfs_stat(path, &st) < 0) return -ENOENT; if (st.type != VN_DIR) return -ENOTDIR;
        strlcpy(p->cwd, path, sizeof p->cwd); return 0; }
    case 82: case 264: case 316: {   /* rename, renameat, renameat2 */
        int d1 = nr == 82 ? AT_FDCWD : (int)a1, d2 = nr == 82 ? AT_FDCWD : (int)a3;
        if ((e = resolve_at(p, d1, nr == 82 ? a1 : a2, path, false)) < 0) return e;
        if ((e = resolve_at(p, d2, nr == 82 ? a2 : a4, path2, false)) < 0) return e;
        return vfs_rename(path, path2); }
    case 83: case 258: if ((e = resolve_at(p, nr == 83 ? AT_FDCWD : (int)a1, nr == 83 ? a1 : a2, path, false)) < 0) return e; return vfs_mkdir(path);
    case 84: case 87: case 263: {   /* rmdir, unlink, unlinkat */
        if ((e = resolve_at(p, nr == 263 ? (int)a1 : AT_FDCWD, nr == 263 ? a2 : a1, path, false)) < 0) return e;
        struct stat st; if (vfs_lstat(path, &st) < 0) return -ENOENT;
        bool want_dir = nr == 84 || (nr == 263 && (a3 & AT_REMOVEDIR));
        if (want_dir && st.type != VN_DIR) return -ENOTDIR;
        if (!want_dir && st.type == VN_DIR) return -EISDIR;
        return vfs_unlink(path); }
    case 89: case 267: {   /* readlink, readlinkat */
        if ((e = resolve_at(p, nr == 89 ? AT_FDCWD : (int)a1, nr == 89 ? a1 : a2, path, false)) < 0) return e;
        uint64_t ubuf = nr == 89 ? a2 : a3; size_t n = MIN(nr == 89 ? a3 : a4, (uint64_t)PATH_MAX);
        int l = vfs_readlink(path, path2, n);
        if (l < 0) return l;
        return copy_to_user(ubuf, path2, (size_t)l) < 0 ? -EFAULT : l; }
    case 88: case 266: {   /* symlink(target, path), symlinkat(target, dirfd, path) */
        long tl = strncpy_from_user(path2, a1, PATH_MAX);
        if (tl < 0) return tl;
        if ((e = resolve_at(p, nr == 88 ? AT_FDCWD : (int)a2, nr == 88 ? a2 : a3, path, false)) < 0) return e;
        return vfs_symlink(path2, path); }
    case 86: case 265: return -EPERM;   /* hard links */
    case 90: case 91: case 92: case 93: case 94: case 268: case 260: return 0;   /* chmod/chown family: permissions are not enforced yet */
    case 95: { uint32_t old = p->umask; p->umask = (uint32_t)a1 & 0777; return old; }
    case 162: vfs_sync(); return 0;
    case 137: case 138: {   /* statfs, fstatfs */
        struct { int64_t type, bsize, blocks, bfree, bavail, files, ffree; int32_t fsid[2]; int64_t namelen, frsize, flags, spare[4]; } sf;
        memset(&sf, 0, sizeof sf);
        sf.type = 0x9123683E; sf.bsize = sf.frsize = 4096; sf.namelen = NAME_MAX; sf.blocks = (int64_t)(pmm_total_bytes() / 4096); sf.bfree = sf.bavail = (int64_t)(pmm_free_bytes() / 4096);
        return copy_to_user(a2, &sf, sizeof sf) < 0 ? -EFAULT : 0; }
    /* poll */
    case 7: return do_poll(p, a1, a2, (int64_t)(int32_t)a3);
    case 271: {   /* ppoll */
        int64_t ms = -1;
        if (a3) { struct kts ts; if (copy_from_user(&ts, a3, sizeof ts) < 0) return -EFAULT; ms = ts.sec * 1000 + (ts.nsec + 999999) / 1000000; }
        if (a4) { uint64_t m; if (copy_from_user(&m, a4, 8) < 0) return -EFAULT; t->saved_sigmask = t->sigmask; t->restore_sigmask = true; t->sigmask = m & ~SIG_UNBLOCKABLE; }
        return do_poll(p, a1, a2, ms); }
    case 23: {   /* select */
        int64_t ms = -1;
        if (a5) { struct { int64_t sec, usec; } tv; if (copy_from_user(&tv, a5, sizeof tv) < 0) return -EFAULT; ms = tv.sec * 1000 + (tv.usec + 999) / 1000; }
        return do_select(p, (int)a1, a2, a3, a4, ms); }
    case 270: {   /* pselect6 */
        int64_t ms = -1;
        if (a5) { struct kts ts; if (copy_from_user(&ts, a5, sizeof ts) < 0) return -EFAULT; ms = ts.sec * 1000 + (ts.nsec + 999999) / 1000000; }
        return do_select(p, (int)a1, a2, a3, a4, ms); }
    /* memory */
    case 9: {   /* mmap */
        uint64_t len = a2; uint32_t prot = (uint32_t)a3, flags = (uint32_t)a4; int fd = (int)a5; uint64_t off = a6;
        if (!len || (off & (PAGE_SIZE - 1))) return -EINVAL;
        if (!(flags & (MAP_SHARED | MAP_PRIVATE))) return -EINVAL;
        if (flags & MAP_ANONYMOUS) return uvm_map(p, a1, len, prot, flags);
        /* file mappings: a private copy of the file's bytes (MAP_SHARED of a file is read-only here) */
        struct file *f = fd_get(p, fd); if (!f) return -EBADF;
        if (f->vn->type != VN_FILE) return -ENODEV;
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) return -ENOTSUP;
        int64_t addr = uvm_map(p, a1, len, prot | PROT_WRITE, (flags & ~MAP_SHARED) | MAP_PRIVATE);
        if (addr < 0) return addr;
        uint8_t *k = kmalloc(XFER_CHUNK);
        if (!k) { uvm_unmap(p, addr, len); return -ENOMEM; }
        for (uint64_t o = 0; o < len && off + o < f->vn->size; o += XFER_CHUNK) {
            int64_t got = vfs_pread(f->vn, off + o, k, MIN((uint64_t)XFER_CHUNK, len - o));
            if (got <= 0) break;
            if (copy_to_user((uint64_t)addr + o, k, (size_t)got) < 0) { kfree(k); uvm_unmap(p, addr, len); return -ENOMEM; }
        }
        kfree(k);
        if (!(prot & PROT_WRITE)) uvm_protect(p, (uint64_t)addr, len, prot);
        return addr; }
    case 10: return uvm_protect(p, a1, a2, (uint32_t)a3);
    case 11: return uvm_unmap(p, a1, a2);
    case 12: return uvm_brk(p, a1);
    case 25: return -ENOMEM;   /* mremap: callers fall back to mmap + copy */
    case 26: case 28: return 0;   /* msync, madvise */
    /* signals */
    case 13: return sys_rt_sigaction(p, (int)a1, a2, a3, a4);
    case 14: return sys_rt_sigprocmask(t, (int)a1, a2, a3, a4);
    case 15: return sys_rt_sigreturn(r);
    case 127: { uint64_t pend = (t->sig_pending | p->sig_pending) & t->sigmask; return copy_to_user(a1, &pend, 8) < 0 ? -EFAULT : 0; }
    case 130: {   /* rt_sigsuspend */
        uint64_t m; if (copy_from_user(&m, a1, 8) < 0) return -EFAULT;
        /* wait with the given mask; the handler's frame saves the old one, so its sigreturn restores it */
        t->saved_sigmask = t->sigmask; t->restore_sigmask = true;
        t->sigmask = m & ~SIG_UNBLOCKABLE;
        while (!signal_interrupted()) thread_sleep_ms(100);
        return -EINTR; }
    case 131: {   /* sigaltstack */
        struct { uint64_t sp; int32_t flags, pad; uint64_t size; } ss, old = { t->alt_sp, t->alt_size ? 0 : 2, 0, t->alt_size };
        if (a1) {
            if (copy_from_user(&ss, a1, sizeof ss) < 0) return -EFAULT;
            if (ss.flags & 2) { t->alt_sp = t->alt_size = 0; }
            else { if (ss.size < 2048) return -ENOMEM; t->alt_sp = ss.sp; t->alt_size = ss.size; }
        }
        if (a2 && copy_to_user(a2, &old, sizeof old) < 0) return -EFAULT;
        return 0; }
    case 62: return sys_kill((int)a1, (int)a2);
    case 200: return sys_tgkill(-1, (int)a1, (int)a2);
    case 234: return sys_tgkill((int)a1, (int)a2, (int)a3);
    case 34: while (!signal_interrupted()) thread_sleep_ms(1000); return -ERESTARTSYS;   /* pause */
    /* processes and threads */
    case 39: return p->pid;
    case 110: return p->ppid;
    case 186: return t->tid;
    case 57: case 58: return do_fork(r, nr == 58 ? CLONE_VFORK : 0, 0, 0, 0, 0);
    case 56: {   /* clone(flags, stack, parent_tid, child_tid, tls) */
        if (a1 & CLONE_THREAD) return do_clone_thread(r, a1, a2, a3, a4, a5);
        return do_fork(r, a1, a2, a3, a4, a5); }
    case 59: return sys_execve(r, p, AT_FDCWD, a1, a2, a3);
    case 322: return sys_execve(r, p, (int)a1, a2, a3, a4);
    case 60: thread_exit_user((int)(a1 & 0xFF));
    case 231: process_exit((int)(a1 & 0xFF));
    case 61: {   /* wait4 */
        int st = 0;
        int64_t res = process_wait_child((int)a1, &st, a3 & WNOHANG);
        if (res > 0 && a2 && copy_to_user(a2, &st, sizeof st) < 0) return -EFAULT;
        if (res > 0 && a4) { uint8_t ru[144]; memset(ru, 0, sizeof ru); copy_to_user(a4, ru, sizeof ru); }
        return res; }
    case 218: t->clear_child_tid = a1; return t->tid;
    case 273: case 274: return 0;   /* set/get_robust_list: futex owners that die are not tracked */
    case 334: return -ENOSYS;       /* rseq */
    case 158: {   /* arch_prctl */
        if (a1 == 0x1002) { if (a2 >= USER_MAX) return -EPERM; t->fs_base = a2; wrmsr(0xC0000100, a2); return 0; }
        if (a1 == 0x1003) return put_user(t->fs_base, a2) < 0 ? -EFAULT : 0;
        return -EINVAL; }
    case 157: {   /* prctl */
        if (a1 == 15) { char n[16]; if (strncpy_from_user(n, a2, sizeof n) < 0) return -EFAULT; strlcpy(t->name, n, sizeof t->name); return 0; }
        if (a1 == 16) return copy_to_user(a2, t->name, 16) < 0 ? -EFAULT : 0;
        return -EINVAL; }
    case 202: return sys_futex(a1, (int)a2, (uint32_t)a3, a4, a5, (uint32_t)a6);
    case 24: thread_yield(); return 0;
    case 204: { uint64_t mask = cpu_count >= 64 ? ~0ULL : (1ULL << cpu_count) - 1; if (a2 < 8) return -EINVAL; return copy_to_user(a3, &mask, 8) < 0 ? -EFAULT : 8; }
    case 203: return 0;   /* sched_setaffinity: one shared run queue */
    case 109: {   /* setpgid */
        int pid = a1 ? (int)a1 : p->pid, pg = a2 ? (int)a2 : pid;
        procs_lock(); struct process *q = process_find(pid);
        int res = !q ? -ESRCH : (q != p && q->ppid != p->pid) ? -ESRCH : 0;
        if (!res) q->pgid = pg;
        procs_unlock(); return res; }
    case 121: { if (!a1) return p->pgid; procs_lock(); struct process *q = process_find((int)a1); int g = q ? q->pgid : -ESRCH; procs_unlock(); return g; }
    case 111: return p->pgid;
    case 112: p->sid = p->pgid = p->pid; return p->pid;
    case 124: { if (!a1) return p->sid; procs_lock(); struct process *q = process_find((int)a1); int s = q ? q->sid : -ESRCH; procs_unlock(); return s; }
    /* identity */
    case 102: return p->uid;
    case 104: return p->gid;
    case 107: return p->euid;
    case 108: return p->egid;
    case 105: return p->euid == 0 ? (p->uid = p->euid = (int)a1, 0) : set_ids(p, -1, (int)a1, false);
    case 106: return p->euid == 0 ? (p->gid = p->egid = (int)a1, 0) : set_ids(p, -1, (int)a1, true);
    case 113: return set_ids(p, (int)a1, (int)a2, false);
    case 114: return set_ids(p, (int)a1, (int)a2, true);
    case 117: return set_ids(p, (int)a1, (int)a2, false);
    case 119: return set_ids(p, (int)a1, (int)a2, true);
    case 118: case 120: {
        int ids[3] = { nr == 118 ? p->uid : p->gid, nr == 118 ? p->euid : p->egid, nr == 118 ? p->euid : p->egid };
        if (put_user(ids[0], a1) < 0 || put_user(ids[1], a2) < 0 || put_user(ids[2], a3) < 0) return -EFAULT;
        return 0; }
    case 115: return 0;   /* getgroups: none */
    case 116: return p->euid == 0 ? 0 : -EPERM;
    /* time */
    case 228: { int64_t ns = clock_ns((int)a1); if (ns < 0) return -EINVAL; struct kts ts = { ns / 1000000000, ns % 1000000000 }; return copy_to_user(a2, &ts, sizeof ts) < 0 ? -EFAULT : 0; }
    case 229: { if (clock_ns((int)a1) < 0) return -EINVAL; struct kts ts = { 0, cpu_tsc_hz() ? 1 : 1000000 }; return a2 && copy_to_user(a2, &ts, sizeof ts) < 0 ? -EFAULT : 0; }
    case 96: { int64_t ns = time_realtime_ns(); struct { int64_t sec, usec; } tv = { ns / 1000000000, (ns % 1000000000) / 1000 }; if (a1 && copy_to_user(a1, &tv, sizeof tv) < 0) return -EFAULT; if (a2) { int32_t tz[2] = { -(int32_t)tz_offset_minutes, 0 }; copy_to_user(a2, tz, sizeof tz); } return 0; }
    case 201: { int64_t s = (int64_t)time_unix(); if (a1 && put_user(s, a1) < 0) return -EFAULT; return s; }
    case 35: case 230: {   /* nanosleep, clock_nanosleep */
        struct kts ts; uint64_t ureq = nr == 35 ? a1 : a3, urem = nr == 35 ? a2 : a4;
        if (copy_from_user(&ts, ureq, sizeof ts) < 0) return -EFAULT;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) return -EINVAL;
        int64_t ns = ts.sec * 1000000000 + ts.nsec;
        if (nr == 230 && (a2 & 1)) { int64_t now = clock_ns((int)a1); if (now < 0) return -EINVAL; ns -= now; urem = 0; }   /* TIMER_ABSTIME */
        int64_t left = 0;
        if (sleep_ns(ns, &left) == 0) return 0;
        if (urem) { struct kts rem = { left / 1000000000, left % 1000000000 }; copy_to_user(urem, &rem, sizeof rem); }
        return -EINTR; }
    case 100: {   /* times */
        int64_t ticks = (int64_t)(time_ms() / 10);
        if (a1) { int64_t tms[4] = { clock_ns(2) / 10000000, 0, 0, 0 }; if (copy_to_user(a1, tms, sizeof tms) < 0) return -EFAULT; }
        return ticks; }
    case 98: { uint8_t ru[144]; memset(ru, 0, sizeof ru); int64_t ns = clock_ns(2); int64_t tv[2] = { ns / 1000000000, (ns % 1000000000) / 1000 }; memcpy(ru, tv, sizeof tv); return copy_to_user(a2, ru, sizeof ru) < 0 ? -EFAULT : 0; }
    /* system */
    case 63: {
        struct linux_utsname u; memset(&u, 0, sizeof u);
        strcpy(u.sysname, "inFlux"); strlcpy(u.nodename, system_hostname, sizeof u.nodename); strcpy(u.release, INFLUX_VERSION);
        strcpy(u.version, "#1 SMP PREEMPT"); strcpy(u.machine, "x86_64"); strcpy(u.domainname, "(none)");
        return copy_to_user(a1, &u, sizeof u) < 0 ? -EFAULT : 0; }
    case 99: {
        struct linux_sysinfo si; memset(&si, 0, sizeof si);
        si.uptime = (int64_t)(time_ms() / 1000); si.totalram = pmm_total_bytes(); si.freeram = pmm_free_bytes(); si.mem_unit = 1;
        struct process *l[64]; si.procs = (uint16_t)process_list(l, 64);
        return copy_to_user(a1, &si, sizeof si) < 0 ? -EFAULT : 0; }
    case 97: case 302: {   /* getrlimit, prlimit64 */
        int res = nr == 97 ? (int)a1 : (int)a2; uint64_t uold = nr == 97 ? a2 : a4;
        if (nr == 302 && a1 && (int)a1 != p->pid) return -ESRCH;
        struct linux_rlimit rl = { ~0ULL, ~0ULL };
        if (res == 3) rl.cur = rl.max = 8ULL << 20;   /* RLIMIT_STACK */
        else if (res == 7) rl.cur = rl.max = MAX_FDS; /* RLIMIT_NOFILE */
        else if (res == 6) rl.cur = rl.max = USER_PROCS_MAX;   /* RLIMIT_NPROC */
        if (uold && copy_to_user(uold, &rl, sizeof rl) < 0) return -EFAULT;
        return 0; }
    case 160: return 0;   /* setrlimit: accepted */
    case 318: {   /* getrandom */
        size_t n = MIN(a2, (uint64_t)XFER_CHUNK);
        uint8_t *k = kmalloc(n ? n : 1); if (!k) return -ENOMEM;
        random_bytes(k, n);
        int64_t res = copy_to_user(a1, k, n) < 0 ? -EFAULT : (int64_t)n;
        memset(k, 0, n); kfree(k);
        return res; }
    /* sockets */
    case 41: return sys_socket((int)a1, (int)a2, (int)a3);
    case 53: return sys_socketpair((int)a1, (int)a2, (int)a3, a4);
    case 42: return sys_connect((int)a1, a2, (uint32_t)a3);
    case 43: return sys_accept4((int)a1, a2, a3, 0);
    case 288: return sys_accept4((int)a1, a2, a3, (int)a4);
    case 44: return sys_sendto((int)a1, a2, a3, (int)a4, a5, (uint32_t)a6);
    case 45: return sys_recvfrom((int)a1, a2, a3, (int)a4, a5, a6);
    case 46: return sys_sendmsg((int)a1, a2, (int)a3);
    case 47: return sys_recvmsg((int)a1, a2, (int)a3);
    case 48: return sys_shutdown((int)a1, (int)a2);
    case 49: return sys_bind((int)a1, a2, (uint32_t)a3);
    case 50: return sys_listen((int)a1, (int)a2);
    case 51: return sys_getsockname((int)a1, a2, a3, false);
    case 52: return sys_getsockname((int)a1, a2, a3, true);
    case 54: return sys_setsockopt((int)a1, (int)a2, (int)a3, a4, (uint32_t)a5);
    case 55: return sys_getsockopt((int)a1, (int)a2, (int)a3, a4, a5);
    }
    if (nr >= 0x1010 && nr < 0x1020) return sys_win(p, nr, a1, a2, a3, a4, a5);
    if (nr >= 0x1020 && nr < 0x1030) return sys_desktop(p, nr, a1, a2, a3, a4, a5);
    if (nr >= 0x1000 && nr < 0x1010) return sys_influx(p, nr, a1, a2, a3);
    return -ENOSYS;
}

/* returns nonzero when the exit must go through iretq (the full frame was replaced) */
uint64_t syscall_dispatch(struct regs *r) {
    struct thread *t = current_thread;
    t->uregs = r;
    t->iret_return = false;
    t->sys_nr = (int64_t)r->rax;
    int64_t res = do_syscall(r);
    if (t->sys_nr != 15) r->rax = (uint64_t)res;   /* rt_sigreturn has restored rax itself */
    if (t->kill_pending || process_has_signal(t->proc, t)) signal_deliver(r);
    if (t->restore_sigmask) { t->sigmask = t->saved_sigmask; t->restore_sigmask = false; }   /* no handler took the temporary mask */
    /* interrupted with no handler to run: restart it */
    if ((int64_t)r->rax == -ERESTARTSYS) { r->rax = (uint64_t)t->sys_nr; r->rip -= 2; }
    /* sysret to a non-canonical rip would fault in ring 0 on the user stack: iretq faults in user mode instead */
    if (r->rip >= USER_MAX) t->iret_return = true;
    t->uregs = NULL;
    return t->iret_return;
}

extern void syscall_entry(void);
/* every CPU, at bring-up: no user code runs before syscall_init() anyway */
void syscall_init_cpu(void) {
    wrmsr(0xC0000081, ((uint64_t)0x13 << 48) | ((uint64_t)KERNEL_CS << 32));   /* STAR */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);                                /* LSTAR */
    wrmsr(0xC0000084, 0x47700);                                                /* SFMASK: clear IF, TF, DF, IOPL, NT, AC */
}

void syscall_init(void) { kprintf("syscall: Linux x86-64 system call ABI on %d CPUs\n", cpu_count); }
