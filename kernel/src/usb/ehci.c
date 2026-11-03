/* EHCI (USB 2.0) host controller driver. Control and bulk endpoints sit on
 * the async schedule, interrupt endpoints on the periodic schedule under
 * skeleton QHs for intervals of 1..256 frames. Low/full-speed devices behind
 * high-speed hubs use split transactions; on root ports they are handed to
 * the companion UHCI/OHCI controller, which stays with the BIOS's legacy
 * emulation. Each endpoint carries one transfer at a time, which is all the
 * class drivers ask for. All memory stays below 4 GiB. */
#include "usb.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "io.h"

#define USBCMD    0x00
#define USBSTS    0x04
#define USBINTR   0x08
#define CTRLDSSEG 0x10
#define PERIODIC  0x14
#define ASYNC     0x18
#define CONFIGFLAG 0x40
#define PORTSC(p) (0x44 + 4 * ((p) - 1))

#define CMD_RS    (1u << 0)
#define CMD_RESET (1u << 1)
#define CMD_PSE   (1u << 4)
#define CMD_ASE   (1u << 5)
#define CMD_IAAD  (1u << 6)
#define STS_INT   (1u << 0)
#define STS_ERR   (1u << 1)
#define STS_PCD   (1u << 2)
#define STS_HSE   (1u << 4)
#define STS_IAA   (1u << 5)
#define STS_HALT  (1u << 12)

#define PORT_CCS  (1u << 0)
#define PORT_CSC  (1u << 1)
#define PORT_PE   (1u << 2)
#define PORT_PEC  (1u << 3)
#define PORT_OCC  (1u << 5)
#define PORT_PR   (1u << 8)
#define PORT_PP   (1u << 12)
#define PORT_OWNER (1u << 13)
#define PORT_RWC  (PORT_CSC | PORT_PEC | PORT_OCC)

#define LINK_T    1u
#define LINK_QH   2u
#define TOK_ACTIVE 0x80u
#define TOK_HALTED 0x40u
#define TOK_ERRS   0x38u     /* data buffer error, babble, transaction error */
#define PID_OUT   0
#define PID_IN    1
#define PID_SETUP 2

#define QH_SIZE  128
#define QTD_SIZE 64
#define MAXQTD   8

struct eqh {
    uint32_t *hw; uint64_t phys;
    struct usb_device *dev;
    struct usb_endpoint *ep;
    bool periodic;
    int bucket;                      /* periodic: interval 2^bucket frames */
    uint32_t *stop; uint64_t stop_phys;   /* inactive qTD that ends a transfer after a short packet */
    uint8_t *setup; uint64_t setup_phys;  /* control: the 8-byte setup packet */
    struct usb_xfer *xfer;
    uint32_t *qtd[MAXQTD]; uint32_t qlen[MAXQTD]; bool qdata[MAXQTD];
    int nq;
    bool control;
    struct eqh *snext;               /* next in its schedule list (async ring or periodic bucket) */
    struct eqh *anext;               /* all QHs of the controller */
};

struct edev { struct eqh *qh[32]; };

struct ehci {
    struct usb_hcd hcd;
    volatile uint8_t *cap, *op;
    int nports, ncc;
    spinlock_t lock;
    struct mutex sched_lock;         /* unlink/relink, which waits for the controller */
    uint32_t *frames;
    struct eqh *async_head, *skel[9];
    struct eqh *all;
    uint8_t addr_used[128];
    uint8_t *free64, *free128;       /* pool free lists */
    volatile uint64_t irqs;
};

