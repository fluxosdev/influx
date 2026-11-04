/*
 * DHCP client (RFC 2131).
 *
 * Acquiring: DISCOVER -> OFFER -> REQUEST -> ACK. A lease is then kept by a
 * per-adapter thread: at T1 (half the lease) it renews with a unicast REQUEST
 * to the server that granted it, at T2 (7/8) it rebinds with a broadcast
 * REQUEST to any server, and when the lease runs out (or a server answers
 * NAK) the address is dropped and the client starts over with DISCOVER. With
 * no server answering, discovery is retried with a growing back-off for as
 * long as the adapter is left on DHCP, so a cable plugged in later still gets
 * an address.
 *
 * Every (re)configuration of an adapter, by this client, the shell's dhcp
 * command or a static ifconfig, bumps netif.dhcp_gen; a lease thread whose
 * generation is no longer current stops at its next step.
 */
#include "net.h"
#include "net_internal.h"
#include "mm.h"
#include "string.h"
#include "fs.h"

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
#define DHCP_RELEASE  7

#define LEASE_INFINITE 0xFFFFFFFFu

struct dhcp_lease { uint32_t ip, mask, router, dns, server, lease, t1, t2, ntp; int type; };

/* ciaddr != 0: renewing or rebinding the address we hold (RFC 2131 4.3.2) */
static size_t build(struct dhcp_msg *m, struct netif *n, uint32_t xid, int type, uint32_t ciaddr, uint32_t req_ip, uint32_t server) {
    memset(m, 0, sizeof *m);
    m->op = 1; m->htype = 1; m->hlen = ETH_ALEN;
    m->xid = htonl(xid);
    m->ciaddr = htonl(ciaddr);
    if (!ciaddr) m->flags = htons(0x8000);   /* no address yet: ask for broadcast replies */
    memcpy(m->chaddr, n->mac, ETH_ALEN);
    m->magic = htonl(0x63825363);
    uint8_t *o = m->options;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, n->mac, ETH_ALEN); o += ETH_ALEN;
    if (req_ip) { *o++ = 50; *o++ = 4; uint32_t v = htonl(req_ip); memcpy(o, &v, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; uint32_t v = htonl(server); memcpy(o, &v, 4); o += 4; }
    size_t hl = strlen(system_hostname);
    if (type != DHCP_RELEASE && hl && hl < 64) { *o++ = 12; *o++ = hl; memcpy(o, system_hostname, hl); o += hl; }
    if (type != DHCP_RELEASE) { *o++ = 55; *o++ = 7; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 42; *o++ = 51; *o++ = 58; *o++ = 59; }
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
        case 42: l->ntp = u; break;
        case 51: l->lease = u; break;
        case 54: l->server = u; break;
        case 58: l->t1 = u; break;
        case 59: l->t2 = u; break;
        }
        o += 2 + olen;
    }
    return l->type != 0;
}

/* sends m to dst (broadcast when 0xFFFFFFFF, from src) up to 'tries' times and waits for a reply of type
 * 'want' or a NAK; true when 'want' arrived */
