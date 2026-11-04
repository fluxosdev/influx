/*
 * BSD sockets for user programs: AF_INET stream (TCP) and datagram (UDP)
 * sockets over the kernel's TCP/IP stack, and AF_UNIX socketpair() as a
 * pair of cross-connected pipes. A socket is a file descriptor; read/write,
 * poll, fcntl(O_NONBLOCK) and close work on it like on any other.
 */
#include "net.h"
#include "syscall.h"
#include "signal.h"
#include "mm.h"
#include "string.h"
#include "tty.h"

#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_LINGER 13
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
#define MSG_PEEK 0x2
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000
#define EINPROGRESS 115
#define EALREADY 114
#define EISCONN 106
#define EDESTADDRREQ 89
#define EAFNOSUPPORT 97
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define ENOPROTOOPT 92
#define EMSGSIZE_ 90
#define EADDRNOTAVAIL_ 99

struct sockaddr_in { uint16_t family, port; uint32_t addr; uint8_t zero[8]; };

struct sock {
    int domain, type;
    struct tcp_conn *tcp;
    struct tcp_listener *lst;
    struct udp_sock *udp;
    uint32_t bind_ip; uint16_t bind_port;
    uint32_t peer_ip; uint16_t peer_port; bool udp_connected;
    int rcvtimeo, sndtimeo;          /* ms, 0 = none */
    int pending_err;                 /* SO_ERROR after a non-blocking connect */
    bool shut_wr;
    struct file *pr, *pw;            /* AF_UNIX pair: our read end and the peer's input */
    struct vnode vn;
};

static const struct vnode_ops sock_ops;
bool file_is_socket(struct file *f) { return f && f->vn->ops == &sock_ops; }
static struct sock *sock_of(struct process *p, int fd, int *err) {
    struct file *f = fd_get(p, fd);
    if (!f) { *err = -EBADF; return NULL; }
    if (f->vn->ops != &sock_ops) { *err = -ENOTSOCK; return NULL; }
    return f->vn->priv;
}
static bool nonblocking(struct process *p, int fd, int flags) { struct file *f = fd_get(p, fd); return (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK)); }

