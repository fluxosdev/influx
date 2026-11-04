/*
 * SNTP client (RFC 4330). One request/response exchange gives the offset
 * between our clock and the server's, ((T2 - T1) + (T3 - T4)) / 2, which is
 * applied by stepping the wall clock (and the RTC with it). A background
 * thread syncs once an adapter has an address and then every four hours;
 * the server is the one DHCP named (option 42), else pool.ntp.org.
 */
#include "net.h"
#include "net_internal.h"
#include "string.h"

#define NTP_PORT 123
#define NTP_UNIX_DELTA 2208988800ull   /* 1900-01-01 to 1970-01-01, seconds */
#define SYNC_INTERVAL_MS (4ull * 3600 * 1000)

bool sntp_enabled = true;
uint64_t sntp_last_sync;
char sntp_last_server[64];

struct ntp_pkt {
    uint8_t li_vn_mode, stratum, poll; int8_t precision;
    uint32_t root_delay, root_disp, ref_id;
    uint32_t ref_s, ref_f, orig_s, orig_f, rx_s, rx_f, tx_s, tx_f;
} PACKED;

static void to_ntp(int64_t unix_ns, uint32_t *s, uint32_t *f) {
    uint64_t secs = (uint64_t)(unix_ns / 1000000000ll) + NTP_UNIX_DELTA;
    uint64_t frac = (uint64_t)(unix_ns % 1000000000ll);
    *s = htonl((uint32_t)secs);
    *f = htonl((uint32_t)((frac << 32) / 1000000000ull));
}
/* NTP era 0 runs out in 2036: timestamps with the top bit clear are taken to be in era 1 */
static int64_t from_ntp(uint32_t s_be, uint32_t f_be) {
    uint64_t s = ntohl(s_be), f = ntohl(f_be);
    if (!(s & 0x80000000u)) s += 1ull << 32;
    return (int64_t)(s - NTP_UNIX_DELTA) * 1000000000ll + (int64_t)((f * 1000000000ull) >> 32);
}

int64_t sntp_sync(uint32_t server, int timeout_ms, int *err) {
    *err = 0;
    const char *name = NULL;
    if (!server) {
        struct netif *n = netif_default();
        if (n && n->configured && n->ntp) server = n->ntp;
    }
    if (!server) {
        name = "pool.ntp.org";
        int r = dns_resolve(name, &server, timeout_ms, NULL);
        if (r < 0) { *err = r; return 0; }
    }
    struct udp_sock *s = udp_open(0);
    if (!s) { *err = -ENOMEM; return 0; }
    int r = -ETIMEDOUT;
    int64_t offset = 0;
    for (int attempt = 0; attempt < 3 && r == -ETIMEDOUT; attempt++) {
        struct ntp_pkt q = { 0 };
        q.li_vn_mode = (0 << 6) | (4 << 3) | 3;   /* no leap warning, version 4, client */
        int64_t t1 = time_realtime_ns();
        uint64_t m1 = time_ns();
        to_ntp(t1, &q.tx_s, &q.tx_f);
        if (udp_sendto(s, server, NTP_PORT, &q, sizeof q) < 0) { thread_sleep_ms(500); continue; }
        uint64_t deadline = time_ms() + (uint64_t)timeout_ms / 3;
        for (;;) {
            int64_t left = (int64_t)(deadline - time_ms());
            if (left <= 0) break;
            struct ntp_pkt a; uint32_t from; uint16_t port;
            int n = udp_recvfrom(s, &a, sizeof a, &from, &port, (int)left);
            if (n < 0) break;
            if (n < (int)sizeof a || from != server || port != NTP_PORT) continue;
            int mode = a.li_vn_mode & 7, li = a.li_vn_mode >> 6;
            /* must answer our request (origin = our transmit time) and be synchronised itself */
            if (mode != 4 || a.orig_s != q.tx_s || a.orig_f != q.tx_f) continue;
            if (a.stratum == 0 || a.stratum > 15 || li == 3 || !a.tx_s) { r = -EPROTO; break; }
            /* T4 from the monotonic clock: the wall clock may not move while we wait, but be safe */
            int64_t t4 = t1 + (int64_t)(time_ns() - m1);
            int64_t t2 = from_ntp(a.rx_s, a.rx_f), t3 = from_ntp(a.tx_s, a.tx_f);
            offset = ((t2 - t1) + (t3 - t4)) / 2;
            r = 0;
            break;
        }
    }
    udp_close(s);
    if (r < 0) { *err = r; return 0; }
    time_set_realtime_ns(time_realtime_ns() + offset);
    sntp_last_sync = time_unix();
    if (name) strlcpy(sntp_last_server, name, sizeof sntp_last_server);
    else ip_to_str(server, sntp_last_server);
    int64_t ms = offset / 1000000;
    kprintf("sntp: clock set from %s, corrected by %ld ms\n", sntp_last_server, ms);
    return ms;
}

static void sntp_thread(void *arg) {
    uint64_t next = 0;
    int failures = 0;
    for (;;) {
        thread_sleep_ms(2000);
        if (!sntp_enabled || time_ms() < next) continue;
        struct netif *n = netif_default();
        if (!n || !n->configured) continue;
        int err;
        sntp_sync(0, 6000, &err);
        if (err == 0) { failures = 0; next = time_ms() + SYNC_INTERVAL_MS; }
        else {
            if (++failures == 3) kprintf("sntp: no time server answered (%d); retrying\n", err);
            next = time_ms() + MIN(15000ull << MIN(failures, 6), SYNC_INTERVAL_MS);
        }
    }
}

void sntp_start(void) { thread_create(kernel_process, "sntp", sntp_thread, NULL); }
