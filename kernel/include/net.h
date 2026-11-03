#pragma once
#include "kernel.h"
#include "sched.h"
#include "fs.h"     /* errno values */

/*
 * inFlux TCP/IP stack.
 *
 * All protocol state is serialised by one mutex (net_lock). The "net" kernel
 * thread services the network adapters (woken by their interrupts, or polling
 * those that have none), feeds received frames up the stack and runs the
 * protocol timers; application threads call the socket-style
 * functions below, which take the lock themselves and sleep on per-socket
 * semaphores while they wait.
 */

#define ETH_ALEN 6
#define ETH_MTU 1500
#define ETH_FRAME_MAX 1514
#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define ECONNRESET   104
#define EHOSTUNREACH 113
#define ENETUNREACH  101
#define ENOTCONN     107
#define EADDRINUSE   98
#define ECANCELED    125
#define EPROTO       71
#ifndef EBUSY
#define EBUSY        16
#endif

/* addresses are kept in host byte order: 10.0.2.15 == 0x0A00020F */
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

struct netif {
    char name[8];
    char driver[32];
    uint8_t mac[ETH_ALEN];
    uint32_t ip, netmask, gateway, dns;
    bool up, configured, dhcp;
    uint32_t rx_window;            /* bytes the adapter can buffer; caps TCP's advertised window (0 = no limit) */
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped;
    int (*send)(struct netif *n, const void *frame, size_t len);
    int (*poll)(struct netif *n);           /* deliver pending frames to eth_input; returns count */
    bool irq_driven;                        /* calls net_kick() from its interrupt: no need to poll */
    void *priv;
};

/* core */
extern struct mutex net_lock;
void net_init(void);
int net_register(struct netif *n);
int netif_count(void);
struct netif *netif_at(int i);
struct netif *netif_default(void);           /* first adapter that is up */
struct netif *netif_find(const char *name);
void net_kick(void);                         /* poll the adapters soon (after transmitting) */
uint16_t net_checksum(const void *data, size_t len, uint32_t initial);
uint32_t net_checksum_partial(const void *data, size_t len, uint32_t sum);
uint16_t net_checksum_finish(uint32_t sum);
void ip_to_str(uint32_t ip, char *out);      /* out >= 16 bytes */
bool str_to_ip(const char *s, uint32_t *ip);
void mac_to_str(const uint8_t *mac, char *out); /* out >= 18 bytes */

/* link layer (called with net_lock held) */
void eth_input(struct netif *n, const uint8_t *frame, size_t len);
int eth_output(struct netif *n, const uint8_t *dst, uint16_t type, const void *payload, size_t len);
void arp_input(struct netif *n, const uint8_t *pkt, size_t len);
void arp_timer(void);
struct arp_entry_info { uint32_t ip; uint8_t mac[ETH_ALEN]; uint64_t age_ms; bool resolved; };
int arp_list(struct arp_entry_info *out, int max);

/* IPv4 (called with net_lock held) */
void ip_input(struct netif *n, const uint8_t *pkt, size_t len);
int ip_output(uint32_t dst, uint8_t proto, const void *payload, size_t len);
int ip_output_src(struct netif *n, uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len);
struct netif *ip_route(uint32_t dst, uint32_t *next_hop);
uint32_t ip_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

/* ICMP */
void icmp_input(struct netif *n, uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len);
/* sends an echo request and waits for the reply; returns RTT in microseconds or -errno */
int64_t icmp_ping(uint32_t dst, uint16_t seq, size_t payload, int timeout_ms, int *ttl_out);

/* UDP */
struct udp_sock;
void udp_input(struct netif *n, uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len);
struct udp_sock *udp_open(uint16_t port);    /* 0 = ephemeral */
/* receives only what arrives on adapter n (NULL = any); a port can be bound once per adapter */
struct udp_sock *udp_open_if(uint16_t port, struct netif *n);
int udp_sendto(struct udp_sock *s, uint32_t dst, uint16_t port, const void *data, size_t len);
int udp_sendto_if(struct udp_sock *s, struct netif *n, uint32_t src, uint32_t dst, uint16_t port, const void *data, size_t len);
int udp_recvfrom(struct udp_sock *s, void *buf, size_t len, uint32_t *src, uint16_t *sport, int timeout_ms);
void udp_close(struct udp_sock *s);

/* TCP */
struct tcp_conn;
void tcp_input(struct netif *n, uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len);
void tcp_timer(void);
struct tcp_conn *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms, volatile bool *cancel, int *err);
int64_t tcp_send(struct tcp_conn *c, const void *data, size_t len);
/* returns bytes read, 0 at end of stream, -ETIMEDOUT, -ECANCELED or another -errno */
int64_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, int timeout_ms, volatile bool *cancel);
void tcp_close(struct tcp_conn *c);          /* graceful close; the connection is freed by the stack */
void tcp_abort(struct tcp_conn *c);          /* send RST and free */
struct tcp_conn_info { uint32_t lip, rip; uint16_t lport, rport; const char *state; uint32_t rx_queued, tx_queued; };
int tcp_list(struct tcp_conn_info *out, int max);

/* DHCP / DNS */
int dhcp_configure(struct netif *n, int timeout_ms);   /* blocking; 0 on success */
void dhcp_start(struct netif *n);                      /* in a background thread */
int dns_resolve(const char *name, uint32_t *ip, int timeout_ms, volatile bool *cancel);
void dns_flush(void);

/* HTTP client */
struct http_response {
    int status;
    char status_text[64];
    char content_type[96];
    char location[1024];
    char final_url[1024];          /* after redirects */
    char *headers;                 /* raw header block */
    uint8_t *body; size_t body_len;
    char error[160];
};
typedef void (*http_progress_fn)(void *ctx, const char *stage, size_t done, size_t total);
struct http_request {
    const char *url;
    const char *method;            /* NULL = GET */
    const void *body; size_t body_len;
    const char *content_type;      /* request body type */
    int max_redirects;             /* < 0 = default (8) */
    size_t max_body;               /* 0 = default (16 MiB) */
    volatile bool *cancel;
    http_progress_fn progress; void *progress_ctx;
};
/* 0 on success (resp->status holds the HTTP status), -errno with resp->error set otherwise */
int http_request(const struct http_request *req, struct http_response *resp);
int http_get(const char *url, struct http_response *resp, volatile bool *cancel);
void http_response_free(struct http_response *resp);
const char *http_header(const struct http_response *resp, const char *name, char *out, size_t outsz);

struct url {
    char scheme[16];
    char host[256];
    uint16_t port;
    char path[2048];               /* includes the query string, always starts with '/' */
};
bool url_parse(const char *s, struct url *u);
/* resolves a (possibly relative) reference against a base URL into out */
void url_resolve(const char *base, const char *ref, char *out, size_t outsz);
void url_encode_component(const char *in, char *out, size_t outsz);

void ne2000_init(void);
void e1000_init(void);