/* ---- vnode ops: read/write/poll on the descriptor ---- */
static int64_t sock_recv_k(struct sock *s, void *buf, size_t n, bool nb, uint32_t *from, uint16_t *fport) {
    if (s->pr) return vfs_read(s->pr, buf, n);
    if (s->tcp) return tcp_recv(s->tcp, buf, n, nb ? 0 : (s->rcvtimeo ? s->rcvtimeo : -1), NULL);
    if (s->udp) {
        for (;;) {
            int r = udp_recvfrom(s->udp, buf, n, from, fport, nb ? 0 : (s->rcvtimeo ? s->rcvtimeo : -1));
            if (r == -ETIMEDOUT) return -EAGAIN;
            if (r < 0) return r;
            /* a connected datagram socket only takes its peer's datagrams */
            if (s->udp_connected && from && (*from != s->peer_ip || *fport != s->peer_port)) continue;
            return r;
        }
    }
    return s->lst ? -EINVAL : -ENOTCONN;
}
static int64_t sock_send_k(struct sock *s, const void *buf, size_t n, bool nb, uint32_t ip, uint16_t port, bool nosignal) {
    if (s->pw) {
        int64_t r = vfs_write(s->pw, buf, n);
        return r;
    }
    if (s->tcp) {
        if (s->shut_wr) { if (!nosignal) signal_send_thread(current_thread, SIGPIPE); return -EPIPE; }
        int64_t r = tcp_send(s->tcp, buf, n, nb ? 0 : (s->sndtimeo ? s->sndtimeo : -1), NULL);
        if (r == -ENOTCONN || r == -ECONNRESET) { if (!nosignal) signal_send_thread(current_thread, SIGPIPE); return r == -ENOTCONN ? -EPIPE : r; }
        if (r == -ETIMEDOUT) return -EAGAIN;
        return r;
    }
    if (s->type == SOCK_DGRAM) {
        if (!ip) { if (!s->udp_connected) return -EDESTADDRREQ; ip = s->peer_ip; port = s->peer_port; }
        if (!s->udp && !(s->udp = udp_open(s->bind_port))) return -EADDRINUSE;
        if (n > ETH_MTU - 28) return -EMSGSIZE_;
        return udp_sendto(s->udp, ip, port, buf, n);
    }
    return s->lst ? -EINVAL : -ENOTCONN;
}
static int64_t sock_read(struct vnode *v, uint64_t off, void *buf, size_t n) { return sock_recv_k(v->priv, buf, n, false, NULL, NULL); }
static int64_t sock_write(struct vnode *v, uint64_t off, const void *buf, size_t n) { return sock_send_k(v->priv, buf, n, false, 0, 0, false); }
static int sock_poll(struct vnode *v, int ev) {
    struct sock *s = v->priv;
    if (s->pr) return vfs_poll(s->pr->vn, POLLIN) | (vfs_poll(s->pw->vn, POLLOUT) & (POLLOUT | POLLERR)) | ((vfs_poll(s->pw->vn, POLLOUT) & POLLERR) ? POLLHUP : 0);
    if (s->lst) return tcp_listener_ready(s->lst) ? POLLIN | POLLRDNORM : 0;
    if (s->tcp) return tcp_poll(s->tcp);
    if (s->udp) return udp_poll(s->udp);
    if (s->type == SOCK_DGRAM) return POLLOUT | POLLWRNORM;
    return POLLHUP;   /* an unconnected stream socket */
}
static int sock_ioctl(struct vnode *v, int cmd, void *arg) {
    struct sock *s = v->priv;
    if (cmd == 0x541B) {   /* FIONREAD */
        int n = 0;
        if (s->pr) { int k; if (s->pr->vn->ops->ioctl && s->pr->vn->ops->ioctl(s->pr->vn, cmd, &k) == 0) n = k; }
        else if (s->udp) n = udp_pending_bytes(s->udp);
        else if (s->tcp) n = (tcp_poll(s->tcp) & POLLIN) ? 1 : 0;
        return copy_to_user((uint64_t)arg, &n, sizeof n) < 0 ? -EFAULT : 0;
    }
    return -ENOTTY_;
}
static void sock_release(struct vnode *v) {
    struct sock *s = v->priv;
    if (s->tcp) tcp_close(s->tcp);
    if (s->lst) tcp_unlisten(s->lst);
    if (s->udp) udp_close(s->udp);
    if (s->pr) vfs_close(s->pr);
    if (s->pw) vfs_close(s->pw);
    kfree(s);
    poll_notify();
}
static const struct vnode_ops sock_ops = { sock_read, sock_write, NULL, NULL, NULL, NULL, NULL, sock_ioctl, sock_release, NULL, NULL, sock_poll };

static struct file *sock_file(struct sock *s, bool nonblock) {
    strcpy(s->vn.name, "socket");
    s->vn.type = VN_DEV; s->vn.ops = &sock_ops; s->vn.priv = s; s->vn.refcount = 1; s->vn.mode = 0140777;
    struct file *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->vn = &s->vn; f->flags = O_RDWR | (nonblock ? O_NONBLOCK : 0); f->refcount = 1;
    return f;
}

int64_t sys_socket(int domain, int type, int proto) {
    int base = type & 0xF;
    if (domain != AF_INET) return domain == AF_UNIX ? -EOPNOTSUPP : -EAFNOSUPPORT;
    if (base != SOCK_STREAM && base != SOCK_DGRAM) return -EPROTONOSUPPORT;
    if (proto && !(base == SOCK_STREAM && proto == 6) && !(base == SOCK_DGRAM && proto == 17)) return -EPROTONOSUPPORT;
    struct sock *s = kzalloc(sizeof *s);
    if (!s) return -ENOMEM;
    s->domain = domain; s->type = base;
    struct file *f = sock_file(s, type & SOCK_NONBLOCK);
    if (!f) { kfree(s); return -ENOMEM; }
    return fd_install(current_thread->proc, f, type & SOCK_CLOEXEC, 0);
}

