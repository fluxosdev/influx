/*
 * TCP (RFC 793/9293): active open, in-order delivery, retransmission with
 * RFC 6298 timers and Karn's rule, slow start / congestion avoidance,
 * zero-window probing, orderly release and TIME_WAIT. Segments that arrive
 * after a hole are kept (up to TCP_OOO_MAX) and acknowledged with duplicate
 * ACKs so the sender fast-retransmits the missing one; there is no listening
 * side (the stack is a client).
 */
#include "net.h"
#include "net_internal.h"
#include "mm.h"
#include "string.h"

enum tcp_state { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSING, TCP_TIME_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK };
static const char *state_names[] = { "CLOSED", "SYN_SENT", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSING", "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK" };

#define TCP_MSS        1460
#define TCP_RBUF       65535
#define TCP_SBUF       (64 * 1024)
#define TCP_RTO_INIT   1000
#define TCP_RTO_MIN    200
#define TCP_RTO_MAX    30000
#define TCP_MAX_RETRIES 8
#define TCP_SYN_RETRIES 5
#define TCP_TIME_WAIT_MS 2000
#define TCP_FIN_WAIT_2_MS 30000
#define TCP_OOO_MAX    24

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)

struct tcp_conn {
    enum tcp_state state;
    uint32_t lip, rip;
    uint16_t lport, rport;
    /* send side: sbuf[0] holds the byte at snd_una (once established) */
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd;
    uint16_t mss;
    uint8_t *sbuf; size_t slen;
    bool fin_queued, fin_assigned;   /* app closed / the FIN has a sequence number */
    uint32_t fin_seq;
    uint32_t cwnd, ssthresh;
    /* receive side: ring buffer */
    uint32_t rcv_nxt;
    uint32_t rcv_wnd_max;       /* largest window we advertise */
    uint8_t *rbuf; size_t rhead, rlen;
    bool rx_fin;
    struct { uint32_t seq; uint16_t len; bool fin; uint8_t *data; } ooo[TCP_OOO_MAX];
    int nooo;
    /* timers */
    uint64_t rto_at;            /* 0 = not armed */
    int rto, retries;
    int srtt, rttvar;           /* ms, 0 = no sample yet */
    bool rtt_active; uint32_t rtt_seq; uint64_t rtt_start;
    uint64_t state_since;
    int err;
    bool app_closed;
    struct semaphore sem;
};

static struct tcp_conn *conns[64];
static uint16_t next_port;

static void set_state(struct tcp_conn *c, enum tcp_state s) { c->state = s; c->state_since = time_ms(); }
static void wake(struct tcp_conn *c) { sem_post(&c->sem); }
static size_t rwnd(struct tcp_conn *c) { return c->app_closed ? c->rcv_wnd_max : MIN((size_t)c->rcv_wnd_max, TCP_RBUF - c->rlen); }

static struct tcp_conn *lookup(uint32_t rip, uint16_t rport, uint32_t lip, uint16_t lport) {
    for (size_t i = 0; i < ARRAY_LEN(conns); i++) {
        struct tcp_conn *c = conns[i];
        if (c && c->state != TCP_CLOSED && c->rip == rip && c->rport == rport && c->lport == lport && c->lip == lip) return c;
    }
    return NULL;
}

static int send_segment(uint32_t lip, uint32_t rip, uint16_t lport, uint16_t rport, uint32_t seq, uint32_t ack, uint8_t flags,
                        uint16_t win, const uint8_t *data, size_t len, uint16_t mss_opt) {
    static uint8_t pkt[ETH_MTU];
    struct tcp_hdr *h = (struct tcp_hdr *)pkt;
    size_t hl = sizeof *h + (mss_opt ? 4 : 0);
    if (hl + len > ETH_MTU - sizeof(struct ip_hdr)) return -EINVAL;
    h->sport = htons(lport); h->dport = htons(rport);
    h->seq = htonl(seq); h->ack = htonl(flags & TCP_ACK ? ack : 0);
    h->off = (hl / 4) << 4; h->flags = flags;
    h->win = htons(win); h->csum = 0; h->urg = 0;
    if (mss_opt) { uint8_t *o = pkt + sizeof *h; o[0] = 2; o[1] = 4; o[2] = mss_opt >> 8; o[3] = mss_opt & 0xFF; }
    if (len) memcpy(pkt + hl, data, len);
    h->csum = htons(net_checksum(pkt, hl + len, ip_pseudo_sum(lip, rip, IP_PROTO_TCP, hl + len)));
    return ip_output_src(NULL, lip, rip, IP_PROTO_TCP, pkt, hl + len);
}

