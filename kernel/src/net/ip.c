/* IPv4 input/output and routing, ICMP echo. Fragmented datagrams are dropped
 * (TCP advertises an MSS that keeps segments within one Ethernet frame). */
#include "net.h"
#include "net_internal.h"
#include "string.h"

uint8_t ip_input_ttl;
static uint16_t ip_ident = 1;
static uint64_t ip_dropped_fragments;

uint32_t ip_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
}

struct netif *ip_route(uint32_t dst, uint32_t *hop) {
    if (dst == 0xFFFFFFFF) { *hop = dst; return netif_default(); }
    for (int i = 0; i < netif_count(); i++) {
        struct netif *n = netif_at(i);
        if (!n->up || !n->configured) continue;
        if ((dst & n->netmask) == (n->ip & n->netmask)) { *hop = dst; return n; }
    }
    for (int i = 0; i < netif_count(); i++) {
        struct netif *n = netif_at(i);
        if (n->up && n->configured && n->gateway) { *hop = n->gateway; return n; }
    }
    return NULL;
}

int ip_output_src(struct netif *n, uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    static uint8_t pkt[ETH_MTU];
    uint32_t hop = dst;
    if (!n) { n = ip_route(dst, &hop); if (!n) return -ENETUNREACH; }
    else if (dst != 0xFFFFFFFF && n->configured && (dst & n->netmask) != (n->ip & n->netmask)) hop = n->gateway;
    if (len + sizeof(struct ip_hdr) > ETH_MTU) return -EINVAL;
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    h->ver_ihl = 0x45; h->tos = 0;
    h->total_len = htons(sizeof *h + len);
    h->id = htons(ip_ident++);
    h->frag = htons(IP_FLAG_DF);
    h->ttl = 64; h->proto = proto; h->csum = 0;
    h->src = htonl(src == 0xFFFFFFFF ? n->ip : src);
    h->dst = htonl(dst);
    h->csum = htons(net_checksum(h, sizeof *h, 0));
    memcpy(pkt + sizeof *h, payload, len);
    return arp_output(n, hop, pkt, sizeof *h + len);
}

int ip_output(uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    uint32_t hop;
    struct netif *n = ip_route(dst, &hop);
    if (!n) return -ENETUNREACH;
    return ip_output_src(n, n->ip, dst, proto, payload, len);
}

void ip_input(struct netif *n, const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *h = (const struct ip_hdr *)pkt;
    if ((h->ver_ihl >> 4) != 4) return;
    size_t ihl = (h->ver_ihl & 0xF) * 4;
    size_t total = ntohs(h->total_len);
    if (ihl < sizeof *h || total < ihl || total > len) return;
    if (net_checksum(h, ihl, 0) != 0) { n->rx_errors++; return; }
    if (ntohs(h->frag) & (IP_FLAG_MF | IP_FRAG_OFFSET)) { ip_dropped_fragments++; return; }
    uint32_t src = ntohl(h->src), dst = ntohl(h->dst);
    bool for_us = !n->configured || dst == n->ip || dst == 0xFFFFFFFF || (n->netmask && dst == (n->ip | ~n->netmask));
    if (!for_us) return;
    ip_input_ttl = h->ttl;
    const uint8_t *payload = pkt + ihl;
    size_t plen = total - ihl;
    switch (h->proto) {
    case IP_PROTO_ICMP: icmp_input(n, src, dst, payload, plen); break;
    case IP_PROTO_UDP: udp_input(n, src, dst, payload, plen); break;
    case IP_PROTO_TCP: tcp_input(n, src, dst, payload, plen); break;
    }
}

/* ---- ICMP ---- */
struct ping_wait { bool used, done; uint16_t id, seq; uint8_t ttl; uint64_t rx_tsc; struct semaphore sem; };
static struct ping_wait pings[8];
static uint16_t ping_ident = 0x4C58;

void icmp_input(struct netif *n, uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct icmp_hdr) || net_checksum(pkt, len, 0) != 0) return;
    const struct icmp_hdr *h = (const struct icmp_hdr *)pkt;
    if (h->type == 8 && h->code == 0 && n->configured && dst == n->ip) {
        /* echo request: answer with the same payload */
        static uint8_t reply[ETH_MTU];
        if (len > sizeof reply - sizeof(struct ip_hdr)) return;
        memcpy(reply, pkt, len);
        struct icmp_hdr *r = (struct icmp_hdr *)reply;
        r->type = 0; r->csum = 0;
        r->csum = htons(net_checksum(reply, len, 0));
        ip_output_src(n, n->ip, src, IP_PROTO_ICMP, reply, len);
        return;
    }
    if (h->type == 0) {
        for (size_t i = 0; i < ARRAY_LEN(pings); i++) {
            struct ping_wait *w = &pings[i];
            if (w->used && !w->done && w->id == ntohs(h->id) && w->seq == ntohs(h->seq)) {
                w->done = true; w->ttl = ip_input_ttl; w->rx_tsc = rdtsc();
                sem_post(&w->sem);
            }
        }
    }
}

int64_t icmp_ping(uint32_t dst, uint16_t seq, size_t payload, int timeout_ms, int *ttl_out) {
    uint8_t pkt[sizeof(struct icmp_hdr) + 1024];
    if (payload > 1024) payload = 1024;
    mutex_lock(&net_lock);
    struct ping_wait *w = NULL;
    for (size_t i = 0; i < ARRAY_LEN(pings); i++) if (!pings[i].used) { w = &pings[i]; break; }
    if (!w) { mutex_unlock(&net_lock); return -EBUSY; }
    w->used = true; w->done = false; w->id = ping_ident; w->seq = seq;
    sem_init(&w->sem, 0);
    struct icmp_hdr *h = (struct icmp_hdr *)pkt;
    h->type = 8; h->code = 0; h->csum = 0; h->id = htons(w->id); h->seq = htons(seq);
    for (size_t i = 0; i < payload; i++) pkt[sizeof *h + i] = 0x20 + (i % 64);
    h->csum = htons(net_checksum(pkt, sizeof *h + payload, 0));
    uint64_t t0 = rdtsc();
    int r = ip_output(dst, IP_PROTO_ICMP, pkt, sizeof *h + payload);
    mutex_unlock(&net_lock);
    if (r < 0) { mutex_lock(&net_lock); w->used = false; mutex_unlock(&net_lock); return r; }
    sem_wait_timeout(&w->sem, timeout_ms);
    mutex_lock(&net_lock);
    bool ok = w->done;
    uint64_t t1 = w->rx_tsc;
    if (ttl_out) *ttl_out = w->ttl;
    w->used = false;
    mutex_unlock(&net_lock);
    if (!ok) return -ETIMEDOUT;
    uint64_t hz = cpu_tsc_hz();
    return hz ? (int64_t)((t1 - t0) * 1000000 / hz) : 0;
}
