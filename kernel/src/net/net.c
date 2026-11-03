/* Network core: adapter registry, the network thread, checksums, Ethernet and ARP. */
#include "net.h"
#include "net_internal.h"
#include "mm.h"
#include "string.h"

struct mutex net_lock = MUTEX_INIT;
static struct netif *ifaces[8];
static int nifaces;
static struct semaphore kick;

int net_register(struct netif *n) {
    if (nifaces >= (int)ARRAY_LEN(ifaces)) return -ENOSPC;
    mutex_lock(&net_lock);
    ifaces[nifaces++] = n;
    mutex_unlock(&net_lock);
    return 0;
}
int netif_count(void) { return nifaces; }
struct netif *netif_at(int i) { return i >= 0 && i < nifaces ? ifaces[i] : NULL; }
struct netif *netif_default(void) {
    for (int i = 0; i < nifaces; i++) if (ifaces[i]->up && ifaces[i]->configured) return ifaces[i];
    for (int i = 0; i < nifaces; i++) if (ifaces[i]->up) return ifaces[i];
    return NULL;
}
struct netif *netif_find(const char *name) {
    for (int i = 0; i < nifaces; i++) if (!strcmp(ifaces[i]->name, name)) return ifaces[i];
    return NULL;
}

void net_kick(void) { sem_post(&kick); }

static void net_thread(void *arg) {
    uint64_t next_timer = 0;
    for (;;) {
        int work = 0;
        mutex_lock(&net_lock);
        for (int i = 0; i < nifaces; i++) if (ifaces[i]->up && ifaces[i]->poll) work += ifaces[i]->poll(ifaces[i]);
        uint64_t now = time_ms();
        if (now >= next_timer) { arp_timer(); tcp_timer(); next_timer = now + TCP_TIMER_MS; }
        mutex_unlock(&net_lock);
        bool polled = false;
        for (int i = 0; i < nifaces; i++) if (ifaces[i]->up && !ifaces[i]->irq_driven) polled = true;
        if (work) thread_yield();
        else if (polled) sem_wait_timeout(&kick, 1);
        else sem_wait_timeout(&kick, next_timer > now ? next_timer - now : 1);   /* until an interrupt or the next timer */
    }
}

void net_init(void) {
    sem_init(&kick, 0);
    e1000_init();
    ne2000_init();
    if (!nifaces) { kprintf("net: no network adapters found\n"); return; }
    thread_create(kernel_process, "net", net_thread, NULL);
    for (int i = 0; i < nifaces; i++) dhcp_start(ifaces[i]);
}

/* ---- helpers ---- */
uint32_t net_checksum_partial(const void *data, size_t len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) { sum += (uint32_t)(p[0] << 8 | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}
uint16_t net_checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
uint16_t net_checksum(const void *data, size_t len, uint32_t initial) { return net_checksum_finish(net_checksum_partial(data, len, initial)); }

void ip_to_str(uint32_t ip, char *out) { snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF); }
bool str_to_ip(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit(*s)) return false;
        uint32_t n = 0; int digits = 0;
        while (isdigit(*s)) { n = n * 10 + (*s++ - '0'); if (++digits > 3) return false; }
        if (n > 255) return false;
        v = (v << 8) | n;
        if (part < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *ip = v;
    return true;
}
void mac_to_str(const uint8_t *m, char *out) { snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); }

/* ---- Ethernet ---- */
const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void eth_input(struct netif *n, const uint8_t *frame, size_t len) {
    if (len < sizeof(struct eth_hdr)) return;
    const struct eth_hdr *h = (const struct eth_hdr *)frame;
    if (memcmp(h->dst, n->mac, ETH_ALEN) && memcmp(h->dst, eth_broadcast, ETH_ALEN)) return;
    const uint8_t *payload = frame + sizeof *h;
    size_t plen = len - sizeof *h;
    switch (ntohs(h->type)) {
    case ETHERTYPE_ARP: arp_input(n, payload, plen); break;
    case ETHERTYPE_IP: ip_input(n, payload, plen); break;
    }
}

int eth_output(struct netif *n, const uint8_t *dst, uint16_t type, const void *payload, size_t len) {
    static uint8_t frame[ETH_FRAME_MAX];   /* serialised by net_lock */
    if (len > ETH_MTU) return -EINVAL;
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst, ETH_ALEN);
    memcpy(h->src, n->mac, ETH_ALEN);
    h->type = htons(type);
    memcpy(frame + sizeof *h, payload, len);
    int r = n->send(n, frame, sizeof *h + len);
    net_kick();
    return r;
}

/* ---- ARP ---- */
#define ARP_ENTRIES 64
#define ARP_PENDING 16
#define ARP_EXPIRE_MS (10 * 60 * 1000)
#define ARP_RETRY_MS 500
#define ARP_GIVEUP_MS 3000

struct arp_pkt {
    uint16_t htype, ptype; uint8_t hlen, plen; uint16_t op;
    uint8_t sha[ETH_ALEN]; uint32_t spa; uint8_t tha[ETH_ALEN]; uint32_t tpa;
} PACKED;

struct arp_entry { uint32_t ip; uint8_t mac[ETH_ALEN]; bool used, resolved; uint64_t updated, last_req, first_req; struct netif *n; };
static struct arp_entry arp_table[ARP_ENTRIES];
/* IP packets waiting for their next hop to be resolved */
struct arp_wait { bool used; struct netif *n; uint32_t hop; uint64_t t; size_t len; uint8_t *pkt; };
static struct arp_wait arp_waiting[ARP_PENDING];