static void send_ctl(struct tcp_conn *c, uint32_t seq, uint8_t flags) {
    send_segment(c->lip, c->rip, c->lport, c->rport, seq, c->rcv_nxt, flags, rwnd(c), NULL, 0, flags & TCP_SYN ? TCP_MSS : 0);
}
static void send_ack(struct tcp_conn *c) { send_ctl(c, c->snd_nxt, TCP_ACK); }

static void arm_rto(struct tcp_conn *c) { c->rto_at = time_ms() + c->rto; }

static void ooo_clear(struct tcp_conn *c) {
    for (int i = 0; i < c->nooo; i++) kfree(c->ooo[i].data);
    c->nooo = 0;
}
/* keep a segment that arrived beyond a hole */
static void ooo_store(struct tcp_conn *c, uint32_t seq, const uint8_t *data, size_t len, bool fin) {
    if (c->nooo == TCP_OOO_MAX || seq - c->rcv_nxt >= c->rcv_wnd_max) return;
    for (int i = 0; i < c->nooo; i++) if (c->ooo[i].seq == seq && c->ooo[i].len >= len) return;
    c->ooo[c->nooo].seq = seq; c->ooo[c->nooo].len = len; c->ooo[c->nooo].fin = fin;
    c->ooo[c->nooo].data = kmalloc(len ? len : 1);
    memcpy(c->ooo[c->nooo].data, data, len);
    c->nooo++;
}
/* appends in-order bytes to the receive buffer (or discards them once the application is gone) */
static size_t deliver(struct tcp_conn *c, const uint8_t *data, size_t len) {
    size_t take = c->app_closed ? len : MIN(len, TCP_RBUF - c->rlen);
    if (!c->app_closed) {
        for (size_t i = 0; i < take; i++) c->rbuf[(c->rhead + c->rlen + i) % TCP_RBUF] = data[i];
        c->rlen += take;
    }
    c->rcv_nxt += take;
    return take;
}
/* moves stored segments that the hole has caught up with; returns true if a FIN became in-order */
static bool ooo_drain(struct tcp_conn *c) {
    bool fin = false, progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < c->nooo; i++) {
            uint32_t s = c->ooo[i].seq, e = s + c->ooo[i].len;
            if (SEQ_GT(s, c->rcv_nxt)) continue;
            bool whole = true;
            if (SEQ_GT(e, c->rcv_nxt)) { size_t off = c->rcv_nxt - s; whole = deliver(c, c->ooo[i].data + off, c->ooo[i].len - off) == c->ooo[i].len - off; }
            if (whole && c->ooo[i].fin && c->rcv_nxt == e) fin = true;
            kfree(c->ooo[i].data);
            c->ooo[i] = c->ooo[--c->nooo];
            progress = true;
            break;
        }
    }
    return fin;
}

