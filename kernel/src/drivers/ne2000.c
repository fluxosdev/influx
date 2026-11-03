/*
 * NE2000 (National Semiconductor DP8390) Ethernet driver.
 *
 * Supports the PCI clones (Realtek RTL8029 -- QEMU's ne2k_pci -- Winbond,
 * VIA, Compex, ...) and probes the classic ISA I/O bases when no PCI card is
 * found. The card's 16 KiB of on-board buffer memory is reached through
 * remote DMA over the data port; pages 0x40-0x45 hold one transmit frame and
 * 0x46-0x7F form the receive ring.
 *
 * PCI cards are interrupt driven (INTx routed through ACPI _PRT): the handler
 * masks the card and wakes the network thread, which drains the ring and
 * unmasks it again. The handler touches only ISR and IMR, which live in
 * register page 0; the code that switches to page 1 holds page_lock, and so
 * does the handler (which may run on another CPU), so it never reads CURR or
 * writes MAR7 by mistake. ISA cards, and PCI cards whose line cannot be
 * routed, are polled.
 */
#include "net.h"
#include "arch.h"
#include "io.h"
#include "mm.h"
#include "string.h"

/* page 0 registers */
#define NE_CR      0x00
#define NE_PSTART  0x01
#define NE_PSTOP   0x02
#define NE_BNRY    0x03
#define NE_TPSR    0x04   /* write */
#define NE_TSR     0x04   /* read */
#define NE_TBCR0   0x05
#define NE_TBCR1   0x06
#define NE_ISR     0x07
#define NE_RSAR0   0x08
#define NE_RSAR1   0x09
#define NE_RBCR0   0x0A
#define NE_RBCR1   0x0B
#define NE_RCR     0x0C
#define NE_TCR     0x0D
#define NE_DCR     0x0E
#define NE_IMR     0x0F
/* page 1 */
#define NE_PAR0    0x01
#define NE_CURR    0x07
#define NE_MAR0    0x08
/* ASIC */
#define NE_DATA    0x10
#define NE_RESET   0x1F

#define CR_STP  0x01
#define CR_STA  0x02
#define CR_TXP  0x04
#define CR_RD0  0x08      /* remote read */
#define CR_RD1  0x10      /* remote write */
#define CR_RD2  0x20      /* abort / complete remote DMA */
#define CR_PAGE0 0x00
#define CR_PAGE1 0x40

#define ISR_PRX 0x01
#define ISR_PTX 0x02
#define ISR_RXE 0x04
#define ISR_TXE 0x08
#define ISR_OVW 0x10
#define ISR_CNT 0x20
#define ISR_RDC 0x40
#define ISR_RST 0x80

#define RCR_AB  0x04      /* accept broadcast */
#define RCR_AM  0x08      /* accept multicast */
#define RCR_MON 0x20
#define TCR_LOOPBACK 0x02
#define DCR_WORD 0x01
#define DCR_LS   0x08     /* normal operation (not loopback) */
#define DCR_FT1  0x40     /* FIFO threshold 8 bytes */

#define IMR_BITS (ISR_PRX | ISR_RXE | ISR_OVW | ISR_CNT)   /* RDC is polled during remote DMA */

#define TX_PAGE   0x40
#define RX_START  0x46
#define RX_STOP   0x80

struct ne2000 {
    struct netif netif;
    uint16_t io;
    bool word;            /* 16-bit data transfers */
    uint8_t next_pkt;     /* next receive ring page to read */
    int idle_polls;
    bool backlog;         /* the last drain stopped at its budget: frames may still be in the ring */
    bool irq;             /* interrupt driven */
    uint64_t irqs;
    spinlock_t page_lock; /* held while register page 1 is selected, and by ne_irq */
    uint8_t rxbuf[ETH_FRAME_MAX + 64] ALIGNED(8);
    uint8_t txbuf[ETH_FRAME_MAX + 64] ALIGNED(8);
};

static int ncards;

static inline void ne_out(struct ne2000 *ne, int reg, uint8_t v) { outb(ne->io + reg, v); }
static inline uint8_t ne_in(struct ne2000 *ne, int reg) { return inb(ne->io + reg); }

static bool wait_rdc(struct ne2000 *ne) {
    for (int i = 0; i < 100000; i++) if (ne_in(ne, NE_ISR) & ISR_RDC) { ne_out(ne, NE_ISR, ISR_RDC); return true; }
    return false;
}

