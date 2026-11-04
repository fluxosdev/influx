/* netif_default() for kernel sources in user programs: the kernel reports its default adapter */
#include "net.h"

struct netinfo { char name[8], driver[32]; uint32_t ip, netmask, gateway, dns; int32_t up, configured, present, pad; };

struct netif *netif_default(void) {
    static struct netif n;
    struct netinfo ni;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(0x1004L), "D"(&ni) : "rcx", "r11", "memory");
    if (r < 0 || !ni.present) return NULL;
    memcpy(n.name, ni.name, sizeof n.name); memcpy(n.driver, ni.driver, sizeof n.driver);
    n.ip = ni.ip; n.netmask = ni.netmask; n.gateway = ni.gateway; n.dns = ni.dns;
    n.up = ni.up; n.configured = ni.configured;
    return &n;
}
