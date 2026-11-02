/* UDP: datagram sockets with a small per-socket receive queue. */
#include "net.h"
#include "net_internal.h"
#include "mm.h"
#include "string.h"

#define UDP_QUEUE 32

struct udp_dgram { uint32_t src; uint16_t sport; uint16_t len; uint8_t *data; };
struct udp_sock {
    bool used;
    uint16_t port;
    struct udp_dgram q[UDP_QUEUE];
    int head, count;
    struct semaphore sem;
};
static struct udp_sock socks[32];
static uint16_t next_ephemeral = 49152;

static struct udp_sock *find_port(uint16_t port) {
    for (size_t i = 0; i < ARRAY_LEN(socks); i++) if (socks[i].used && socks[i].port == port) return &socks[i];
    return NULL;
}

struct udp_sock *udp_open(uint16_t port) {
    mutex_lock(&net_lock);
    if (!port) {
        for (int tries = 0; tries < 16384; tries++) {
            uint16_t p = next_ephemeral++;
            if (next_ephemeral < 49152) next_ephemeral = 49152;
            if (!find_port(p)) { port = p; break; }
        }
    } else if (find_port(port)) { mutex_unlock(&net_lock); return NULL; }
    struct udp_sock *s = NULL;
    for (size_t i = 0; i < ARRAY_LEN(socks); i++) if (!socks[i].used) { s = &socks[i]; break; }
    if (s && port) {
        memset(s, 0, sizeof *s);
        s->used = true; s->port = port;
        sem_init(&s->sem, 0);
    } else s = NULL;
    mutex_unlock(&net_lock);
    return s;
}

void udp_close(struct udp_sock *s) {
    if (!s) return;
    mutex_lock(&net_lock);
    for (int i = 0; i < s->count; i++) kfree(s->q[(s->head + i) % UDP_QUEUE].data);
    s->used = false;
    mutex_unlock(&net_lock);
}

void udp_input(struct netif *n, uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *h = (const struct udp_hdr *)pkt;
    size_t ulen = ntohs(h->len);
    if (ulen < sizeof *h || ulen > len) return;
    if (h->csum) {
        uint32_t sum = ip_pseudo_sum(src, dst, IP_PROTO_UDP, ulen);
        if (net_checksum(pkt, ulen, sum) != 0) { n->rx_errors++; return; }
    }
    struct udp_sock *s = find_port(ntohs(h->dport));
    if (!s) return;
    if (s->count == UDP_QUEUE) { n->rx_dropped++; return; }
    struct udp_dgram *d = &s->q[(s->head + s->count) % UDP_QUEUE];
    d->src = src; d->sport = ntohs(h->sport); d->len = ulen - sizeof *h;
    d->data = kmalloc(d->len ? d->len : 1);
    memcpy(d->data, pkt + sizeof *h, d->len);
    s->count++;
    sem_post(&s->sem);
}

int udp_sendto_if(struct udp_sock *s, struct netif *n, uint32_t src, uint32_t dst, uint16_t port, const void *data, size_t len) {
    uint8_t pkt[ETH_MTU];
    if (len > ETH_MTU - sizeof(struct ip_hdr) - sizeof(struct udp_hdr)) return -EINVAL;
    mutex_lock(&net_lock);
    uint32_t hop;
    if (!n) n = ip_route(dst, &hop);
    if (!n) { mutex_unlock(&net_lock); return -ENETUNREACH; }
    if (src == 0xFFFFFFFF) src = n->ip;
    struct udp_hdr *h = (struct udp_hdr *)pkt;
    h->sport = htons(s->port); h->dport = htons(port);
    h->len = htons(sizeof *h + len); h->csum = 0;
    memcpy(pkt + sizeof *h, data, len);
    uint16_t c = net_checksum(pkt, sizeof *h + len, ip_pseudo_sum(src, dst, IP_PROTO_UDP, sizeof *h + len));
    h->csum = htons(c ? c : 0xFFFF);
    int r = ip_output_src(n, src, dst, IP_PROTO_UDP, pkt, sizeof *h + len);
    mutex_unlock(&net_lock);
    return r < 0 ? r : (int)len;
}

int udp_sendto(struct udp_sock *s, uint32_t dst, uint16_t port, const void *data, size_t len) {
    return udp_sendto_if(s, NULL, 0xFFFFFFFF, dst, port, data, len);
}

int udp_recvfrom(struct udp_sock *s, void *buf, size_t len, uint32_t *src, uint16_t *sport, int timeout_ms) {
    uint64_t deadline = time_ms() + (timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        mutex_lock(&net_lock);
        if (s->count) {
            struct udp_dgram *d = &s->q[s->head];
            size_t n = MIN(len, (size_t)d->len);
            memcpy(buf, d->data, n);
            if (src) *src = d->src;
            if (sport) *sport = d->sport;
            kfree(d->data);
            s->head = (s->head + 1) % UDP_QUEUE; s->count--;
            mutex_unlock(&net_lock);
            return (int)n;
        }
        mutex_unlock(&net_lock);
        uint64_t now = time_ms();
        if (timeout_ms >= 0 && now >= deadline) return -ETIMEDOUT;
        if (timeout_ms < 0) sem_wait(&s->sem);
        else sem_wait_timeout(&s->sem, deadline - now);
    }
}