/* remote DMA from card memory into buf; in 16-bit mode an odd length writes one byte past len */
static void ne_read_mem(struct ne2000 *ne, uint16_t addr, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t count = ne->word ? (len + 1) & ~1u : len;
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_RD2);
    ne_out(ne, NE_ISR, ISR_RDC);
    ne_out(ne, NE_RBCR0, count & 0xFF);
    ne_out(ne, NE_RBCR1, count >> 8);
    ne_out(ne, NE_RSAR0, addr & 0xFF);
    ne_out(ne, NE_RSAR1, addr >> 8);
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_RD0);
    if (ne->word) insw(ne->io + NE_DATA, p, count / 2);
    else {
        for (size_t i = 0; i < len; i++) p[i] = inb(ne->io + NE_DATA);
    }
    wait_rdc(ne);
}

/* in 16-bit mode an odd length reads one byte past len */
static void ne_write_mem(struct ne2000 *ne, uint16_t addr, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t count = ne->word ? (len + 1) & ~1u : len;
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_RD2);
    ne_out(ne, NE_ISR, ISR_RDC);
    ne_out(ne, NE_RBCR0, count & 0xFF);
    ne_out(ne, NE_RBCR1, count >> 8);
    ne_out(ne, NE_RSAR0, addr & 0xFF);
    ne_out(ne, NE_RSAR1, addr >> 8);
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_RD1);
    if (ne->word) outsw(ne->io + NE_DATA, p, count / 2);
    else {
        for (size_t i = 0; i < len; i++) outb(ne->io + NE_DATA, p[i]);
    }
    wait_rdc(ne);
}

/* (re)initialise the DP8390 with an empty receive ring; the station address is in ne->netif.mac */
static void ne_setup(struct ne2000 *ne) {
    spin_lock(&ne->page_lock);
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STP | CR_RD2);
    ne_out(ne, NE_DCR, (ne->word ? DCR_WORD : 0) | DCR_LS | DCR_FT1);
    ne_out(ne, NE_RBCR0, 0);
    ne_out(ne, NE_RBCR1, 0);
    ne_out(ne, NE_RCR, RCR_MON);
    ne_out(ne, NE_TCR, TCR_LOOPBACK);
    ne_out(ne, NE_TPSR, TX_PAGE);
    ne_out(ne, NE_PSTART, RX_START);
    ne_out(ne, NE_BNRY, RX_START);
    ne_out(ne, NE_PSTOP, RX_STOP);
    ne_out(ne, NE_ISR, 0xFF);
    ne_out(ne, NE_IMR, 0x00);                   /* unmasked at the end if interrupt driven */
    ne_out(ne, NE_CR, CR_PAGE1 | CR_STP | CR_RD2);
    for (int i = 0; i < ETH_ALEN; i++) ne_out(ne, NE_PAR0 + i, ne->netif.mac[i]);
    for (int i = 0; i < 8; i++) ne_out(ne, NE_MAR0 + i, 0xFF);
    ne_out(ne, NE_CURR, RX_START + 1);
    ne->next_pkt = RX_START + 1;
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_RD2);
    ne_out(ne, NE_TCR, 0x00);
    ne_out(ne, NE_RCR, RCR_AB | RCR_AM);
    ne_out(ne, NE_ISR, 0xFF);
    if (ne->irq) ne_out(ne, NE_IMR, IMR_BITS);
    spin_unlock(&ne->page_lock);
}

static int ne_send(struct netif *n, const void *frame, size_t len) {
    struct ne2000 *ne = n->priv;
    if (len > ETH_FRAME_MAX) { n->tx_errors++; return -EINVAL; }
    /* wait for the previous transmission to finish */
    for (int i = 0; i < 200000 && (ne_in(ne, NE_CR) & CR_TXP); i++) pause();
    size_t sz = len < 60 ? 60 : len;
    memcpy(ne->txbuf, frame, len);
    if (sz > len) memset(ne->txbuf + len, 0, sz - len);
    ne_write_mem(ne, TX_PAGE << 8, ne->txbuf, sz);
    ne_out(ne, NE_TPSR, TX_PAGE);
    ne_out(ne, NE_TBCR0, sz & 0xFF);
    ne_out(ne, NE_TBCR1, sz >> 8);
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_TXP | CR_RD2);
    n->tx_packets++; n->tx_bytes += len;
    return 0;
}

static uint8_t ne_curr(struct ne2000 *ne) {
    spin_lock(&ne->page_lock);
    ne_out(ne, NE_CR, CR_PAGE1 | CR_STA | CR_RD2);
    uint8_t c = ne_in(ne, NE_CURR);
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STA | CR_RD2);
    spin_unlock(&ne->page_lock);
    return c;
}