/* transmits whatever the windows allow; probe forces one byte into a closed window */
static void tcp_output(struct tcp_conn *c, bool probe) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT && c->state != TCP_FIN_WAIT_1 && c->state != TCP_LAST_ACK && c->state != TCP_CLOSING)
        return;
    size_t off = c->snd_nxt - c->snd_una;
    size_t window = MIN(c->snd_wnd, c->cwnd);
    if (probe && window == 0) window = 1;
    while (off < c->slen && off < window) {
        size_t n = MIN(MIN((size_t)c->mss, c->slen - off), window - off);
        uint8_t flags = TCP_ACK | (off + n == c->slen ? TCP_PSH : 0);
        send_segment(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, flags, rwnd(c), c->sbuf + off, n, 0);
        if (!c->rtt_active) { c->rtt_active = true; c->rtt_seq = c->snd_nxt + n; c->rtt_start = time_ms(); }
        c->snd_nxt += n; off += n;
        if (SEQ_GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
        if (!c->rto_at) arm_rto(c);
    }
    if (c->fin_queued && off == c->slen) {
        if (!c->fin_assigned) {
            c->fin_seq = c->snd_nxt; c->fin_assigned = true;
            if (c->state == TCP_ESTABLISHED) set_state(c, TCP_FIN_WAIT_1);
            else if (c->state == TCP_CLOSE_WAIT) set_state(c, TCP_LAST_ACK);
        }
        if (c->snd_nxt == c->fin_seq) {   /* first transmission or retransmission of the FIN */
            send_ctl(c, c->snd_nxt, TCP_FIN | TCP_ACK);
            c->snd_nxt++;
            if (SEQ_GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
            if (!c->rto_at) arm_rto(c);
        }
    }
}

static void conn_fail(struct tcp_conn *c, int err) {
    c->err = err;
    set_state(c, TCP_CLOSED);
    c->rto_at = 0;
    wake(c);
}

static void rtt_sample(struct tcp_conn *c, int ms) {
    if (ms < 1) ms = 1;
    if (!c->srtt) { c->srtt = ms; c->rttvar = ms / 2; }
    else {
        int d = c->srtt - ms; if (d < 0) d = -d;
        c->rttvar = (3 * c->rttvar + d) / 4;
        c->srtt = (7 * c->srtt + ms) / 8;
    }
    c->rto = CLAMP(c->srtt + MAX(TCP_TIMER_MS, 4 * c->rttvar), TCP_RTO_MIN, TCP_RTO_MAX);
}

static void parse_options(struct tcp_conn *c, const uint8_t *o, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (o[i] == 0) break;
        if (o[i] == 1) { i++; continue; }
        if (i + 1 >= len || o[i + 1] < 2 || i + o[i + 1] > len) break;
        if (o[i] == 2 && o[i + 1] == 4) { uint16_t m = o[i + 2] << 8 | o[i + 3]; if (m >= 64) c->mss = MIN(m, TCP_MSS); }
        i += o[i + 1];
    }
}