static inline uint32_t rd(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
#define wmb() __asm__ volatile("" ::: "memory")

/* ---- pool of 64/128-byte chunks below 4 GiB; caller holds e->lock ---- */

static void *pool_get(struct ehci *e, int size) {
    uint8_t **fl = size == 64 ? &e->free64 : &e->free128;
    if (!*fl) {
        uint64_t p = pmm_alloc_pages_low(1);
        if (!p) return NULL;
        uint8_t *v = P2V(p);
        for (int o = 0; o < (int)PAGE_SIZE; o += size) { *(uint8_t **)(v + o) = *fl; *fl = v + o; }
    }
    uint8_t *c = *fl;
    *fl = *(uint8_t **)c;
    memset(c, 0, size);
    return c;
}
static void pool_put(struct ehci *e, void *c, int size) {
    uint8_t **fl = size == 64 ? &e->free64 : &e->free128;
    *(uint8_t **)c = *fl;
    *fl = c;
}
static uint32_t phys32(const void *v) { return (uint32_t)V2P(v); }

/* ---- queue heads ---- */

static int qh_index(const struct usb_endpoint *ep) { return ep->type == USB_EP_CONTROL ? 0 : (ep->addr & 0xF) * 2 + ((ep->addr & USB_DIR_IN) ? 1 : 0); }

static void qh_chars(struct eqh *q) {
    struct usb_device *d = q->dev;
    struct usb_endpoint *ep = q->ep;
    bool hs = d->speed == USB_SPEED_HIGH;
    int eps = hs ? 2 : d->speed == USB_SPEED_LOW ? 1 : 0;
    q->hw[1] = d->addr | (ep->addr & 0xF) << 8 | eps << 12 | (q->control ? 1u << 14 : 0) | (uint32_t)ep->mps << 16
             | (q->control && !hs ? 1u << 27 : 0) | (hs && !q->periodic ? 4u << 28 : 0);
    uint32_t caps = (uint32_t)(hs ? ep->mult + 1 : 1) << 30;
    if (!hs && d->tt_hub) caps |= (uint32_t)d->tt_hub->addr << 16 | (uint32_t)d->tt_port << 23;
    if (q->periodic) caps |= hs ? 0x01 : 0x01 | 0x1C << 8;   /* start in microframe 0; complete splits in 2-4 */
    q->hw[2] = caps;
}

static void overlay_reset(struct eqh *q, bool keep_toggle) {
    q->hw[3] = 0; q->hw[4] = LINK_T; q->hw[5] = LINK_T;
    q->hw[6] = keep_toggle ? q->hw[6] & 0x80000001u : 0;
    for (int i = 7; i < 17; i++) q->hw[i] = 0;
}

static void qh_link(struct ehci *e, struct eqh *q) {
    struct eqh *head = q->periodic ? e->skel[q->bucket] : e->async_head;
    spin_lock(&e->lock);
    q->hw[0] = head->hw[0];
    q->snext = head->snext;
    wmb();
    head->hw[0] = (uint32_t)q->phys | LINK_QH;
    head->snext = q;
    spin_unlock(&e->lock);
}

/* take the QH off its schedule and wait until the controller cannot be using it */
static void qh_unlink(struct ehci *e, struct eqh *q) {
    struct eqh *p = q->periodic ? e->skel[q->bucket] : e->async_head;
    spin_lock(&e->lock);
    while (p && p->snext != q) p = p->snext;
    if (p) { p->hw[0] = q->hw[0]; p->snext = q->snext; }
    spin_unlock(&e->lock);
    if (!p) return;
    if (q->periodic) { thread_sleep_ms(2); return; }
    wr(e->op, USBSTS, STS_IAA);
    wr(e->op, USBCMD, rd(e->op, USBCMD) | CMD_IAAD);   /* interrupt on async advance doorbell */
    int i = 0;
    while (i < 50 && !(rd(e->op, USBSTS) & STS_IAA)) { thread_sleep_ms(1); i++; }
    if (i == 50) { kprintf("ehci: async advance doorbell timed out\n"); thread_sleep_ms(10); }
    wr(e->op, USBSTS, STS_IAA);
}

static struct eqh *qh_new(struct ehci *e, struct usb_device *d, struct usb_endpoint *ep) {
    spin_lock(&e->lock);
    struct eqh *q = kzalloc(sizeof *q);
    q->hw = pool_get(e, QH_SIZE);
    q->stop = pool_get(e, QTD_SIZE);
    q->setup = pool_get(e, QTD_SIZE);
    if (!q->hw || !q->stop || !q->setup) { spin_unlock(&e->lock); return NULL; }   /* low memory exhausted: leaked */
    q->phys = V2P(q->hw); q->stop_phys = V2P(q->stop); q->setup_phys = V2P(q->setup);
    q->stop[0] = LINK_T; q->stop[1] = LINK_T; q->stop[2] = 0;
    q->dev = d; q->ep = ep;
    q->control = ep->type == USB_EP_CONTROL;
    q->periodic = ep->type == USB_EP_INTR;
    if (q->periodic) {
        int frames = d->speed == USB_SPEED_HIGH ? (1 << (CLAMP(ep->interval, 1, 16) - 1)) / 8 : ep->interval;
        frames = CLAMP(frames, 1, 256);
        q->bucket = 31 - __builtin_clz(frames);
    }
    overlay_reset(q, false);
    q->anext = e->all; e->all = q;
    spin_unlock(&e->lock);
    qh_chars(q);
    return q;
}

static void qh_free_chain(struct ehci *e, struct eqh *q) {
    for (int i = 0; i < q->nq; i++) pool_put(e, q->qtd[i], QTD_SIZE);
    q->nq = 0;
}

static void qh_destroy(struct ehci *e, struct eqh *q) {
    spin_lock(&e->lock);
    for (struct eqh **pp = &e->all; *pp; pp = &(*pp)->anext) if (*pp == q) { *pp = q->anext; break; }
    qh_free_chain(e, q);
    pool_put(e, q->hw, QH_SIZE); pool_put(e, q->stop, QTD_SIZE); pool_put(e, q->setup, QTD_SIZE);
    spin_unlock(&e->lock);
    kfree(q);
}

/* ---- completions ---- */

/* 1 when the queued transfer finished (status and actual filled in) */
static int qh_check(struct eqh *q, int *status, uint32_t *actual) {
    uint32_t got = 0;
    for (int i = 0; i < q->nq; i++) {
        uint32_t tok = q->qtd[i][2];
        if (tok & TOK_ACTIVE) return 0;
        if (tok & TOK_HALTED) { *status = (tok & TOK_ERRS) ? -EIO : -EPIPE; *actual = got; return 1; }
        uint32_t left = (tok >> 16) & 0x7FFF;
        if (q->qdata[i]) got += q->qlen[i] - MIN(left, q->qlen[i]);
        /* a short packet: bulk/interrupt stop here, control goes on to its status stage */
        if (left && q->qdata[i] && !q->control) break;
    }
    *status = 0; *actual = got;
    return 1;
}

static void ehci_poll(struct ehci *e) {
    struct usb_xfer *done = NULL, **tail = &done;
    uint64_t f = irq_save();
    spin_lock(&e->lock);
    for (struct eqh *q = e->all; q; q = q->anext) {
        int st; uint32_t act;
        if (!q->xfer || !qh_check(q, &st, &act)) continue;
        struct usb_xfer *x = q->xfer;
        q->xfer = NULL;
        x->status = st; x->actual = act;
        x->next = NULL; *tail = x; tail = &x->next;
    }
    spin_unlock(&e->lock);
    for (struct usb_xfer *x = done, *n; x; x = n) { n = x->next; usb_complete(x, x->status, x->actual); }
    irq_restore(f);
}

static void ehci_irq(struct regs *r, void *ctx) {
    struct ehci *e = ctx;
    uint32_t sts = rd(e->op, USBSTS) & (STS_INT | STS_ERR | STS_PCD | STS_HSE);
    if (!sts) return;
    wr(e->op, USBSTS, sts);
    e->irqs++;
    if (sts & STS_HSE) kprintf("ehci: host system error\n");
    if (sts & STS_PCD) usb_hub_event();
    ehci_poll(e);
}

static void poll_thread(void *arg) {
    struct ehci *e = arg;
    for (;;) { thread_sleep_ms(e->irqs ? 50 : 2); ehci_poll(e); }
}

/* ---- transfers ---- */

static uint32_t *qtd_new(struct ehci *e, struct eqh *q, uint64_t phys, uint32_t len, int pid, int toggle, bool data) {
    uint32_t *t = pool_get(e, QTD_SIZE);
    if (!t) return NULL;
    t[0] = LINK_T; t[1] = LINK_T;
    t[2] = TOK_ACTIVE | pid << 8 | 3u << 10 | len << 16 | (uint32_t)toggle << 31;
    t[3] = (uint32_t)phys;
    for (int i = 1; i < 5; i++) t[3 + i] = (uint32_t)((phys & ~0xFFFULL) + i * 4096);
    if (q->nq) q->qtd[q->nq - 1][0] = phys32(t);
    q->qtd[q->nq] = t; q->qlen[q->nq] = len; q->qdata[q->nq] = data;
    q->nq++;
    return t;
}

static int ehci_submit(struct usb_xfer *x) {
    struct usb_device *d = x->dev;
    struct ehci *e = d->hcd->priv;
    bool in = usb_ep_in(x);
    uint64_t phys = x->len ? V2P(x->data) : 0;
    spin_lock(&e->lock);
    struct edev *ed = d->hcpriv;
    struct eqh *q = ed && !d->gone ? ed->qh[qh_index(x->ep)] : NULL;
    if (!q) { spin_unlock(&e->lock); return !ed || d->gone ? -ENODEV : -EINVAL; }
    if (q->xfer) { spin_unlock(&e->lock); return -EBUSY; }
    qh_free_chain(e, q);   /* the previous transfer's qTDs */
    int pid = in ? PID_IN : PID_OUT, r = 0;
    if (q->control) {
        memcpy(q->setup, x->setup, 8);
        if (!qtd_new(e, q, q->setup_phys, 8, PID_SETUP, 0, false)) r = -ENOMEM;
    }
    for (uint32_t off = 0; r == 0 && (off < x->len || (!x->len && !q->control && !q->nq)); ) {
        uint32_t c = MIN(x->len - off, 5 * 4096 - ((phys + off) & 0xFFF));
        if (off + c < x->len) c -= c % x->ep->mps;   /* only the last qTD may end in a short packet */
        if (q->nq >= MAXQTD - 1 || !qtd_new(e, q, phys + off, c, pid, 1, true)) r = -ENOMEM;
        off += c;
    }
    if (q->control && r == 0 && !qtd_new(e, q, 0, 0, (x->len && in) ? PID_OUT : PID_IN, 1, false)) r = -ENOMEM;
    if (r < 0) { qh_free_chain(e, q); spin_unlock(&e->lock); return r; }
    /* a short IN packet continues at the status stage (control) or the stop qTD */
    uint32_t alt = q->control ? phys32(q->qtd[q->nq - 1]) : (uint32_t)q->stop_phys;
    for (int i = 0; i < q->nq - 1; i++) if (q->qdata[i]) q->qtd[i][1] = alt;
    q->qtd[q->nq - 1][2] |= 1u << 15;   /* IOC */
    q->xfer = x;
    /* Point the overlay at the new chain before clearing its token: a halted
     * overlay may still name a qTD of the old chain, which the pool has just
     * handed out again inside this one */
    wmb();
    q->hw[4] = phys32(q->qtd[0]);
    q->hw[5] = LINK_T;
    wmb();
    q->hw[6] &= 0x80000001u;   /* inactive overlay keeps the data toggle and ping state */
    spin_unlock(&e->lock);
    return 0;
}

/* stop the endpoint, fail its transfer, reset the overlay (toggle 0 unless keep);
 * the caller holds sched_lock, so the device cannot be freed meanwhile */
static void qh_reset(struct ehci *e, struct eqh *q, int status, bool keep_toggle) {
    qh_unlink(e, q);
    uint64_t f = irq_save();
    spin_lock(&e->lock);
    struct usb_xfer *x = q->xfer;
    q->xfer = NULL;
    overlay_reset(q, keep_toggle);
    spin_unlock(&e->lock);
    if (x) usb_complete(x, status, 0);
    irq_restore(f);
    qh_link(e, q);
}

static void ehci_abort(struct usb_xfer *x) {
    struct ehci *e = x->dev->hcd->priv;
    mutex_lock(&e->sched_lock);
    struct edev *ed = x->dev->hcpriv;
    struct eqh *q = ed ? ed->qh[qh_index(x->ep)] : NULL;
    if (q && q->xfer == x) qh_reset(e, q, -ETIMEDOUT, true);
    mutex_unlock(&e->sched_lock);
}

static int ehci_reset_ep(struct usb_device *d, struct usb_endpoint *ep) {
    struct ehci *e = d->hcd->priv;
    mutex_lock(&e->sched_lock);
    struct edev *ed = d->hcpriv;
    struct eqh *q = ed ? ed->qh[qh_index(ep)] : NULL;
    if (q) qh_reset(e, q, -EIO, false);
    mutex_unlock(&e->sched_lock);
    return q ? 0 : -ENODEV;
}

/* ---- devices ---- */

/* the controller may cache a linked QH: change its fields only while unlinked */
static void qh_rewrite(struct ehci *e, struct eqh *q) {
    mutex_lock(&e->sched_lock);
    qh_unlink(e, q);
    qh_chars(q);
    qh_link(e, q);
    mutex_unlock(&e->sched_lock);
}

static int ehci_enable_device(struct usb_device *d) {
    struct ehci *e = d->hcd->priv;
    if (d->speed != USB_SPEED_HIGH && !d->tt_hub) return -ENODEV;   /* belongs to a companion controller */
    struct edev *ed = kzalloc(sizeof *ed);
    d->hcpriv = ed;
    if (!(ed->qh[0] = qh_new(e, d, &d->ep0))) return -ENOMEM;
    mutex_lock(&e->sched_lock); qh_link(e, ed->qh[0]); mutex_unlock(&e->sched_lock);
    uint8_t buf[8];
    if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, buf, 8) < 8) return -EIO;
    if (buf[7] == 8 || buf[7] == 16 || buf[7] == 32 || buf[7] == 64) d->ep0.mps = buf[7];
    int addr = 0;
    spin_lock(&e->lock);
    for (int a = 1; a < 128 && !addr; a++) if (!e->addr_used[a]) { e->addr_used[a] = 1; addr = a; }
    spin_unlock(&e->lock);
    if (!addr) return -ENOSPC;
    qh_rewrite(e, ed->qh[0]);   /* new max packet size, still address 0 */
    if (usb_control(d, 0, USB_REQ_SET_ADDRESS, addr, 0, NULL, 0) < 0) { e->addr_used[addr] = 0; return -EIO; }
    thread_sleep_ms(2);
    d->addr = addr;
    qh_rewrite(e, ed->qh[0]);
    return 0;
}