/* interrupt context: quiet the card until the network thread has drained it */
static void ne_irq(struct regs *r, void *ctx) {
    struct ne2000 *ne = ctx;
    spin_lock(&ne->page_lock);                     /* another CPU may have page 1 selected */
    bool ours = ne_in(ne, NE_ISR) & IMR_BITS;
    if (ours) ne_out(ne, NE_IMR, 0);
    spin_unlock(&ne->page_lock);
    if (!ours) return;                             /* shared line, not us */
    ne->irqs++;
    net_kick();
}

static int ne_drain(struct netif *n);
static int ne_poll(struct netif *n) {
    struct ne2000 *ne = n->priv;
    int count = ne_drain(n);
    if (ne->irq) ne_out(ne, NE_IMR, IMR_BITS);   /* anything still pending in ISR fires again at once */
    return count;
}

static int ne_drain(struct netif *n) {
    struct ne2000 *ne = n->priv;
    uint8_t isr = ne_in(ne, NE_ISR);
    /* idle polls cost a single register read; the ring is still checked now and then in case
     * a receive status was missed. PRX was acknowledged before the last drain, so frames that it
     * left behind (backlog) raise no new status and must not wait for that occasional check */
    if (!ne->backlog && !(isr & (ISR_PRX | ISR_RXE | ISR_OVW | ISR_PTX | ISR_TXE | ISR_CNT)) && ++ne->idle_polls < 100) return 0;
    ne->idle_polls = 0;
    ne->backlog = false;
    if (isr & ISR_OVW) {
        /* receive ring overflow: reset the chip and start over with an empty ring */
        kprintf("%s: receive ring overflow, resetting\n", n->name);
        n->rx_dropped++;
        ne_setup(ne);
        return 0;
    }
    if (isr & (ISR_PTX | ISR_TXE)) { if (isr & ISR_TXE) n->tx_errors++; ne_out(ne, NE_ISR, ISR_PTX | ISR_TXE); }
    if (isr & ISR_RXE) { n->rx_errors++; ne_out(ne, NE_ISR, ISR_RXE); }
    if (isr & ISR_CNT) { ne_in(ne, 0x0D); ne_in(ne, 0x0E); ne_in(ne, 0x0F); ne_out(ne, NE_ISR, ISR_CNT); }
    /* acknowledge before draining: a frame that lands after this sets the bit again */
    ne_out(ne, NE_ISR, ISR_PRX);

    /* count is every ring entry consumed, bad ones included, so a budget-limited drain always
     * reports work and the network thread polls again at once instead of sleeping */
    int count = 0;
    for (int budget = 0;; budget++) {
        uint8_t curr = ne_curr(ne);
        if (ne->next_pkt == curr) break;
        if (budget == 32) { ne->backlog = true; break; }
        uint8_t hdr[4];
        ne_read_mem(ne, ne->next_pkt << 8, hdr, 4);
        uint8_t status = hdr[0], next = hdr[1];
        uint16_t len = hdr[2] | (hdr[3] << 8);
        if (next < RX_START || next >= RX_STOP || len < 4 + 14 || len > ETH_FRAME_MAX + 4 + 4) {
            /* corrupt ring header: resynchronise */
            n->rx_errors++;
            ne_setup(ne);
            break;
        }
        size_t plen = len - 4;
        if ((status & 0x01) && plen <= ETH_FRAME_MAX + 4) {
            uint16_t start = (ne->next_pkt << 8) + 4;
            uint16_t ring_end = RX_STOP << 8;
            if (start + plen > ring_end) {
                size_t first = ring_end - start;
                ne_read_mem(ne, start, ne->rxbuf, first);
                ne_read_mem(ne, RX_START << 8, ne->rxbuf + first, plen - first);
            } else {
                ne_read_mem(ne, start, ne->rxbuf, plen);
            }
            /* real cards store the frame check sequence after the frame (QEMU does not);
             * IPv4 and ARP carry their own lengths, so trailing bytes are harmless */
            n->rx_packets++; n->rx_bytes += plen;
            eth_input(n, ne->rxbuf, plen);
        } else {
            n->rx_errors++;
        }
        ne->next_pkt = next;
        ne_out(ne, NE_BNRY, next == RX_START ? RX_STOP - 1 : next - 1);
        count++;
    }
    return count;
}

