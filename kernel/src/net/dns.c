/* DNS stub resolver: A records over UDP, following CNAME chains, with a small TTL cache. */
#include "net.h"
#include "net_internal.h"
#include "mm.h"
#include "string.h"

struct dns_cache_entry { char name[128]; uint32_t ip; uint64_t expires; };
static struct dns_cache_entry cache[32];
static int cache_next;
static struct mutex cache_lock = MUTEX_INIT;
static uint16_t dns_id = 0x1234;

void dns_flush(void) { mutex_lock(&cache_lock); memset(cache, 0, sizeof cache); mutex_unlock(&cache_lock); }

static bool cache_get(const char *name, uint32_t *ip) {
    bool hit = false;
    mutex_lock(&cache_lock);
    for (size_t i = 0; i < ARRAY_LEN(cache); i++)
        if (cache[i].name[0] && !strcasecmp(cache[i].name, name) && cache[i].expires > time_ms()) { *ip = cache[i].ip; hit = true; break; }
    mutex_unlock(&cache_lock);
    return hit;
}
static void cache_put(const char *name, uint32_t ip, uint32_t ttl) {
    if (strlen(name) >= sizeof cache[0].name) return;
    if (ttl < 30) ttl = 30;
    if (ttl > 3600) ttl = 3600;
    mutex_lock(&cache_lock);
    struct dns_cache_entry *e = &cache[cache_next++ % ARRAY_LEN(cache)];
    strlcpy(e->name, name, sizeof e->name); e->ip = ip; e->expires = time_ms() + (uint64_t)ttl * 1000;
    mutex_unlock(&cache_lock);
}

/* skips an encoded (possibly compressed) name; returns the offset after it or -1 */
static int skip_name(const uint8_t *msg, int len, int off) {
    while (off < len) {
        uint8_t l = msg[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2 <= len ? off + 2 : -1;
        off += 1 + l;
    }
    return -1;
}

static int build_query(uint8_t *q, uint16_t id, const char *name) {
    memset(q, 0, 12);
    q[0] = id >> 8; q[1] = id & 0xFF;
    q[2] = 0x01;          /* recursion desired */
    q[5] = 1;             /* one question */
    int off = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int l = dot ? (int)(dot - p) : (int)strlen(p);
        if (l == 0 || l > 63 || off + l + 2 > 300) return -1;
        q[off++] = l; memcpy(q + off, p, l); off += l;
        p += l; if (*p == '.') p++;
    }
    q[off++] = 0;
    q[off++] = 0; q[off++] = 1;   /* type A */
    q[off++] = 0; q[off++] = 1;   /* class IN */
    return off;
}

/* returns 0 and the first A record, -ENOENT for NXDOMAIN / no address, -EPROTO for garbage */
static int parse_reply(const uint8_t *m, int len, uint16_t id, uint32_t *ip, uint32_t *ttl) {
    if (len < 12 || (m[0] << 8 | m[1]) != id || !(m[2] & 0x80)) return -EPROTO;
    int rcode = m[3] & 0x0F;
    if (rcode == 3) return -ENOENT;
    if (rcode != 0) return -EPROTO;
    int qd = m[4] << 8 | m[5], an = m[6] << 8 | m[7];
    int off = 12;
    for (int i = 0; i < qd; i++) { off = skip_name(m, len, off); if (off < 0 || off + 4 > len) return -EPROTO; off += 4; }
    for (int i = 0; i < an; i++) {
        off = skip_name(m, len, off);
        if (off < 0 || off + 10 > len) return -EPROTO;
        uint16_t type = m[off] << 8 | m[off + 1], cls = m[off + 2] << 8 | m[off + 3];
        uint32_t t = (uint32_t)m[off + 4] << 24 | m[off + 5] << 16 | m[off + 6] << 8 | m[off + 7];
        uint16_t rdlen = m[off + 8] << 8 | m[off + 9];
        off += 10;
        if (off + rdlen > len) return -EPROTO;
        /* CNAME records precede the address in recursive answers; take the first A */
        if (type == 1 && cls == 1 && rdlen == 4) {
            *ip = (uint32_t)m[off] << 24 | m[off + 1] << 16 | m[off + 2] << 8 | m[off + 3];
            *ttl = t;
            return 0;
        }
        off += rdlen;
    }
    return -ENOENT;
}

int dns_resolve(const char *name, uint32_t *ip, int timeout_ms, volatile bool *cancel) {
    if (str_to_ip(name, ip)) return 0;
    if (!strcasecmp(name, "localhost")) { *ip = IP4(127, 0, 0, 1); return 0; }
    char host[256];
    strlcpy(host, name, sizeof host);
    size_t hl = strlen(host);
    if (hl && host[hl - 1] == '.') host[--hl] = 0;
    if (!hl) return -EINVAL;
    if (cache_get(host, ip)) return 0;
    struct netif *n = netif_default();
    if (!n || !n->configured) return -ENETUNREACH;
    uint32_t server = n->dns ? n->dns : n->gateway;
    if (!server) return -ENETUNREACH;

    struct udp_sock *s = udp_open(0);
    if (!s) return -EBUSY;
    uint8_t q[320], reply[1500];
    uint16_t id = dns_id++ ^ (uint16_t)rdtsc();
    int qlen = build_query(q, id, host);
    if (qlen < 0) { udp_close(s); return -EINVAL; }
    int r = -ETIMEDOUT;
    uint64_t deadline = time_ms() + timeout_ms;
    int per_try = 1000;
    while (time_ms() < deadline && r == -ETIMEDOUT) {
        if (cancel && *cancel) { r = -ECANCELED; break; }
        udp_sendto(s, server, 53, q, qlen);
        uint64_t try_end = MIN(deadline, time_ms() + per_try);
        for (;;) {
            if (cancel && *cancel) { r = -ECANCELED; break; }
            /* signed and checked here: a negative timeout would make udp_recvfrom wait forever */
            int64_t left = (int64_t)(try_end - time_ms());
            if (left <= 0) break;
            uint32_t from; uint16_t port;
            int got = udp_recvfrom(s, reply, sizeof reply, &from, &port, (int)MIN(left, 100));
            if (got < 0) continue;
            if (from != server || port != 53) continue;
            uint32_t ttl = 0;
            int pr = parse_reply(reply, got, id, ip, &ttl);
            if (pr == -EPROTO) continue;
            r = pr;
            if (pr == 0) cache_put(host, *ip, ttl);
            break;
        }
        per_try = MIN(per_try * 2, 4000);
    }
    udp_close(s);
    return r;
}
