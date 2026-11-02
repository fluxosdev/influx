#pragma once
/* Wire formats and helpers shared by the protocol modules in kernel/src/net. */
#include "net.h"

#define TCP_TIMER_MS 50

struct eth_hdr { uint8_t dst[ETH_ALEN], src[ETH_ALEN]; uint16_t type; } PACKED;

struct ip_hdr {
    uint8_t ver_ihl, tos; uint16_t total_len;
    uint16_t id, frag;
    uint8_t ttl, proto; uint16_t csum;
    uint32_t src, dst;
} PACKED;
#define IP_FLAG_MF 0x2000
#define IP_FLAG_DF 0x4000
#define IP_FRAG_OFFSET 0x1FFF

struct icmp_hdr { uint8_t type, code; uint16_t csum; uint16_t id, seq; } PACKED;
struct udp_hdr { uint16_t sport, dport, len, csum; } PACKED;
struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off;          /* data offset in the high nibble */
    uint8_t flags;
    uint16_t win, csum, urg;
} PACKED;
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

extern const uint8_t eth_broadcast[ETH_ALEN];
int arp_output(struct netif *n, uint32_t hop, const void *pkt, size_t len);
/* the TTL of the IP packet currently being delivered (for ping) */
extern uint8_t ip_input_ttl;