static int ehci_configure(struct usb_device *d) {
    struct ehci *e = d->hcd->priv;
    struct edev *ed = d->hcpriv;
    for (int i = 0; i < d->nintf; i++) for (int k = 0; k < d->intf[i].nep; k++) {
        struct usb_endpoint *ep = &d->intf[i].ep[k];
        int idx = qh_index(ep);
        if (ep->type == USB_EP_ISOC || ed->qh[idx]) continue;
        if (!(ed->qh[idx] = qh_new(e, d, ep))) return -ENOMEM;
        mutex_lock(&e->sched_lock); qh_link(e, ed->qh[idx]); mutex_unlock(&e->sched_lock);
    }
    return 0;
}

static void ehci_free_device(struct usb_device *d) {
    struct ehci *e = d->hcd->priv;
    mutex_lock(&e->sched_lock);
    spin_lock(&e->lock);
    struct edev *ed = d->hcpriv;
    d->hcpriv = NULL;   /* submit fails from here on */
    spin_unlock(&e->lock);
    if (!ed) { mutex_unlock(&e->sched_lock); return; }
    for (int i = 0; i < 32; i++) {
        struct eqh *q = ed->qh[i];
        if (!q) continue;
        qh_unlink(e, q);
        uint64_t f = irq_save();
        spin_lock(&e->lock);
        struct usb_xfer *x = q->xfer;
        q->xfer = NULL;
        spin_unlock(&e->lock);
        if (x) usb_complete(x, -ENODEV, 0);
        irq_restore(f);
        qh_destroy(e, q);
    }
    mutex_unlock(&e->sched_lock);
    if (d->addr) e->addr_used[d->addr] = 0;
    kfree(ed);
}