/* reset the card and read its station address; false if nothing NE2000-like responds */
static bool ne_probe(struct ne2000 *ne) {
    if (inb(ne->io + NE_CR) == 0xFF) return false;
    outb(ne->io + NE_RESET, inb(ne->io + NE_RESET));
    int i;
    for (i = 0; i < 100000 && !(inb(ne->io + NE_ISR) & ISR_RST); i++) pause();
    if (i == 100000) return false;
    ne_out(ne, NE_ISR, 0xFF);
    ne_out(ne, NE_CR, CR_PAGE0 | CR_STP | CR_RD2);
    ne_out(ne, NE_DCR, DCR_WORD | DCR_LS | DCR_FT1);
    ne->word = true;
    ne_out(ne, NE_RBCR0, 0); ne_out(ne, NE_RBCR1, 0);
    ne_out(ne, NE_IMR, 0);
    ne_out(ne, NE_ISR, 0xFF);
    ne_out(ne, NE_RCR, RCR_MON);
    ne_out(ne, NE_TCR, TCR_LOOPBACK);
    /* the 32-byte station PROM: in word mode every byte appears twice */
    uint8_t prom[32];
    ne_read_mem(ne, 0, prom, 32);
    bool doubled = true;
    for (int k = 0; k < 6; k++) if (prom[2 * k] != prom[2 * k + 1]) doubled = false;
    for (int k = 0; k < 6; k++) ne->netif.mac[k] = doubled ? prom[2 * k] : prom[k];
    bool zero = true;
    for (int k = 0; k < 6; k++) if (ne->netif.mac[k] != 0x00 && ne->netif.mac[k] != 0xFF) zero = false;
    if (zero) return false;
    if (!doubled) { ne->word = false; }
    return true;
}

static void ne_attach(uint16_t io, const char *desc, struct pci_device *pci) {
    struct ne2000 *ne = kzalloc(sizeof *ne);
    ne->io = io;
    if (!ne_probe(ne)) { kfree(ne); return; }
    char how[64] = "no interrupt line";
    ne->irq = pci && pci_irq_install(pci, ne_irq, ne, true, how, sizeof how);
    ne_setup(ne);
    struct netif *n = &ne->netif;
    snprintf(n->name, sizeof n->name, "ne%d", ncards++);
    strlcpy(n->driver, desc, sizeof n->driver);
    n->send = ne_send;
    n->poll = ne_poll;
    n->irq_driven = ne->irq;
    n->priv = ne;
    n->up = true;
    /* the receive ring holds nine full-size frames, and every frame takes at least one 256-byte
     * page however short it is: keep TCP senders well within that */
    n->rx_window = 6 * 1460;
    char mac[18]; mac_to_str(n->mac, mac);
    kprintf("%s: %s at io %#x, %d-bit, MAC %s, %s%s\n", n->name, desc, io, ne->word ? 16 : 8, mac, ne->irq ? "" : "polled: ", how);
    net_register(n);
}

static const struct { uint16_t vendor, device; const char *name; } pci_ids[] = {
    { 0x10EC, 0x8029, "Realtek RTL8029 (NE2000)" },
    { 0x1050, 0x0940, "Winbond 89C940 (NE2000)" },
    { 0x1050, 0x5A5A, "Winbond W89C940F (NE2000)" },
    { 0x11F6, 0x1401, "Compex RL2000 (NE2000)" },
    { 0x8E2E, 0x3000, "KTI ET32P2 (NE2000)" },
    { 0x4A14, 0x5000, "NetVin NV5000SC (NE2000)" },
    { 0x1106, 0x0926, "VIA 86C926 (NE2000)" },
    { 0x10BD, 0x0E34, "SureCom NE34 (NE2000)" },
    { 0x12C3, 0x0058, "Holtek HT80232 (NE2000)" },
    { 0x12C3, 0x5598, "Holtek HT80229 (NE2000)" },
};

void ne2000_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        for (size_t k = 0; k < ARRAY_LEN(pci_ids); k++) {
            if (d->vendor != pci_ids[k].vendor || d->device != pci_ids[k].device) continue;
            if (d->bar_is_mmio[0] || !d->bar_addr[0]) continue;
            pci_enable_bus_master(d);
            ne_attach((uint16_t)d->bar_addr[0], pci_ids[k].name, d);
        }
    }
    if (ncards) return;
    /* ISA cards sit at one of a few jumpered I/O bases */
    static const uint16_t isa_bases[] = { 0x300, 0x280, 0x320, 0x340, 0x360 };
    for (size_t i = 0; i < ARRAY_LEN(isa_bases) && !ncards; i++) ne_attach(isa_bases[i], "NE2000 (ISA)", NULL);
}