static struct arp_entry *arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_ENTRIES; i++) if (arp_table[i].used && arp_table[i].ip == ip) return &arp_table[i];
    return NULL;
}
static struct arp_entry *arp_slot(uint32_t ip) {
    struct arp_entry *e = arp_lookup(ip);
    if (e) return e;
    struct arp_entry *oldest = &arp_table[0];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!arp_table[i].used) { oldest = &arp_table[i]; break; }
        if (arp_table[i].updated < oldest->updated) oldest = &arp_table[i];
    }
    memset(oldest, 0, sizeof *oldest);
    oldest->used = true; oldest->ip = ip;
    return oldest;
}

static void arp_send(struct netif *n, uint16_t op, const uint8_t *tha, uint32_t tpa, const uint8_t *dst) {
    struct arp_pkt p;
    p.htype = htons(1); p.ptype = htons(ETHERTYPE_IP); p.hlen = ETH_ALEN; p.plen = 4; p.op = htons(op);
    memcpy(p.sha, n->mac, ETH_ALEN); p.spa = htonl(n->ip);
    memcpy(p.tha, tha, ETH_ALEN); p.tpa = htonl(tpa);
    eth_output(n, dst, ETHERTYPE_ARP, &p, sizeof p);
}

static void arp_learn(struct netif *n, uint32_t ip, const uint8_t *mac) {
    struct arp_entry *e = arp_slot(ip);
    memcpy(e->mac, mac, ETH_ALEN); e->resolved = true; e->updated = time_ms(); e->n = n;
    for (int i = 0; i < ARP_PENDING; i++) {
        struct arp_wait *w = &arp_waiting[i];
        if (!w->used || w->hop != ip) continue;
        eth_output(w->n, mac, ETHERTYPE_IP, w->pkt, w->len);
        kfree(w->pkt); w->used = false;
    }
}

void arp_input(struct netif *n, const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *p = (const struct arp_pkt *)pkt;
    if (ntohs(p->htype) != 1 || ntohs(p->ptype) != ETHERTYPE_IP || p->hlen != ETH_ALEN || p->plen != 4) return;
    uint32_t spa = ntohl(p->spa), tpa = ntohl(p->tpa);
    if (spa) {
        /* refresh an existing entry, or add one when the packet is meant for us */
        if (arp_lookup(spa) || (n->ip && tpa == n->ip)) arp_learn(n, spa, p->sha);
    }
    if (ntohs(p->op) == 1 && n->ip && tpa == n->ip) arp_send(n, 2, p->sha, spa, p->sha);
}

/* sends an IP packet to a next hop on n, resolving its hardware address first if needed */
int arp_output(struct netif *n, uint32_t hop, const void *pkt, size_t len) {
    if (hop == 0xFFFFFFFF || (n->netmask && hop == (n->ip | ~n->netmask)))
        return eth_output(n, eth_broadcast, ETHERTYPE_IP, pkt, len);
    struct arp_entry *e = arp_lookup(hop);
    if (e && e->resolved) return eth_output(n, e->mac, ETHERTYPE_IP, pkt, len);
    /* queue the packet (replacing the oldest waiter) and ask who has the address */
    struct arp_wait *slot = NULL;
    for (int i = 0; i < ARP_PENDING; i++) if (!arp_waiting[i].used) { slot = &arp_waiting[i]; break; }
    if (!slot) {
        slot = &arp_waiting[0];
        for (int i = 1; i < ARP_PENDING; i++) if (arp_waiting[i].t < slot->t) slot = &arp_waiting[i];
        kfree(slot->pkt);
    }
    slot->pkt = kmalloc(len);
    memcpy(slot->pkt, pkt, len);
    slot->len = len; slot->hop = hop; slot->n = n; slot->t = time_ms(); slot->used = true;
    uint64_t now = time_ms();
    if (!e) { e = arp_slot(hop); e->first_req = now; e->n = n; e->updated = now; }
    if (!e->last_req || now - e->last_req >= ARP_RETRY_MS / 5) {
        static const uint8_t zero[ETH_ALEN];
        arp_send(n, 1, zero, hop, eth_broadcast);
        e->last_req = now;
    }
    return 0;
}

void arp_timer(void) {
    uint64_t now = time_ms();
    static const uint8_t zero[ETH_ALEN];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (!e->used) continue;
        if (e->resolved) { if (now - e->updated > ARP_EXPIRE_MS) e->used = false; continue; }
        if (now - e->first_req > ARP_GIVEUP_MS) { e->used = false; continue; }
        if (now - e->last_req >= ARP_RETRY_MS && e->n) { arp_send(e->n, 1, zero, e->ip, eth_broadcast); e->last_req = now; }
    }
    for (int i = 0; i < ARP_PENDING; i++) {
        struct arp_wait *w = &arp_waiting[i];
        if (w->used && now - w->t > ARP_GIVEUP_MS) { kfree(w->pkt); w->used = false; }
    }
}

int arp_list(struct arp_entry_info *out, int max) {
    int k = 0;
    mutex_lock(&net_lock);
    uint64_t now = time_ms();
    for (int i = 0; i < ARP_ENTRIES && k < max; i++) {
        struct arp_entry *e = &arp_table[i];
        if (!e->used) continue;
        out[k].ip = e->ip; memcpy(out[k].mac, e->mac, ETH_ALEN);
        out[k].resolved = e->resolved; out[k].age_ms = now - e->updated;
        k++;
    }
    mutex_unlock(&net_lock);
    return k;
}