/* ---- root hub ---- */

static void ehci_port_status(struct usb_hcd *h, int port, struct usb_port_status *st) {
    struct ehci *e = h->priv;
    uint32_t v = rd(e->op, PORTSC(port));
    st->connected = (v & PORT_CCS) && !(v & PORT_OWNER);
    st->enabled = v & PORT_PE;
    st->changed = v & PORT_CSC;
    st->speed = USB_SPEED_HIGH;   /* only high-speed devices stay on an EHCI root port */
    if (v & PORT_RWC) wr(e->op, PORTSC(port), v);
}

static void release_port(struct ehci *e, int port, const char *why) {
    if (!e->ncc) { kprintf("ehci: port %d: %s device and no companion controller\n", port, why); return; }
    wr(e->op, PORTSC(port), (rd(e->op, PORTSC(port)) & ~PORT_RWC) | PORT_OWNER);
    kprintf("ehci: port %d: %s device, handed to the companion controller\n", port, why);
}

static int ehci_port_reset(struct usb_hcd *h, int port) {
    struct ehci *e = h->priv;
    uint32_t v = rd(e->op, PORTSC(port));
    if (!(v & PORT_CCS)) return -ENODEV;
    if (((v >> 10) & 3) == 1) { release_port(e, port, "low-speed"); return -ENODEV; }   /* K state */
    wr(e->op, PORTSC(port), (v & ~(PORT_RWC | PORT_PE)) | PORT_PR);
    thread_sleep_ms(50);
    wr(e->op, PORTSC(port), rd(e->op, PORTSC(port)) & ~(PORT_RWC | PORT_PR));
    for (int i = 0; i < 20 && (rd(e->op, PORTSC(port)) & PORT_PR); i++) thread_sleep_ms(1);
    thread_sleep_ms(2);
    v = rd(e->op, PORTSC(port));
    if (!(v & PORT_CCS)) return -ENODEV;
    if (!(v & PORT_PE)) { release_port(e, port, "full-speed"); return -ENODEV; }
    return 0;
}

