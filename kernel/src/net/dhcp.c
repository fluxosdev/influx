/* DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK. Leases are not renewed. */
#include "net.h"
#include "net_internal.h"
#include "mm.h"
#include "string.h"

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t options[312];
} PACKED;

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

struct dhcp_lease { uint32_t ip, mask, router, dns, server, lease; int type; };

static size_t build(struct dhcp_msg *m, struct netif *n, uint32_t xid, int type, uint32_t req_ip, uint32_t server) {
    memset(m, 0, sizeof *m);
    m->op = 1; m->htype = 1; m->hlen = ETH_ALEN;
    m->xid = htonl(xid);
    m->flags = htons(0x8000);   /* ask for broadcast replies: we have no address yet */
    memcpy(m->chaddr, n->mac, ETH_ALEN);
    m->magic = htonl(0x63825363);
    uint8_t *o = m->options;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, n->mac, ETH_ALEN); o += ETH_ALEN;
    if (req_ip) { *o++ = 50; *o++ = 4; uint32_t v = htonl(req_ip); memcpy(o, &v, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; uint32_t v = htonl(server); memcpy(o, &v, 4); o += 4; }
    size_t hl = strlen(system_hostname);
    if (hl && hl < 64) { *o++ = 12; *o++ = hl; memcpy(o, system_hostname, hl); o += hl; }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;
    return (size_t)(o - (uint8_t *)m);
}

static bool parse(const uint8_t *buf, int len, uint32_t xid, const uint8_t *mac, struct dhcp_lease *l) {
    if (len < (int)offsetof(struct dhcp_msg, options)) return false;
    const struct dhcp_msg *m = (const struct dhcp_msg *)buf;
    if (m->op != 2 || ntohl(m->xid) != xid || ntohl(m->magic) != 0x63825363 || memcmp(m->chaddr, mac, ETH_ALEN)) return false;
    memset(l, 0, sizeof *l);
    l->ip = ntohl(m->yiaddr);
    const uint8_t *o = m->options, *end = buf + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        uint8_t code = o[0], olen = o[1];
        const uint8_t *v = o + 2;
        uint32_t u = olen >= 4 ? ((uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]) : 0;
        switch (code) {
        case 53: if (olen >= 1) l->type = v[0]; break;
        case 1: l->mask = u; break;
        case 3: l->router = u; break;
        case 6: l->dns = u; break;
        case 51: l->lease = u; break;
        case 54: l->server = u; break;
        }
        o += 2 + olen;
    }
    return l->type != 0;
}

static bool exchange(struct udp_sock *s, struct netif *n, struct dhcp_msg *m, size_t len, uint32_t xid, int want, struct dhcp_lease *l, int timeout_ms) {
    uint8_t buf[600];
    for (int attempt = 0; attempt < 4; attempt++) {
        udp_sendto_if(s, n, 0, 0xFFFFFFFF, 67, m, len);
        uint64_t deadline = time_ms() + timeout_ms;
        for (;;) {
            /* signed and checked here: a negative timeout would make udp_recvfrom wait forever */
            int64_t left = (int64_t)(deadline - time_ms());
            if (left <= 0) break;
            int r = udp_recvfrom(s, buf, sizeof buf, NULL, NULL, (int)left);
            if (r < 0) break;
            if (parse(buf, r, xid, n->mac, l) && (l->type == want || l->type == DHCP_NAK)) return l->type == want;
        }
    }
    return false;
}

int dhcp_configure(struct netif *n, int timeout_ms) {
    /* bound to this adapter, so every adapter can run its own client on port 68 */
    struct udp_sock *s = udp_open_if(68, n);
    if (!s) return -EADDRINUSE;
    struct dhcp_msg *m = kmalloc(sizeof *m);
    uint32_t xid = (uint32_t)rdtsc() ^ ((uint32_t)n->mac[5] << 24);
    struct dhcp_lease offer = { 0 }, ack = { 0 };
    int step = timeout_ms / 8 > 500 ? timeout_ms / 8 : 500;
    int r = -ETIMEDOUT;
    size_t len = build(m, n, xid, DHCP_DISCOVER, 0, 0);
    if (exchange(s, n, m, len, xid, DHCP_OFFER, &offer, step) && offer.ip) {
        len = build(m, n, xid, DHCP_REQUEST, offer.ip, offer.server);
        if (exchange(s, n, m, len, xid, DHCP_ACK, &ack, step)) {
            mutex_lock(&net_lock);
            n->ip = ack.ip ? ack.ip : offer.ip;
            n->netmask = ack.mask ? ack.mask : offer.mask ? offer.mask : 0xFFFFFF00;
            n->gateway = ack.router ? ack.router : offer.router;
            n->dns = ack.dns ? ack.dns : offer.dns ? offer.dns : n->gateway;
            n->configured = true; n->dhcp = true;
            mutex_unlock(&net_lock);
            char ip[16], gw[16], dns[16];
            ip_to_str(n->ip, ip); ip_to_str(n->gateway, gw); ip_to_str(n->dns, dns);
            kprintf("%s: DHCP lease %s/%d, gateway %s, DNS %s, %u s\n", n->name, ip, __builtin_popcount(n->netmask), gw, dns, ack.lease);
            dns_flush();
            r = 0;
        } else if (ack.type == DHCP_NAK) r = -ECONNREFUSED;
    }
    kfree(m);
    udp_close(s);
    return r;
}

static void dhcp_thread(void *arg) {
    struct netif *n = arg;
    for (int attempt = 0, busy = 0; attempt < 3;) {
        if (n->configured) return;
        int r = dhcp_configure(n, 8000);
        if (r == 0) return;
        /* port 68 on this adapter taken (the shell's dhcp command is running): wait for it rather than give up */
        if (r == -EADDRINUSE && ++busy < 30) { thread_sleep_ms(1000); continue; }
        attempt++;
    }
    kprintf("%s: no DHCP server answered; configure the adapter with ifconfig\n", n->name);
}

void dhcp_start(struct netif *n) { thread_create(kernel_process, "dhcp", dhcp_thread, n); }