int64_t sys_socketpair(int domain, int type, int proto, uint64_t usv) {
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if ((type & 0xF) != SOCK_STREAM) return -EOPNOTSUPP;
    struct file *ar, *aw, *br, *bw;
    pipe_create(&ar, &bw);   /* b writes, a reads */
    pipe_create(&br, &aw);   /* a writes, b reads */
    struct sock *a = kzalloc(sizeof *a), *b = kzalloc(sizeof *b);
    a->domain = b->domain = AF_UNIX; a->type = b->type = SOCK_STREAM;
    a->pr = ar; a->pw = aw; b->pr = br; b->pw = bw;
    struct process *p = current_thread->proc;
    int fa = fd_install(p, sock_file(a, type & SOCK_NONBLOCK), type & SOCK_CLOEXEC, 0);
    if (fa < 0) { vfs_close(br); vfs_close(bw); kfree(b); return fa; }
    int fb = fd_install(p, sock_file(b, type & SOCK_NONBLOCK), type & SOCK_CLOEXEC, 0);
    if (fb < 0) { vfs_close(p->fds[fa]); p->fds[fa] = NULL; return fb; }
    int sv[2] = { fa, fb };
    if (copy_to_user(usv, sv, sizeof sv) < 0) return -EFAULT;
    return 0;
}

static int read_addr(uint64_t uaddr, uint32_t len, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in a;
    if (len < sizeof a - 8) return -EINVAL;
    memset(&a, 0, sizeof a);
    if (copy_from_user(&a, uaddr, MIN(len, sizeof a)) < 0) return -EFAULT;
    if (a.family != AF_INET) return -EAFNOSUPPORT;
    *ip = ntohl(a.addr); *port = ntohs(a.port);
    return 0;
}
static int write_addr(uint64_t uaddr, uint64_t ulen, uint32_t ip, uint16_t port) {
    if (!uaddr) return 0;
    uint32_t len;
    if (copy_from_user(&len, ulen, sizeof len) < 0) return -EFAULT;
    struct sockaddr_in a = { AF_INET, htons(port), htonl(ip), { 0 } };
    if (copy_to_user(uaddr, &a, MIN(len, sizeof a)) < 0) return -EFAULT;
    len = sizeof a;
    return copy_to_user(ulen, &len, sizeof len) < 0 ? -EFAULT : 0;
}

int64_t sys_bind(int fd, uint64_t uaddr, uint32_t len) {
    int e = 0; struct sock *s = sock_of(current_thread->proc, fd, &e); if (!s) return e;
    if (s->pr) return -EINVAL;
    uint32_t ip; uint16_t port;
    if ((e = read_addr(uaddr, len, &ip, &port)) < 0) return e;
    if (s->bind_port || s->tcp || s->lst) return -EINVAL;
    if (ip && ip != 0x7F000001) {   /* an address this machine has */
        bool ours = false;
        for (int i = 0; i < netif_count(); i++) if (netif_at(i)->ip == ip) ours = true;
        if (!ours) return -EADDRNOTAVAIL_;
    }
    s->bind_ip = ip; s->bind_port = port;
    if (s->type == SOCK_DGRAM) {
        s->udp = udp_open(port);
        if (!s->udp) return -EADDRINUSE;
        s->bind_port = udp_port(s->udp);
    }
    return 0;
}

int64_t sys_listen(int fd, int backlog) {
    int e = 0; struct sock *s = sock_of(current_thread->proc, fd, &e); if (!s) return e;
    if (s->type != SOCK_STREAM || s->tcp || s->pr) return -EOPNOTSUPP;
    if (s->lst) return 0;
    s->lst = tcp_listen(s->bind_ip, s->bind_port, backlog > 0 ? backlog : 8, &e);
    if (!s->lst) return -e;
    s->bind_port = tcp_listener_port(s->lst);
    return 0;
}