static const struct usb_hcd_ops ehci_ops = {
    .port_status = ehci_port_status, .port_reset = ehci_port_reset,
    .enable_device = ehci_enable_device, .configure = ehci_configure, .free_device = ehci_free_device,
    .submit = ehci_submit, .abort = ehci_abort, .reset_ep = ehci_reset_ep,
};

/* ---- bring-up ---- */

static void bios_handoff(struct ehci *e, struct pci_device *pd) {
    uint8_t off = (rd(e->cap, 8) >> 8) & 0xFF;
    for (int guard = 0; off >= 0x40 && guard < 16; guard++) {
        uint32_t v = pci_read32(pd->bus, pd->slot, pd->func, off);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                pci_write32(pd->bus, pd->slot, pd->func, off, v | (1u << 24));
                for (int i = 0; i < 100 && (pci_read32(pd->bus, pd->slot, pd->func, off) & (1u << 16)); i++) thread_sleep_ms(10);
                if (pci_read32(pd->bus, pd->slot, pd->func, off) & (1u << 16)) {
                    kprintf("ehci: BIOS did not release the controller, taking it\n");
                    pci_write32(pd->bus, pd->slot, pd->func, off, (v & ~(1u << 16)) | (1u << 24));
                } else kprintf("ehci: took ownership from the BIOS\n");
            }
            pci_write32(pd->bus, pd->slot, pd->func, off + 4, 0xE0000000u);   /* SMIs off, clear their status */
        }
        off = (v >> 8) & 0xFF;
    }
}

