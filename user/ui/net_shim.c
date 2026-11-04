/*
 * The kernel's client networking API (net.h: dns_resolve, tcp_connect,
 * tcp_send, tcp_recv, tcp_close) for kernel sources compiled into user
 * programs (the HTTP client), on top of BSD sockets. Waits poll in short
 * steps so the caller's cancel flag stops them, as in the kernel.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define ECANCELED_ 125
struct tcp_conn { int fd; };

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }

void ip_to_str(uint32_t ip, char *out) { snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF); }

/* ip in host byte order, like the kernel */
int dns_resolve(const char *name, uint32_t *ip, int timeout_ms, volatile bool *cancel) {
    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) { *ip = ntohl(a.s_addr); return 0; }
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(name, NULL, &hints, &res);
    if (e || !res) return e == EAI_NONAME || e == EAI_NODATA ? -ENOENT : e == EAI_AGAIN ? -ETIMEDOUT : -EIO;
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 0;
}

/* waits for ev on fd; 0 ready, -ETIMEDOUT, -ECANCELED */
static int wait_fd(int fd, short ev, uint64_t deadline, volatile bool *cancel) {
    for (;;) {
        if (cancel && *cancel) return -ECANCELED_;
        uint64_t now = now_ms();
        if (deadline && now >= deadline) return -ETIMEDOUT;
        int step = deadline ? (int)(deadline - now < 100 ? deadline - now : 100) : 100;
        struct pollfd p = { fd, ev, 0 };
        int r = poll(&p, 1, step);
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) return -errno;
    }
}

struct tcp_conn *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms, volatile bool *cancel, int *err) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { *err = errno; return NULL; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip) };
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) { *err = errno; close(fd); return NULL; }
    int w = wait_fd(fd, POLLOUT, now_ms() + (uint64_t)timeout_ms, cancel);
    if (w < 0) { *err = -w; close(fd); return NULL; }
    int soerr = 0; socklen_t l = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l);
    if (soerr) { *err = soerr; close(fd); return NULL; }
    struct tcp_conn *c = malloc(sizeof *c);
    if (!c) { *err = ENOMEM; close(fd); return NULL; }
    c->fd = fd;
    *err = 0;
    return c;
}

int64_t tcp_send(struct tcp_conn *c, const void *data, size_t len, int timeout_ms, volatile bool *cancel) {
    const char *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t r = send(c->fd, p + done, len - done, MSG_NOSIGNAL);
        if (r > 0) { done += (size_t)r; continue; }
        if (r < 0 && errno != EAGAIN && errno != EINTR) return -errno;
        int w = wait_fd(c->fd, POLLOUT, timeout_ms >= 0 ? now_ms() + (uint64_t)timeout_ms : 0, cancel);
        if (w < 0) return w;
    }
    return (int64_t)done;
}

int64_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, int timeout_ms, volatile bool *cancel) {
    for (;;) {
        ssize_t r = recv(c->fd, buf, len, 0);
        if (r >= 0) return r;
        if (errno != EAGAIN && errno != EINTR) return -errno;
        int w = wait_fd(c->fd, POLLIN, timeout_ms >= 0 ? now_ms() + (uint64_t)timeout_ms : 0, cancel);
        if (w < 0) return w;
    }
}

int tcp_fd(struct tcp_conn *c) { return c->fd; }
void tcp_close(struct tcp_conn *c) { if (!c) return; shutdown(c->fd, SHUT_WR); close(c->fd); free(c); }
void tcp_abort(struct tcp_conn *c) {
    if (!c) return;
    struct linger lg = { 1, 0 };   /* RST */
    setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(c->fd);
    free(c);
}