void tcp_input(struct netif *n, uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    if (net_checksum(pkt, len, ip_pseudo_sum(src, dst, IP_PROTO_TCP, len)) != 0) { n->rx_errors++; return; }
    const struct tcp_hdr *h = (const struct tcp_hdr *)pkt;
    size_t hl = (h->off >> 4) * 4;
    if (hl < sizeof *h || hl > len) return;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    uint8_t flags = h->flags;
    const uint8_t *data = pkt + hl;
    size_t dlen = len - hl;
    uint16_t win = ntohs(h->win);

    struct tcp_conn *c = lookup(src, ntohs(h->sport), dst, ntohs(h->dport));
    if (!c) {
        /* nobody here: reset the sender (never answer a reset) */
        if (flags & TCP_RST) return;
        if (flags & TCP_ACK) send_segment(dst, src, ntohs(h->dport), ntohs(h->sport), ack, 0, TCP_RST, 0, NULL, 0, 0);
        else send_segment(dst, src, ntohs(h->dport), ntohs(h->sport), 0, seq + dlen + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0), TCP_RST | TCP_ACK, 0, NULL, 0, 0);
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        bool ack_ok = (flags & TCP_ACK) && ack == c->iss + 1;
        if ((flags & TCP_ACK) && !ack_ok) { if (!(flags & TCP_RST)) send_segment(dst, src, c->lport, c->rport, ack, 0, TCP_RST, 0, NULL, 0, 0); return; }
        if (flags & TCP_RST) { if (ack_ok) conn_fail(c, ECONNREFUSED); return; }
        if (!(flags & TCP_SYN) || !ack_ok) return;
        c->rcv_nxt = seq + 1;
        c->snd_una = c->snd_nxt = c->snd_max = ack;
        c->snd_wnd = win;
        parse_options(c, pkt + sizeof *h, hl - sizeof *h);
        c->cwnd = 10 * c->mss;
        c->rto_at = 0; c->retries = 0;
        if (c->rtt_active) { rtt_sample(c, (int)(time_ms() - c->rtt_start)); c->rtt_active = false; }
        set_state(c, TCP_ESTABLISHED);
        send_ack(c);
        wake(c);
        return;
    }

    /* acceptability: we only take segments that start at rcv_nxt (trimming retransmitted prefixes) */
    if (flags & TCP_RST) {
        if (SEQ_LEQ(c->rcv_nxt, seq) && SEQ_LT(seq, c->rcv_nxt + TCP_RBUF)) conn_fail(c, ECONNRESET);
        return;
    }
    if (flags & TCP_SYN) { send_ack(c); return; }   /* retransmitted SYN-ACK: our ACK was lost */
    if (SEQ_LT(seq, c->rcv_nxt)) {
        uint32_t dup = c->rcv_nxt - seq;
        if (dup >= dlen + ((flags & TCP_FIN) ? 1 : 0)) {
            if (dlen || (flags & TCP_FIN)) send_ack(c);   /* entirely old: re-acknowledge */
            dlen = 0; flags &= ~TCP_FIN;
            seq = c->rcv_nxt;
        } else { data += dup; dlen -= dup; seq = c->rcv_nxt; }
    }
    if (!(flags & TCP_ACK)) return;

    /* ACK processing */
    if (SEQ_GT(ack, c->snd_max)) { send_ack(c); return; }
    if (SEQ_LT(c->snd_una, ack)) {
        uint32_t acked = ack - c->snd_una;
        bool fin_acked = c->fin_assigned && SEQ_GT(ack, c->fin_seq);
        size_t data_acked = MIN((size_t)acked - (fin_acked ? 1 : 0), c->slen);
        memmove(c->sbuf, c->sbuf + data_acked, c->slen - data_acked);
        c->slen -= data_acked;
        c->snd_una = ack;
        if (SEQ_GT(ack, c->snd_nxt)) c->snd_nxt = ack;   /* data sent before a timeout rewind arrived after all */
        c->retries = 0;
        if (c->rtt_active && SEQ_LEQ(c->rtt_seq, ack)) { rtt_sample(c, (int)(time_ms() - c->rtt_start)); c->rtt_active = false; }
        if (c->cwnd < c->ssthresh) c->cwnd += c->mss;
        else c->cwnd += MAX(1u, (uint32_t)c->mss * c->mss / c->cwnd);
        c->rto_at = 0;
        if (c->snd_nxt != c->snd_una) arm_rto(c);
        if (fin_acked) {
            if (c->state == TCP_FIN_WAIT_1) set_state(c, TCP_FIN_WAIT_2);
            else if (c->state == TCP_CLOSING) set_state(c, TCP_TIME_WAIT);
            else if (c->state == TCP_LAST_ACK) { set_state(c, TCP_CLOSED); }
        }
        wake(c);
    }
    if (SEQ_LEQ(c->snd_una, ack)) c->snd_wnd = win;

    bool receiving = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;
    if (SEQ_GT(seq, c->rcv_nxt)) {
        /* a hole before this segment: keep it and ask again for the missing bytes */
        if (receiving && (dlen || (flags & TCP_FIN))) ooo_store(c, seq, data, dlen, flags & TCP_FIN);
        send_ack(c);
        tcp_output(c, false);
        return;
    }

    /* data */
    bool need_ack = false;
    if (dlen && receiving) {
        size_t take = deliver(c, data, dlen);
        if (take < dlen) flags &= ~TCP_FIN;   /* the FIN lies beyond what we kept */
        if (c->nooo && take == dlen && !(flags & TCP_FIN) && ooo_drain(c)) flags |= TCP_FIN;
        need_ack = true;
        wake(c);
    }
    if (flags & TCP_FIN) {
        c->rcv_nxt++;
        c->rx_fin = true;
        need_ack = true;
        switch (c->state) {
        case TCP_ESTABLISHED: set_state(c, TCP_CLOSE_WAIT); break;
        case TCP_FIN_WAIT_1: set_state(c, SEQ_GT(c->snd_una, c->fin_seq) ? TCP_TIME_WAIT : TCP_CLOSING); break;
        case TCP_FIN_WAIT_2: set_state(c, TCP_TIME_WAIT); break;
        default: break;
        }
        wake(c);
    }
    if (need_ack) send_ack(c);
    tcp_output(c, false);
}