int64_t sys_accept4(int fd, uint64_t uaddr, uint64_t ulen, int flags) {
    struct process *p = current_thread->proc;
    int e = 0; struct sock *s = sock_of(p, fd, &e); if (!s) return e;
    if (!s->lst) return -EINVAL;
    struct tcp_conn *c = tcp_accept(s->lst, nonblocking(p, fd, 0) ? 0 : (s->rcvtimeo ? s->rcvtimeo : -1), &e);
    if (!c) return e == ETIMEDOUT ? -EAGAIN : e == ECANCELED ? -EINTR : -e;
    struct sock *n = kzalloc(sizeof *n);
    if (!n) { tcp_abort(c); return -ENOMEM; }
    n->domain = AF_INET; n->type = SOCK_STREAM; n->tcp = c;
    uint32_t rip; uint16_t rport;
    tcp_endpoints(c, &n->bind_ip, &n->bind_port, &rip, &rport);
    n->peer_ip = rip; n->peer_port = rport;
    if ((e = write_addr(uaddr, ulen, rip, rport)) < 0) { tcp_abort(c); n->tcp = NULL; kfree(n); return e; }
    struct file *f = sock_file(n, flags & SOCK_NONBLOCK);
    return fd_install(p, f, flags & SOCK_CLOEXEC, 0);
}

int64_t sys_connect(int fd, uint64_t uaddr, uint32_t len) {
    struct process *p = current_thread->proc;
    int e = 0; struct sock *s = sock_of(p, fd, &e); if (!s) return e;
    uint32_t ip; uint16_t port;
    if ((e = read_addr(uaddr, len, &ip, &port)) < 0) return e;
    if (s->type == SOCK_DGRAM) {
        s->peer_ip = ip; s->peer_port = port; s->udp_connected = ip != 0;
        if (!s->udp && !(s->udp = udp_open(s->bind_port))) return -EADDRINUSE;
        return 0;
    }
    if (s->lst || s->pr) return -EINVAL;
    if (s->tcp) {
        if (tcp_connecting(s->tcp)) return -EALREADY;
        return -EISCONN;
    }
    if (!ip) ip = 0x7F000001;   /* INADDR_ANY: this machine */
    s->tcp = tcp_connect_start(ip, port, s->bind_port, &e);
    if (!s->tcp) return -e;
    s->peer_ip = ip; s->peer_port = port;
    if (nonblocking(p, fd, 0)) return -EINPROGRESS;
    uint64_t deadline = s->sndtimeo ? time_ms() + s->sndtimeo : 0;
    for (;;) {
        uint64_t gen = poll_generation();
        if (!tcp_connecting(s->tcp)) break;
        if (signal_interrupted()) return -EINTR;
        if (deadline && time_ms() >= deadline) return -EINPROGRESS;
        poll_wait(gen, 100);
    }
    e = tcp_error(s->tcp);
    if (e) { tcp_close(s->tcp); s->tcp = NULL; return -e; }
    return 0;
}

static int64_t do_send(int fd, uint64_t buf, size_t len, int flags, uint32_t ip, uint16_t port) {
    struct process *p = current_thread->proc;
    int e = 0; struct sock *s = sock_of(p, fd, &e); if (!s) return e;
    bool nb = nonblocking(p, fd, flags);
    size_t done = 0;
    uint8_t *k = kmalloc(MIN(len, (size_t)XFER_CHUNK) + 1);
    if (!k) return -ENOMEM;
    do {
        size_t n = MIN(len - done, (size_t)XFER_CHUNK);
        if (copy_from_user(k, buf + done, n) < 0) { kfree(k); return done ? (int64_t)done : -EFAULT; }
        int64_t r = sock_send_k(s, k, n, nb, ip, port, flags & MSG_NOSIGNAL);
        if (r < 0) { kfree(k); return done ? (int64_t)done : r; }
        done += (size_t)r;
        if ((size_t)r < n || s->type == SOCK_DGRAM) break;
    } while (done < len);
    kfree(k);
    return (int64_t)done;
}

int64_t sys_sendto(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint32_t alen) {
    uint32_t ip = 0; uint16_t port = 0;
    if (uaddr) { int e = read_addr(uaddr, alen, &ip, &port); if (e < 0) return e; }
    return do_send(fd, buf, len, flags, ip, port);
}