static bool exchange(struct udp_sock *s, struct netif *n, uint32_t src, uint32_t dst, struct dhcp_msg *m, size_t len,
                     uint32_t xid, int want, struct dhcp_lease *l, int timeout_ms, int tries) {
    uint8_t buf[600];
    for (int attempt = 0; attempt < tries; attempt++) {
        if (udp_sendto_if(s, n, src, dst, 67, m, len) < 0) { thread_sleep_ms(timeout_ms); continue; }
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

static void apply(struct netif *n, const struct dhcp_lease *ack, const struct dhcp_lease *offer, const char *how) {
    mutex_lock(&net_lock);
    uint32_t old_ip = n->ip;
    n->ip = ack->ip ? ack->ip : offer->ip;
    n->netmask = ack->mask ? ack->mask : offer->mask ? offer->mask : 0xFFFFFF00;
    n->gateway = ack->router ? ack->router : offer->router;
    n->dns = ack->dns ? ack->dns : offer->dns ? offer->dns : n->gateway;
    n->ntp = ack->ntp ? ack->ntp : offer->ntp;
    n->dhcp_server = ack->server ? ack->server : offer->server;
    n->configured = true; n->dhcp = true;
    uint32_t lease = ack->lease ? ack->lease : offer->lease;
    n->lease_expires_ms = lease == LEASE_INFINITE || !lease ? 0 : time_ms() + (uint64_t)lease * 1000;
    mutex_unlock(&net_lock);
    char ip[16], gw[16], dns[16];
    ip_to_str(n->ip, ip); ip_to_str(n->gateway, gw); ip_to_str(n->dns, dns);
    if (lease == LEASE_INFINITE) kprintf("%s: DHCP %s %s/%d, gateway %s, DNS %s, infinite lease\n", n->name, how, ip, __builtin_popcount(n->netmask), gw, dns);
    else kprintf("%s: DHCP %s %s/%d, gateway %s, DNS %s, %u s\n", n->name, how, ip, __builtin_popcount(n->netmask), gw, dns, lease);
    if (old_ip != n->ip) dns_flush();
    /* for user programs' resolvers (musl reads it) */
    char conf[64]; snprintf(conf, sizeof conf, "nameserver %s\n", dns);
    vfs_write_file("/etc/resolv.conf", conf, strlen(conf));
}

static void unconfigure(struct netif *n, const char *why) {
    mutex_lock(&net_lock);
    n->configured = false; n->ip = 0; n->lease_expires_ms = 0;
    mutex_unlock(&net_lock);
    kprintf("%s: %s; address dropped, looking for a DHCP server again\n", n->name, why);
    dns_flush();
}

static uint32_t new_xid(struct netif *n) { return (uint32_t)rdtsc() ^ ((uint32_t)n->mac[5] << 24) ^ ((uint32_t)n->mac[4] << 16); }

/* DISCOVER/OFFER/REQUEST/ACK; configures n on success. -EADDRINUSE: another client holds port 68 on n */
static int acquire(struct netif *n, int timeout_ms, struct dhcp_lease *out) {
    struct udp_sock *s = udp_open_if(68, n);
    if (!s) return -EADDRINUSE;
    struct dhcp_msg *m = kmalloc(sizeof *m);
    if (!m) { udp_close(s); return -ENOMEM; }
    uint32_t xid = new_xid(n);
    struct dhcp_lease offer = { 0 }, ack = { 0 };
    int step = timeout_ms / 8 > 500 ? timeout_ms / 8 : 500;
    int r = -ETIMEDOUT;
    size_t len = build(m, n, xid, DHCP_DISCOVER, 0, 0, 0);
    if (exchange(s, n, 0, 0xFFFFFFFF, m, len, xid, DHCP_OFFER, &offer, step, 4) && offer.ip) {
        len = build(m, n, xid, DHCP_REQUEST, 0, offer.ip, offer.server);
        if (exchange(s, n, 0, 0xFFFFFFFF, m, len, xid, DHCP_ACK, &ack, step, 4)) {
            apply(n, &ack, &offer, "lease");
            if (!ack.lease) ack.lease = offer.lease;
            if (!ack.server) ack.server = offer.server;
            if (!ack.ip) ack.ip = offer.ip;
            *out = ack;
            r = 0;
        } else if (ack.type == DHCP_NAK) r = -ECONNREFUSED;
    }
    kfree(m);
    udp_close(s);
    return r;
}

/* REQUEST for the address we hold: unicast to the server (renewing) or broadcast (rebinding) */
static int extend(struct netif *n, bool rebind, int timeout_ms, struct dhcp_lease *l) {
    struct udp_sock *s = udp_open_if(68, n);
    if (!s) return -EADDRINUSE;
    struct dhcp_msg *m = kmalloc(sizeof *m);
    if (!m) { udp_close(s); return -ENOMEM; }
    uint32_t xid = new_xid(n), ip = n->ip;
    struct dhcp_lease ack = { 0 };
    size_t len = build(m, n, xid, DHCP_REQUEST, ip, 0, 0);
    int r = -ETIMEDOUT;
    if (exchange(s, n, ip, rebind ? 0xFFFFFFFF : n->dhcp_server, m, len, xid, DHCP_ACK, &ack, timeout_ms, 1)) {
        if (!ack.ip) ack.ip = ip;
        if (!ack.server) ack.server = n->dhcp_server;
        apply(n, &ack, &ack, rebind ? "rebound" : "renewed");
        *l = ack;
        r = 0;
    } else if (ack.type == DHCP_NAK) r = -ECONNREFUSED;
    kfree(m);
    udp_close(s);
    return r;
}

static bool current(struct netif *n, uint32_t gen) { return n->dhcp_gen == gen; }

/* sleeps until time_ms() reaches 'until', waking every few seconds to see whether the lease thread was superseded */
static bool sleep_until(struct netif *n, uint32_t gen, uint64_t until) {
    for (;;) {
        if (!current(n, gen)) return false;
        uint64_t now = time_ms();
        if (now >= until) return true;
        thread_sleep_ms(MIN(until - now, 5000));
    }
}

struct lease_arg { struct netif *n; uint32_t gen; struct dhcp_lease lease; bool have_lease; };

/* keeps n's address: renews and rebinds a lease, rediscovers when it is lost */
static void lease_thread(void *argp) {
    struct lease_arg *a = argp;
    struct netif *n = a->n;
    uint32_t gen = a->gen;
    struct dhcp_lease l = a->lease;
    bool have = a->have_lease;
    kfree(a);
    int failures = 0, busy = 0;
    while (current(n, gen)) {
        if (!have) {
            int r = acquire(n, 8000, &l);
            if (!current(n, gen)) break;
            if (r == 0) { have = true; failures = 0; continue; }
            /* port 68 on this adapter taken (the shell's dhcp command is running): wait for it */
            if (r == -EADDRINUSE && ++busy < 60) { thread_sleep_ms(1000); continue; }
            busy = 0;
            if (++failures == 3) kprintf("%s: no DHCP server answered; still trying (or configure the adapter with ifconfig)\n", n->name);
            uint64_t backoff = failures < 3 ? 2000 : MIN(60000, 4000ull << MIN(failures - 3, 4));
            sleep_until(n, gen, time_ms() + backoff);
            continue;
        }
        if (l.lease == LEASE_INFINITE || !l.lease) break;   /* nothing to renew */
        uint64_t start = time_ms();
        uint64_t lease_ms = (uint64_t)l.lease * 1000;
        uint64_t t1 = start + (l.t1 && l.t1 < l.lease ? (uint64_t)l.t1 * 1000 : lease_ms / 2);
        uint64_t t2 = start + (l.t2 && l.t2 < l.lease && l.t2 * 1000ull > t1 - start ? (uint64_t)l.t2 * 1000 : lease_ms * 7 / 8);
        uint64_t expiry = start + lease_ms;
        if (!sleep_until(n, gen, t1)) break;
        bool extended = false, nak = false;
        while (!extended && !nak && current(n, gen)) {
            uint64_t now = time_ms();
            if (now >= expiry) break;
            bool rebind = now >= t2;
            /* RFC 2131 4.4.5: wait half the remaining time to T2 (or to expiry), at least 60 s */
            uint64_t left = (rebind ? expiry : t2) - now;
            int wait = (int)MIN(left, 10000);
            int r = extend(n, rebind, wait, &l);
            if (!current(n, gen)) break;
            if (r == 0) extended = true;
            else if (r == -ECONNREFUSED) nak = true;
            else {
                uint64_t next = time_ms() + MAX(left / 2, 60000);
                if (!rebind && next > t2) next = t2;
                if (next > expiry) next = expiry;
                sleep_until(n, gen, next);
            }
        }
        if (!current(n, gen)) break;
        if (!extended) { unconfigure(n, nak ? "DHCP server refused to extend the lease" : "DHCP lease expired"); have = false; failures = 0; }
    }
}

static void start_lease_thread(struct netif *n, uint32_t gen, const struct dhcp_lease *l) {
    struct lease_arg *a = kmalloc(sizeof *a);
    if (!a) return;
    memset(a, 0, sizeof *a);
    a->n = n; a->gen = gen;
    if (l) { a->lease = *l; a->have_lease = true; }
    thread_create(kernel_process, "dhcp", lease_thread, a);
}

static uint32_t bump_gen(struct netif *n) {
    mutex_lock(&net_lock);
    uint32_t g = ++n->dhcp_gen;
    mutex_unlock(&net_lock);
    return g;
}

int dhcp_configure(struct netif *n, int timeout_ms) {
    uint32_t gen = bump_gen(n);   /* the old lease thread stops */
    struct dhcp_lease l;
    int r = acquire(n, timeout_ms, &l);
    /* a busy port 68 means the superseded thread is mid-exchange: it notices the new generation within one exchange */
    for (int i = 0; r == -EADDRINUSE && i < 20; i++) { thread_sleep_ms(500); r = acquire(n, timeout_ms, &l); }
    if (r == 0 && current(n, gen)) start_lease_thread(n, gen, &l);
    return r;
}

void dhcp_stop(struct netif *n) { bump_gen(n); }

void dhcp_start(struct netif *n) { start_lease_thread(n, bump_gen(n), NULL); }