void tcp_timer(void) {
    uint64_t now = time_ms();
    for (size_t i = 0; i < ARRAY_LEN(conns); i++) {
        struct tcp_conn *c = conns[i];
        if (!c) continue;
        if (c->state == TCP_CLOSED) {
            if (c->app_closed) { ooo_clear(c); kfree(c->sbuf); kfree(c->rbuf); kfree(c); conns[i] = NULL; }
            continue;
        }
        if (c->state == TCP_TIME_WAIT && now - c->state_since >= TCP_TIME_WAIT_MS) { set_state(c, TCP_CLOSED); wake(c); continue; }
        if (c->state == TCP_FIN_WAIT_2 && c->app_closed && now - c->state_since >= TCP_FIN_WAIT_2_MS) { set_state(c, TCP_CLOSED); continue; }
        if (c->rto_at && now >= c->rto_at) {
            c->rto_at = 0;
            if (++c->retries > (c->state == TCP_SYN_SENT ? TCP_SYN_RETRIES : TCP_MAX_RETRIES)) { conn_fail(c, ETIMEDOUT); continue; }
            c->rto = MIN(c->rto * 2, TCP_RTO_MAX);
            c->rtt_active = false;   /* Karn: no samples from retransmitted data */
            if (c->state == TCP_SYN_SENT) { send_ctl(c, c->iss, TCP_SYN); arm_rto(c); continue; }
            uint32_t flight = c->snd_nxt - c->snd_una;
            c->ssthresh = MAX(flight / 2, 2u * c->mss);
            c->cwnd = c->mss;
            /* go back to the first unacknowledged byte */
            c->snd_nxt = c->snd_una;
            tcp_output(c, c->snd_wnd == 0);
            if (!c->rto_at && (c->slen || c->snd_max != c->snd_una)) arm_rto(c);
        } else if (!c->rto_at && c->slen && c->snd_wnd == 0 && c->snd_nxt == c->snd_una) {
            arm_rto(c);   /* persist timer for a closed peer window */
        }
    }
}

struct tcp_conn *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms, volatile bool *cancel, int *err) {
    *err = 0;
    mutex_lock(&net_lock);
    uint32_t hop;
    struct netif *n = ip_route(ip, &hop);
    if (!n || (ip >> 24) == 127 || ip == 0) { mutex_unlock(&net_lock); *err = ENETUNREACH; return NULL; }
    int slot = -1;
    for (size_t i = 0; i < ARRAY_LEN(conns); i++) if (!conns[i]) { slot = i; break; }
    if (slot < 0) { mutex_unlock(&net_lock); *err = EBUSY; return NULL; }
    struct tcp_conn *c = kzalloc(sizeof *c);
    c->sbuf = kmalloc(TCP_SBUF);
    c->rbuf = kmalloc(TCP_RBUF);
    sem_init(&c->sem, 0);
    c->lip = n->ip; c->rip = ip; c->rport = port;
    c->rcv_wnd_max = n->rx_window ? MIN((uint32_t)TCP_RBUF, MAX(n->rx_window, (uint32_t)TCP_MSS)) : TCP_RBUF;
    if (!next_port) next_port = 49152 + (uint16_t)(rdtsc() % 16000);
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port++;
        if (next_port < 49152) next_port = 49152;
        bool used = false;
        for (size_t i = 0; i < ARRAY_LEN(conns); i++) if (conns[i] && conns[i]->lport == p) used = true;
        if (!used) { c->lport = p; break; }
    }
    c->iss = (uint32_t)(rdtsc() >> 4);
    c->snd_una = c->iss; c->snd_nxt = c->snd_max = c->iss + 1;
    c->mss = 536;
    c->ssthresh = 65535;
    c->cwnd = 10 * TCP_MSS;
    c->rto = TCP_RTO_INIT;
    conns[slot] = c;
    set_state(c, TCP_SYN_SENT);
    c->rtt_active = true; c->rtt_start = time_ms();
    send_ctl(c, c->iss, TCP_SYN);
    arm_rto(c);
    mutex_unlock(&net_lock);

    uint64_t deadline = time_ms() + timeout_ms;
    for (;;) {
        mutex_lock(&net_lock);
        enum tcp_state s = c->state; int e = c->err;
        mutex_unlock(&net_lock);
        if (s == TCP_ESTABLISHED || s == TCP_CLOSE_WAIT) return c;
        int fail = 0;
        if (s == TCP_CLOSED) fail = e ? e : ECONNRESET;
        else if (cancel && *cancel) fail = ECANCELED;
        else if (time_ms() >= deadline) fail = ETIMEDOUT;
        if (fail) { tcp_abort(c); *err = fail; return NULL; }
        sem_wait_timeout(&c->sem, MIN(100, deadline - time_ms() + 1));
    }
}