int64_t sys_recvfrom(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint64_t ualen) {
    struct process *p = current_thread->proc;
    int e = 0; struct sock *s = sock_of(p, fd, &e); if (!s) return e;
    size_t n = MIN(len, (size_t)XFER_CHUNK);
    uint8_t *k = kmalloc(n + 1);
    if (!k) return -ENOMEM;
    uint32_t from = 0; uint16_t fport = 0;
    int64_t r = sock_recv_k(s, k, n, nonblocking(p, fd, flags), &from, &fport);
    if (r > 0 && copy_to_user(buf, k, (size_t)r) < 0) r = -EFAULT;
    kfree(k);
    if (r >= 0 && uaddr) {
        if (s->tcp) { from = s->peer_ip; fport = s->peer_port; }
        if ((e = write_addr(uaddr, ualen, from, fport)) < 0) return e;
    }
    return r;
}

struct kiovec { uint64_t base, len; };
struct kmsghdr { uint64_t name; uint32_t namelen, pad; uint64_t iov; uint64_t iovlen; uint64_t control, controllen; int32_t flags, pad2; };

int64_t sys_sendmsg(int fd, uint64_t umsg, int flags) {
    struct kmsghdr m;
    if (copy_from_user(&m, umsg, sizeof m) < 0) return -EFAULT;
    if (m.iovlen > 64) return -EINVAL;
    uint32_t ip = 0; uint16_t port = 0;
    if (m.name) { int e = read_addr(m.name, m.namelen, &ip, &port); if (e < 0) return e; }
    /* gather into one buffer, so a datagram stays one datagram */
    size_t total = 0;
    struct kiovec iov[64];
    if (copy_from_user(iov, m.iov, m.iovlen * sizeof iov[0]) < 0) return -EFAULT;
    for (uint64_t i = 0; i < m.iovlen; i++) { total += iov[i].len; if (total > (1u << 20)) return -EMSGSIZE_; }
    uint8_t *k = kmalloc(total + 1);
    if (!k) return -ENOMEM;
    size_t o = 0;
    for (uint64_t i = 0; i < m.iovlen; i++) { if (copy_from_user(k + o, iov[i].base, iov[i].len) < 0) { kfree(k); return -EFAULT; } o += iov[i].len; }
    struct process *p = current_thread->proc;
    int e = 0; struct sock *s = sock_of(p, fd, &e); if (!s) { kfree(k); return e; }
    int64_t r = sock_send_k(s, k, total, nonblocking(p, fd, flags), ip, port, flags & MSG_NOSIGNAL);
    kfree(k);
    return r;
}

int64_t sys_recvmsg(int fd, uint64_t umsg, int flags) {
    struct kmsghdr m;
    if (copy_from_user(&m, umsg, sizeof m) < 0) return -EFAULT;
    if (m.iovlen > 64) return -EINVAL;
    struct kiovec iov[64];
    if (copy_from_user(iov, m.iov, m.iovlen * sizeof iov[0]) < 0) return -EFAULT;
    size_t total = 0;
    for (uint64_t i = 0; i < m.iovlen; i++) total += iov[i].len;
    total = MIN(total, (size_t)XFER_CHUNK);
    struct process *p = current_thread->proc;
    int e = 0; struct sock *s = sock_of(p, fd, &e); if (!s) return e;
    uint8_t *k = kmalloc(total + 1);
    if (!k) return -ENOMEM;
    uint32_t from = 0; uint16_t fport = 0;
    int64_t r = sock_recv_k(s, k, total, nonblocking(p, fd, flags), &from, &fport);
    if (r > 0) {
        size_t o = 0;
        for (uint64_t i = 0; i < m.iovlen && o < (size_t)r; i++) {
            size_t n = MIN(iov[i].len, (size_t)r - o);
            if (copy_to_user(iov[i].base, k + o, n) < 0) { kfree(k); return -EFAULT; }
            o += n;
        }
    }
    kfree(k);
    if (r >= 0) {
        if (m.name && m.namelen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in a = { AF_INET, htons(s->tcp ? s->peer_port : fport), htonl(s->tcp ? s->peer_ip : from), { 0 } };
            if (copy_to_user(m.name, &a, sizeof a) < 0) return -EFAULT;
            uint32_t nl = sizeof a;
            copy_to_user(umsg + offsetof(struct kmsghdr, namelen), &nl, sizeof nl);
        }
        uint64_t zero = 0;
        copy_to_user(umsg + offsetof(struct kmsghdr, controllen), &zero, sizeof zero);
        int32_t mf = 0;
        copy_to_user(umsg + offsetof(struct kmsghdr, flags), &mf, sizeof mf);
    }
    return r;
}