static struct eqh *skel_new(struct ehci *e) {
    struct eqh *q = kzalloc(sizeof *q);
    q->hw = pool_get(e, QH_SIZE);
    if (!q->hw) return NULL;
    q->phys = V2P(q->hw);
    q->hw[1] = 2u << 12 | 64u << 16;   /* high-speed, never has work */
    q->hw[2] = 1u << 30 | 0x01;
    overlay_reset(q, false);
    return q;
}

static void ehci_probe(struct pci_device *pd) {
    if (!pd->bar_addr[0] || !pd->bar_is_mmio[0]) return;
    pci_enable_bus_master(pd);
    struct ehci *e = kzalloc(sizeof *e);
    e->cap = vmm_map_mmio(pd->bar_addr[0], 0x1000);
    uint32_t caplen = rd(e->cap, 0) & 0xFF, hcs = rd(e->cap, 4);
    e->op = e->cap + caplen;
    e->nports = hcs & 0xF; e->ncc = (hcs >> 12) & 0xF;
    e->hcd.dma32 = true;
    kprintf("ehci: controller %04x:%04x at %lx, %d ports, %d companion controller%s\n", pd->vendor, pd->device, pd->bar_addr[0],
            e->nports, e->ncc, e->ncc == 1 ? "" : "s");
    bios_handoff(e, pd);

    wr(e->op, USBINTR, 0);
    wr(e->op, USBCMD, rd(e->op, USBCMD) & ~CMD_RS);
    for (int i = 0; i < 20 && !(rd(e->op, USBSTS) & STS_HALT); i++) thread_sleep_ms(1);
    wr(e->op, USBCMD, CMD_RESET);
    for (int i = 0; i < 250 && (rd(e->op, USBCMD) & CMD_RESET); i++) thread_sleep_ms(1);
    if (rd(e->op, USBCMD) & CMD_RESET) { kprintf("ehci: reset timed out\n"); return; }

    mutex_init(&e->sched_lock);
    uint64_t fl = pmm_alloc_pages_low(1);
    if (!fl) return;
    e->frames = P2V(fl);
    spin_lock(&e->lock);
    for (int k = 0; k < 9; k++) if (!(e->skel[k] = skel_new(e))) { spin_unlock(&e->lock); return; }
    e->async_head = skel_new(e);
    spin_unlock(&e->lock);
    if (!e->async_head) return;
    for (int k = 0; k < 9; k++) e->skel[k]->hw[0] = k ? (uint32_t)e->skel[k - 1]->phys | LINK_QH : LINK_T;
    for (int i = 0; i < 1024; i++) {
        int k = i ? MIN(__builtin_ctz(i), 8) : 8;
        e->frames[i] = (uint32_t)e->skel[k]->phys | LINK_QH;
    }
    e->async_head->hw[0] = (uint32_t)e->async_head->phys | LINK_QH;   /* the async ring starts as just the head */
    e->async_head->hw[1] |= 1u << 15;                                 /* head of reclamation list */
    e->async_head->hw[2] = 1u << 30;

    wr(e->op, CTRLDSSEG, 0);
    wr(e->op, PERIODIC, (uint32_t)fl);
    wr(e->op, ASYNC, (uint32_t)e->async_head->phys);
    char how[64];
    if (!pci_irq_install(pd, ehci_irq, e, true, how, sizeof how)) kprintf("ehci: no interrupt (%s), polling\n", how);
    else kprintf("ehci: interrupt %s\n", how);
    wr(e->op, USBINTR, STS_INT | STS_ERR | STS_PCD | STS_HSE);
    wr(e->op, USBCMD, 1u << 16 | CMD_ASE | CMD_PSE | CMD_RS);   /* interrupt threshold: 1 microframe */
    wr(e->op, CONFIGFLAG, 1);   /* route every port to EHCI */
    thread_sleep_ms(5);
    if (hcs & (1 << 4)) for (int p = 1; p <= e->nports; p++) wr(e->op, PORTSC(p), (rd(e->op, PORTSC(p)) & ~PORT_RWC) | PORT_PP);
    thread_sleep_ms(20);

    e->hcd.name = "EHCI"; e->hcd.ops = &ehci_ops; e->hcd.priv = e; e->hcd.pci = pd; e->hcd.nports = e->nports;
    thread_create(kernel_process, "ehci", poll_thread, e);
    usb_register_hcd(&e->hcd);
}

void ehci_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->class_code == 0x0C && pd->subclass == 0x03 && pd->prog_if == 0x20) ehci_probe(pd);
    }
}