int64_t tcp_send(struct tcp_conn *c, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        mutex_lock(&net_lock);
        if (c->err || c->fin_queued || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)) {
            int e = c->err ? c->err : ENOTCONN;
            mutex_unlock(&net_lock);
            return done ? (int64_t)done : -e;
        }
        size_t n = MIN(len - done, TCP_SBUF - c->slen);
        if (n) {
            memcpy(c->sbuf + c->slen, p + done, n);
            c->slen += n; done += n;
            tcp_output(c, false);
        }
        mutex_unlock(&net_lock);
        if (!n) { if (thread_should_exit()) return done ? (int64_t)done : -ECANCELED; sem_wait_timeout(&c->sem, 100); }
    }
    return done;
}

int64_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, int timeout_ms, volatile bool *cancel) {
    uint8_t *p = buf;
    uint64_t deadline = time_ms() + (timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        mutex_lock(&net_lock);
        if (c->rlen) {
            size_t before = rwnd(c);
            size_t n = MIN(len, c->rlen);
            for (size_t i = 0; i < n; i++) p[i] = c->rbuf[(c->rhead + i) % TCP_RBUF];
            c->rhead = (c->rhead + n) % TCP_RBUF; c->rlen -= n;
            /* window update once a mostly-closed window has reopened */
            if (before < c->rcv_wnd_max / 2 && rwnd(c) >= c->rcv_wnd_max / 2 && c->state != TCP_CLOSED) send_ack(c);
            mutex_unlock(&net_lock);
            return n;
        }
        bool eof = c->rx_fin || c->state == TCP_CLOSED;
        int e = c->err;
        mutex_unlock(&net_lock);
        if (e) return -e;
        if (eof) return 0;
        if ((cancel && *cancel) || thread_should_exit()) return -ECANCELED;
        uint64_t now = time_ms();
        if (timeout_ms >= 0 && now >= deadline) return -ETIMEDOUT;
        sem_wait_timeout(&c->sem, timeout_ms >= 0 ? MIN(100, deadline - now) : 100);
    }
}

void tcp_close(struct tcp_conn *c) {
    if (!c) return;
    mutex_lock(&net_lock);
    c->app_closed = true;
    c->rlen = 0;
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) { c->fin_queued = true; tcp_output(c, false); }
    else if (c->state == TCP_SYN_SENT) set_state(c, TCP_CLOSED);
    mutex_unlock(&net_lock);
}

void tcp_abort(struct tcp_conn *c) {
    if (!c) return;
    mutex_lock(&net_lock);
    if (c->state != TCP_CLOSED && c->state != TCP_SYN_SENT && c->state != TCP_TIME_WAIT)
        send_segment(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, TCP_RST | TCP_ACK, 0, NULL, 0, 0);
    c->app_closed = true;
    set_state(c, TCP_CLOSED);
    c->rto_at = 0;
    mutex_unlock(&net_lock);
}

int tcp_list(struct tcp_conn_info *out, int max) {
    int k = 0;
    mutex_lock(&net_lock);
    for (size_t i = 0; i < ARRAY_LEN(conns) && k < max; i++) {
        struct tcp_conn *c = conns[i];
        if (!c) continue;
        out[k].lip = c->lip; out[k].rip = c->rip; out[k].lport = c->lport; out[k].rport = c->rport;
        out[k].state = state_names[c->state]; out[k].rx_queued = c->rlen; out[k].tx_queued = c->slen;
        k++;
    }
    mutex_unlock(&net_lock);
    return k;
}