int64_t sys_shutdown(int fd, int how) {
    int e = 0; struct sock *s = sock_of(current_thread->proc, fd, &e); if (!s) return e;
    if (how < 0 || how > 2) return -EINVAL;
    if (s->pr) { if (how != 0 && s->pw) { vfs_close(s->pw); s->pw = NULL; } return 0; }
    if (!s->tcp) return -ENOTCONN;
    if (how != 0) { s->shut_wr = true; tcp_shutdown_write(s->tcp); }
    return 0;
}

int64_t sys_getsockname(int fd, uint64_t uaddr, uint64_t ulen, bool peer) {
    int e = 0; struct sock *s = sock_of(current_thread->proc, fd, &e); if (!s) return e;
    uint32_t ip = s->bind_ip; uint16_t port = s->bind_port;
    if (s->tcp) { uint32_t lip, rip; uint16_t lp, rp; tcp_endpoints(s->tcp, &lip, &lp, &rip, &rp); if (peer) { ip = rip; port = rp; } else { ip = lip; port = lp; } }
    else if (peer) { if (!s->udp_connected) return -ENOTCONN; ip = s->peer_ip; port = s->peer_port; }
    else if (s->udp) port = udp_port(s->udp);
    return write_addr(uaddr, ulen, ip, port);
}

int64_t sys_setsockopt(int fd, int level, int opt, uint64_t uval, uint32_t len) {
    int e = 0; struct sock *s = sock_of(current_thread->proc, fd, &e); if (!s) return e;
    if (level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO)) {
        struct { int64_t sec, usec; } tv;
        if (len < sizeof tv || copy_from_user(&tv, uval, sizeof tv) < 0) return -EFAULT;
        int ms = (int)MIN(tv.sec * 1000 + tv.usec / 1000, 0x7FFFFFFFLL);
        if (opt == SO_RCVTIMEO) s->rcvtimeo = ms; else s->sndtimeo = ms;
        return 0;
    }
    /* accepted and not needed: the stack has no Nagle delay, no keepalives, and addresses are reusable at once */
    if (level == SOL_SOCKET || (level == IPPROTO_TCP && opt == TCP_NODELAY) || level == 0 /* IPPROTO_IP */) return 0;
    return -ENOPROTOOPT;
}

int64_t sys_getsockopt(int fd, int level, int opt, uint64_t uval, uint64_t ulen) {
    int e = 0; struct sock *s = sock_of(current_thread->proc, fd, &e); if (!s) return e;
    int v = 0;
    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_ERROR: v = s->tcp ? tcp_error(s->tcp) : s->pending_err; s->pending_err = 0; break;
        case SO_TYPE: v = s->type; break;
        case SO_RCVBUF: case SO_SNDBUF: v = 65536; break;
        default: v = 0; break;
        }
    } else if (level == IPPROTO_TCP && opt == TCP_NODELAY) v = 1;
    else return -ENOPROTOOPT;
    uint32_t len;
    if (copy_from_user(&len, ulen, sizeof len) < 0) return -EFAULT;
    if (copy_to_user(uval, &v, MIN(len, sizeof v)) < 0) return -EFAULT;
    len = sizeof v;
    return copy_to_user(ulen, &len, sizeof len) < 0 ? -EFAULT : 0;
}
