/* xHCI (USB 3) host controller driver. One interrupter with a single event
 * ring segment, a command ring, and a one-page transfer ring per endpoint.
 * Completions arrive by MSI/INTx; a poll thread backs them up (every 2 ms
 * until the first interrupt is seen, then as a slow safety net). */
#include "usb.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "io.h"

#define USBCMD   0x00
#define USBSTS   0x04
#define PAGESIZE 0x08
#define CRCR     0x18
#define DCBAAP   0x30
#define CONFIG   0x38
#define PORTSC(p) (0x400 + 0x10 * ((p) - 1))
#define IMAN     0x00
#define IMOD     0x04
#define ERSTSZ   0x08
#define ERSTBA   0x10
#define ERDP     0x18

#define CMD_RS    (1 << 0)
#define CMD_HCRST (1 << 1)
#define CMD_INTE  (1 << 2)
#define STS_HCH   (1 << 0)
#define STS_HSE   (1 << 2)
#define STS_EINT  (1 << 3)
#define STS_PCD   (1 << 4)
#define STS_CNR   (1 << 11)

#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_CSC  (1u << 17)
#define PORT_WRC  (1u << 19)
#define PORT_PRC  (1u << 21)
#define PORT_WPR  (1u << 31)
#define PORT_CHANGES 0x00FE0000u
#define PORT_KEEP    0x4F00FFE9u   /* read-only and read/write-preserve bits: writing them back changes nothing */

#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_ISOCH    5
#define TRB_LINK     6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_DEQ      16
#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD       33
#define TRB_EV_PORT      34
#define TRB_EV_HC        37
#define TYPE(t)   ((uint32_t)(t) << 10)
#define TRB_TC    (1u << 1)    /* link: toggle cycle */
#define TRB_ISP   (1u << 2)
#define TRB_CH    (1u << 4)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_SIA   (1u << 31)   /* isochronous: start as soon as possible */

#define CC_SUCCESS 1
#define CC_STALL   6
#define CC_SHORT   13
#define CC_UNDERRUN 14
#define CC_OVERRUN  15
#define CC_STOPPED 26
#define CC_STOPPED_LEN 27
#define CC_STOPPED_SHORT 28

#define RING_TRBS 256
#define EV_TRBS   256

struct trb { uint32_t p0, p1, status, ctrl; };

struct ring {
    volatile struct trb *trb;
    uint64_t phys;
    int enq;
    uint32_t cycle;
    int used;                            /* TRBs owned by queued transfers */
    struct usb_xfer *owner[RING_TRBS];
    uint32_t off[RING_TRBS];             /* byte offset of each TRB's data within its transfer */
    struct usb_xfer *head, *tail;        /* queued transfers in ring order, linked through hcpriv */
};

struct xslot {
    int id;
    uint8_t *in, *out;
    uint64_t in_phys, out_phys;
    struct ring *ring[32];               /* by device context index */
};

struct xhci {
    struct usb_hcd hcd;
    volatile uint8_t *cap, *op, *ir;
    volatile uint32_t *db;
    int maxslots, maxports, csz;
    uint16_t version;
    uint64_t *dcbaa;
    struct ring *cmd;
    volatile struct trb *ev; uint64_t ev_phys; int ev_deq; uint32_t ev_cycle;
    struct xslot *slots[256];
    uint8_t port_major[256];
    spinlock_t lock;
    struct mutex cmd_lock;
    struct mutex ep_lock;                /* endpoint recovery against free_device */
    struct semaphore cmd_sem;
    volatile uint64_t cmd_trb;
    volatile int cmd_cc, cmd_slot;
    volatile uint64_t irqs;
};

struct done_list { struct usb_xfer *head, **tail; };

static inline uint32_t rd(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v) { wr(b, off, (uint32_t)v); wr(b, off + 4, (uint32_t)(v >> 32)); }
#define wmb() __asm__ volatile("" ::: "memory")

static uint64_t xalloc(struct xhci *x, size_t pages) { return x->hcd.dma32 ? pmm_alloc_pages_low(pages) : pmm_alloc_pages(pages); }

static int ep_dci(const struct usb_endpoint *ep) {
    if (ep->type == USB_EP_CONTROL) return 1;
    return (ep->addr & 0xF) * 2 + ((ep->addr & USB_DIR_IN) ? 1 : 0);
}
static uint32_t *ictx(struct xhci *x, struct xslot *s, int i) { return (uint32_t *)(s->in + i * x->csz); }
static uint32_t *octx(struct xhci *x, struct xslot *s, int i) { return (uint32_t *)(s->out + i * x->csz); }

/* ---- rings ---- */

static struct ring *ring_new(struct xhci *x) {
    struct ring *r = kzalloc(sizeof *r);
    if (!(r->phys = xalloc(x, 1))) { kfree(r); return NULL; }
    r->trb = P2V(r->phys);
    r->cycle = 1;
    r->trb[RING_TRBS - 1].p0 = (uint32_t)r->phys;
    r->trb[RING_TRBS - 1].p1 = (uint32_t)(r->phys >> 32);
    return r;
}
static void ring_free(struct ring *r) { if (r) { pmm_free_page(r->phys); kfree(r); } }

/* 'hold' writes the TRB with the cycle bit the controller does not own yet:
 * the first TRB of a transfer is handed over only once the rest is written */
static int ring_push(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status, uint32_t ctrl, struct usb_xfer *owner, uint32_t off, bool hold) {
    int i = r->enq;
    volatile struct trb *t = &r->trb[i];
    t->p0 = p0; t->p1 = p1; t->status = status;
    r->owner[i] = owner; r->off[i] = off;
    wmb();
    t->ctrl = (ctrl & ~1u) | (hold ? r->cycle ^ 1 : r->cycle);
    if (++r->enq == RING_TRBS - 1) {
        wmb();
        r->trb[RING_TRBS - 1].ctrl = TYPE(TRB_LINK) | TRB_TC | (ctrl & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return i;
}

static void finish(struct ring *r, struct usb_xfer *xf, int status, struct done_list *dl) {
    for (int k = 0, i = xf->hc_first; k < xf->hc_ntrb; k++) { r->owner[i] = NULL; if (++i == RING_TRBS - 1) i = 0; }
    struct usb_xfer *prev = NULL;
    for (struct usb_xfer *q = r->head; q; prev = q, q = q->hcpriv) {
        if (q != xf) continue;
        if (prev) prev->hcpriv = q->hcpriv; else r->head = q->hcpriv;
        if (r->tail == q) r->tail = prev;
        break;
    }
    r->used -= xf->hc_ntrb;
    xf->status = status;
    xf->next = NULL; *dl->tail = xf; dl->tail = &xf->next;
}
static void flush(struct ring *r, int status, struct done_list *dl) {
    for (int i = 0; i < RING_TRBS - 1; i++) if (r->owner[i]) finish(r, r->owner[i], status, dl);
}
/* callbacks run with interrupts still off, so nothing can free the transfers in between */
static void complete_all(struct done_list *dl) {
    for (struct usb_xfer *x = dl->head, *n; x; x = n) { n = x->next; usb_complete(x, x->status, x->actual); }
}

/* ---- events ---- */

static void transfer_event(struct xhci *x, volatile struct trb *e, struct done_list *dl) {
    int slot = e->ctrl >> 24, dci = (e->ctrl >> 16) & 31, cc = e->status >> 24;
    uint32_t resid = e->status & 0xFFFFFF;
    struct xslot *s = x->slots[slot];
    struct ring *r = s ? s->ring[dci] : NULL;
    if (!r) return;
    uint64_t ptr = e->p0 | (uint64_t)e->p1 << 32;
    /* An isochronous ring ran empty (OUT) or full (IN). The pointer, when
     * given, is where the controller stopped: transfers before it are done,
     * the one it names and later ones may have been queued since */
    if (cc == CC_UNDERRUN || cc == CC_OVERRUN) {
        if (ptr < r->phys || ptr >= r->phys + RING_TRBS * sizeof(struct trb)) return;
        int at = (ptr - r->phys) / sizeof(struct trb);
        while (r->head && r->owner[at] != r->head) finish(r, r->head, 0, dl);
        return;
    }
    if (ptr < r->phys || ptr >= r->phys + RING_TRBS * sizeof(struct trb)) return;
    int i = (ptr - r->phys) / sizeof(struct trb);
    struct usb_xfer *xf = r->owner[i];
    if (!xf) return;
    if (cc == CC_STOPPED || cc == CC_STOPPED_LEN || cc == CC_STOPPED_SHORT) return;   /* xhci_abort finishes it */
    uint32_t type = (r->trb[i].ctrl >> 10) & 63, tlen = r->trb[i].status & 0x1FFFF;
    bool control = xf->ep->type == USB_EP_CONTROL;
    if (type == TRB_NORMAL || type == TRB_DATA || type == TRB_ISOCH) xf->actual = r->off[i] + tlen - MIN(resid, tlen);
    if (xf->ep->type == USB_EP_ISOC) {
        /* Only the last packet interrupts. Missed packets (a Missed Service
         * Error may carry no TRB pointer) are not failures: an event for a
         * later transfer means the controller is done with the earlier ones */
        while (r->head && r->head != xf) finish(r, r->head, 0, dl);
        if (i == xf->hc_last) finish(r, xf, 0, dl);
        return;
    }
    if (cc == CC_SUCCESS || cc == CC_SHORT) {
        /* a short data stage of a control transfer still runs its status stage */
        if (i != xf->hc_last && (control || cc != CC_SHORT)) return;
        finish(r, xf, 0, dl);
    } else {
        if (cc != CC_STALL) kprintf("xhci: slot %d endpoint %d: completion code %d\n", slot, dci, cc);
        finish(r, xf, cc == CC_STALL ? -EPIPE : -EIO, dl);
    }
}

static void xhci_poll(struct xhci *x) {
    struct done_list dl = { NULL, &dl.head };
    uint64_t f = irq_save();
    spin_lock(&x->lock);
    bool any = false;
    for (;;) {
        volatile struct trb *e = &x->ev[x->ev_deq];
        uint32_t ctrl = e->ctrl;
        if ((ctrl & 1) != x->ev_cycle) break;
        wmb();
        switch ((ctrl >> 10) & 63) {
        case TRB_EV_TRANSFER: transfer_event(x, e, &dl); break;
        case TRB_EV_CMD:
            if ((e->p0 | (uint64_t)e->p1 << 32) == x->cmd_trb) { x->cmd_cc = e->status >> 24; x->cmd_slot = ctrl >> 24; x->cmd_trb = 0; sem_post(&x->cmd_sem); }
            break;
        case TRB_EV_PORT: usb_hub_event(); break;
        case TRB_EV_HC: kprintf("xhci: host controller event, code %u\n", e->status >> 24); break;
        }
        if (++x->ev_deq == EV_TRBS) { x->ev_deq = 0; x->ev_cycle ^= 1; }
        any = true;
    }
    if (any) wr64(x->ir, ERDP, (x->ev_phys + x->ev_deq * sizeof(struct trb)) | (1 << 3));
    spin_unlock(&x->lock);
    complete_all(&dl);
    irq_restore(f);
}

static void xhci_irq(struct regs *r, void *ctx) {
    struct xhci *x = ctx;
    uint32_t sts = rd(x->op, USBSTS), iman = rd(x->ir, IMAN);
    if (!(sts & STS_EINT) && !(iman & 1)) return;   /* shared line, not ours */
    wr(x->op, USBSTS, STS_EINT | STS_PCD);
    wr(x->ir, IMAN, iman | 1);
    x->irqs++;
    xhci_poll(x);
}

static void poll_thread(void *arg) {
    struct xhci *x = arg;
    for (;;) { thread_sleep_ms(x->irqs ? 50 : 2); xhci_poll(x); }
}

/* ---- commands ---- */

static const char *cmd_name(int t) {
    switch (t) { case TRB_ENABLE_SLOT: return "Enable Slot"; case TRB_DISABLE_SLOT: return "Disable Slot"; case TRB_ADDRESS_DEV: return "Address Device";
    case TRB_CONFIG_EP: return "Configure Endpoint"; case TRB_EVAL_CTX: return "Evaluate Context"; case TRB_RESET_EP: return "Reset Endpoint";
    case TRB_STOP_EP: return "Stop Endpoint"; case TRB_SET_DEQ: return "Set TR Dequeue Pointer"; default: return "command"; }
}

static int xhci_cmd(struct xhci *x, uint64_t param, uint32_t ctrl, int *slot) {
    mutex_lock(&x->cmd_lock);
    while (sem_trywait(&x->cmd_sem)) {}   /* a late completion of a timed-out command */
    spin_lock(&x->lock);
    int i = x->cmd->enq;
    x->cmd_trb = x->cmd->phys + i * sizeof(struct trb);
    ring_push(x->cmd, (uint32_t)param, (uint32_t)(param >> 32), 0, ctrl, NULL, 0, false);
    x->db[0] = 0;
    spin_unlock(&x->lock);
    int r = 0, type = (ctrl >> 10) & 63;
    if (!sem_wait_timeout(&x->cmd_sem, 3000)) { x->cmd_trb = 0; kprintf("xhci: %s timed out\n", cmd_name(type)); r = -ETIMEDOUT; }
    else if (x->cmd_cc != CC_SUCCESS) {
        if (type != TRB_STOP_EP && type != TRB_RESET_EP) kprintf("xhci: %s failed, completion code %d\n", cmd_name(type), x->cmd_cc);
        r = -EIO;
    }
    if (slot) *slot = x->cmd_slot;
    mutex_unlock(&x->cmd_lock);
    return r;
}
#define SLOT_EP(s, dci) (((uint32_t)(s) << 24) | ((uint32_t)(dci) << 16))

/* point the endpoint past everything queued, failing those transfers;
 * the endpoint must be stopped or halted-then-reset */
static void ring_skip(struct xhci *x, struct xslot *s, int dci, int status) {
    struct done_list dl = { NULL, &dl.head };
    uint64_t f = irq_save();
    spin_lock(&x->lock);
    struct ring *r = s->ring[dci];
    if (!r) { spin_unlock(&x->lock); irq_restore(f); return; }   /* given up: see ep_recover */
    uint64_t deq = r->phys + r->enq * sizeof(struct trb) + r->cycle;
    spin_unlock(&x->lock);
    irq_restore(f);
    xhci_cmd(x, deq, TYPE(TRB_SET_DEQ) | SLOT_EP(s->id, dci), NULL);
    f = irq_save();
    spin_lock(&x->lock);
    flush(r, status, &dl);
    spin_unlock(&x->lock);
    complete_all(&dl);
    irq_restore(f);
}

/* ---- transfers ---- */

/* TD Size (xHCI 4.11.2.4): packets of the TD still to come after this TRB */
static uint32_t td_size(struct xhci *x, uint32_t done, uint32_t total, uint32_t mps, bool last) {
    if (last) return 0;
    uint32_t v = x->version >= 0x100 ? (total + mps - 1) / mps - done / mps : (total - done) >> 10;
    return MIN(v, 31u) << 17;
}

static int xhci_submit(struct usb_xfer *xf) {
    struct usb_device *d = xf->dev;
    struct xhci *x = d->hcd->priv;
    bool control = xf->ep->type == USB_EP_CONTROL, in = usb_ep_in(xf);
    int dci = ep_dci(xf->ep);
    uint64_t phys = xf->len ? V2P(xf->data) : 0;
    bool isoc = xf->ep->type == USB_EP_ISOC;
    int nchunks = 0;
    if (isoc) {   /* one TD per packet, each split at 64 KiB boundaries */
        uint32_t off = 0;
        for (int p = 0; p < xf->npackets; p++) {
            uint32_t end = off + xf->packet_len[p];
            do { off += MIN(end - off, 0x10000 - ((phys + off) & 0xFFFF)); nchunks++; } while (off < end);
        }
        if (!xf->npackets || off != xf->len) return -EINVAL;
    } else for (uint32_t off = 0; off < xf->len; nchunks++) off += MIN(xf->len - off, 0x10000 - ((phys + off) & 0xFFFF));
    int need = control ? nchunks + 2 : MAX(nchunks, 1);
    uint32_t mps = xf->ep->mps ? xf->ep->mps : 8;

    spin_lock(&x->lock);
    struct xslot *s = d->hcpriv;
    struct ring *r = s && !d->gone ? s->ring[dci] : NULL;
    if (!r) { spin_unlock(&x->lock); return !s || d->gone ? -ENODEV : -EINVAL; }
    if (r->used + need > RING_TRBS - 8) { spin_unlock(&x->lock); return -EBUSY; }
    int first = r->enq, last = first;
    uint32_t first_cycle = r->cycle;
    xf->hc_first = first; xf->hc_ntrb = need;
    xf->actual = control ? xf->len : 0;
    bool hold = true;
    if (control) {
        const uint8_t *sp = xf->setup;
        uint32_t trt = xf->len ? (in ? 3 : 2) : 0;
        ring_push(r, sp[0] | sp[1] << 8 | sp[2] << 16 | (uint32_t)sp[3] << 24, sp[4] | sp[5] << 8 | sp[6] << 16 | (uint32_t)sp[7] << 24,
                  8, TYPE(TRB_SETUP) | TRB_IDT | trt << 16, xf, 0, true);
        hold = false;
    }
    uint32_t off = 0;
    if (isoc) {
        /* packets per TD and their bursts (xHCI 4.11.2.3); HS high-bandwidth endpoints keep mult in Max Burst */
        int mb = d->speed == USB_SPEED_SUPER ? xf->ep->burst : d->speed == USB_SPEED_HIGH ? xf->ep->mult : 0;
        for (int p = 0; p < xf->npackets; p++) {
            uint32_t plen = xf->packet_len[p], poff = 0, pk = MAX((plen + mps - 1) / mps, 1u);
            uint32_t tbc = (pk + mb) / (mb + 1) - 1, res = pk % (mb + 1), tlbpc = res ? res - 1 : mb;
            for (int k = 0; k == 0 || poff < plen; k++) {
                uint32_t c = plen ? MIN(plen - poff, 0x10000 - ((phys + off + poff) & 0xFFFF)) : 0;
                bool lastc = poff + c >= plen;
                uint32_t ctrl = TYPE(k == 0 ? TRB_ISOCH : TRB_NORMAL) | (in ? TRB_ISP : 0) | (lastc ? 0 : TRB_CH);
                if (k == 0) ctrl |= TRB_SIA | tbc << 7 | tlbpc << 16;
                if (lastc && p == xf->npackets - 1) ctrl |= TRB_IOC;
                uint64_t a = phys + off + poff;
                last = ring_push(r, (uint32_t)a, (uint32_t)(a >> 32), c | td_size(x, poff + c, plen, mps, lastc), ctrl, xf, off + poff, hold);
                hold = false;
                poff += c;
            }
            off += plen;
        }
    }
    for (int k = 0; !isoc && (k < nchunks || (!control && k == 0)); k++) {
        uint32_t c = xf->len ? MIN(xf->len - off, 0x10000 - ((phys + off) & 0xFFFF)) : 0;
        bool lastc = off + c >= xf->len;
        uint32_t ctrl = TYPE(control && k == 0 ? TRB_DATA : TRB_NORMAL) | (in ? TRB_ISP : 0);
        if (!lastc) ctrl |= TRB_CH;
        else if (!control) ctrl |= TRB_IOC;
        if (control && k == 0 && in) ctrl |= TRB_DIR_IN;
        last = ring_push(r, (uint32_t)(phys + off), (uint32_t)((phys + off) >> 32), c | td_size(x, off + c, xf->len, mps, lastc), ctrl, xf, off, hold);
        hold = false;
        off += c;
    }
    if (control) last = ring_push(r, 0, 0, 0, TYPE(TRB_STATUS) | TRB_IOC | ((xf->len && in) ? 0 : TRB_DIR_IN), xf, 0, false);
    xf->hc_last = last;
    r->used += need;
    xf->hcpriv = NULL;
    if (r->tail) r->tail->hcpriv = xf; else r->head = xf;
    r->tail = xf;
    wmb();
    r->trb[first].ctrl = (r->trb[first].ctrl & ~1u) | first_cycle;
    wmb();
    x->db[s->id] = dci;
    spin_unlock(&x->lock);
    return 0;
}

static void fill_ep(struct xhci *x, uint32_t *c, struct usb_device *d, struct usb_endpoint *ep, struct ring *r);
enum { EP_RUNNING = 1, EP_HALTED = 2 };
static int ep_state(struct xhci *x, struct xslot *s, int dci) { return octx(x, s, dci)[0] & 7; }

/* Bring the endpoint to Stopped and point it past everything queued. A halted
 * endpoint (stall, transaction error, babble) needs Reset Endpoint, which also
 * resets the host's data toggle; with toggle, a running one is stopped and then
 * dropped and re-added, the only other way to reset its toggle (4.6.8).
 * The caller holds ep_lock. */
static void ep_recover(struct xhci *x, struct usb_device *d, struct xslot *s, int dci, int status, bool toggle) {
    /* an earlier recovery gave this endpoint up (both Configure Endpoints failed): its ring is gone
     * and everything queued on it has already failed, so there is nothing to stop or skip */
    if (!s->ring[dci]) return;
    int st = ep_state(x, s, dci);
    if (st == EP_HALTED) xhci_cmd(x, 0, TYPE(TRB_RESET_EP) | SLOT_EP(s->id, dci), NULL);
    else if (st == EP_RUNNING) xhci_cmd(x, 0, TYPE(TRB_STOP_EP) | SLOT_EP(s->id, dci), NULL);
    if (toggle && st != EP_HALTED && dci > 1) {
        struct done_list dl = { NULL, &dl.head };
        uint64_t f = irq_save();
        spin_lock(&x->lock);
        struct ring *r = s->ring[dci];
        flush(r, status, &dl);
        spin_unlock(&x->lock);
        complete_all(&dl);
        irq_restore(f);
        struct usb_endpoint *ep = NULL;
        for (int i = 0; i < d->nintf && !ep; i++) for (int k = 0; k < d->intf[i].nep; k++) if (ep_dci(&d->intf[i].ep[k]) == dci) ep = &d->intf[i].ep[k];
        if (!ep) return;
        memset(s->in, 0, 33 * x->csz);
        uint32_t *ctl = ictx(x, s, 0), *sc = ictx(x, s, 1);
        memcpy(sc, octx(x, s, 0), x->csz);
        sc[3] = 0;
        ctl[0] = 1u << dci; ctl[1] = 1u | 1u << dci;   /* drop and add: a fresh endpoint at the current enqueue position */
        fill_ep(x, ictx(x, s, dci + 1), d, ep, r);
        if (xhci_cmd(x, s->in_phys, TYPE(TRB_CONFIG_EP) | SLOT_EP(s->id, 0), NULL) == 0) return;
        if (ep_state(x, s, dci)) { ring_skip(x, s, dci, status); return; }   /* refused, still stopped: skip what was queued */
        ctl[0] = 0;   /* dropped but not added back: add it alone */
        int rc = xhci_cmd(x, s->in_phys, TYPE(TRB_CONFIG_EP) | SLOT_EP(s->id, 0), NULL);
        if (rc == 0) return;
        f = irq_save();   /* give the endpoint up: submits fail instead of waiting forever */
        spin_lock(&x->lock);
        dl.head = NULL; dl.tail = &dl.head;
        flush(r, status, &dl);
        s->ring[dci] = NULL;
        spin_unlock(&x->lock);
        complete_all(&dl);
        irq_restore(f);
        if (rc == -EIO) ring_free(r);   /* after a timeout the controller may still read it */
        return;
    }
    ring_skip(x, s, dci, status);
}

static void xhci_abort(struct usb_xfer *xf) {
    struct xhci *x = xf->dev->hcd->priv;
    mutex_lock(&x->ep_lock);
    struct xslot *s = xf->dev->hcpriv;   /* NULL: free_device already failed the transfer */
    if (s) ep_recover(x, xf->dev, s, ep_dci(xf->ep), -ETIMEDOUT, false);
    mutex_unlock(&x->ep_lock);
}

static int xhci_reset_ep(struct usb_device *d, struct usb_endpoint *ep) {
    struct xhci *x = d->hcd->priv;
    int dci = ep_dci(ep), r = 0;
    mutex_lock(&x->ep_lock);
    struct xslot *s = d->hcpriv;
    if (!s || !s->ring[dci]) r = -ENODEV;
    else ep_recover(x, d, s, dci, -EIO, true);
    mutex_unlock(&x->ep_lock);
    return r;
}

/* ---- devices ---- */

static void fill_ep(struct xhci *x, uint32_t *c, struct usb_device *d, struct usb_endpoint *ep, struct ring *r) {
    bool in = ep->addr & USB_DIR_IN, periodic = ep->type == USB_EP_INTR || ep->type == USB_EP_ISOC;
    int type = ep->type == USB_EP_CONTROL ? 4 : ep->type + (in ? 4 : 0);
    int interval = 0;
    if (periodic) {
        if (d->speed == USB_SPEED_HIGH || d->speed == USB_SPEED_SUPER || ep->type == USB_EP_ISOC) interval = CLAMP(ep->interval, 1, 16) - 1 + (d->speed <= USB_SPEED_LOW ? 3 : 0);
        else interval = CLAMP(31 - __builtin_clz(MAX(ep->interval, 1) * 8), 3, 10);   /* frames -> 2^n microframes */
    }
    int burst = d->speed == USB_SPEED_SUPER ? ep->burst : d->speed == USB_SPEED_HIGH && periodic ? ep->mult : 0;
    uint32_t esit = periodic ? ep->mps * (burst + 1) : 0;
    memset(c, 0, x->csz);
    c[0] = interval << 16 | (esit >> 16) << 24;
    c[1] = (ep->type == USB_EP_ISOC ? 0 : 3) << 1 | type << 3 | burst << 8 | (uint32_t)ep->mps << 16;
    c[2] = (uint32_t)(r->phys + r->enq * sizeof(struct trb)) | r->cycle;
    c[3] = (uint32_t)(r->phys >> 32);
    c[4] = (ep->type == USB_EP_CONTROL ? 8 : periodic ? esit : 3072) | (esit & 0xFFFF) << 16;
}

static void slot_free(struct xhci *x, struct xslot *s) {
    for (int i = 0; i < 32; i++) ring_free(s->ring[i]);
    if (s->in_phys) pmm_free_page(s->in_phys);
    if (s->out_phys) pmm_free_page(s->out_phys);
    kfree(s);
}

static int xhci_enable_device(struct usb_device *d) {
    struct xhci *x = d->hcd->priv;
    int id = 0;
    if (xhci_cmd(x, 0, TYPE(TRB_ENABLE_SLOT), &id) < 0 || id <= 0 || id > x->maxslots) return -EIO;
    struct xslot *s = kzalloc(sizeof *s);
    s->id = id;
    s->in_phys = xalloc(x, 1); s->out_phys = xalloc(x, 1); s->ring[1] = ring_new(x);
    if (!s->in_phys || !s->out_phys || !s->ring[1]) { slot_free(x, s); xhci_cmd(x, 0, TYPE(TRB_DISABLE_SLOT) | SLOT_EP(id, 0), NULL); return -ENOMEM; }
    s->in = P2V(s->in_phys); s->out = P2V(s->out_phys);
    spin_lock(&x->lock);
    x->dcbaa[id] = s->out_phys; x->slots[id] = s; d->hcpriv = s;
    spin_unlock(&x->lock);

    uint32_t *ctl = ictx(x, s, 0), *sc = ictx(x, s, 1);
    ctl[1] = 3;   /* add slot + ep0 */
    sc[0] = (d->route & 0xFFFFF) | (uint32_t)d->speed << 20 | 1u << 27;
    sc[1] = (uint32_t)d->root_port << 16;
    if (d->tt_hub && d->tt_hub->hcpriv) sc[2] = ((struct xslot *)d->tt_hub->hcpriv)->id | d->tt_port << 8;
    fill_ep(x, ictx(x, s, 2), d, &d->ep0, s->ring[1]);
    if (xhci_cmd(x, s->in_phys, TYPE(TRB_ADDRESS_DEV) | SLOT_EP(id, 0), NULL) < 0) return -EIO;
    d->addr = octx(x, s, 0)[3] & 0xFF;

    if (d->speed == USB_SPEED_FULL) {   /* ep0 packet size is 8..64: read it, then tell the controller */
        uint8_t buf[8];
        if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, buf, 8) < 8) return -EIO;
        if (buf[7] != d->ep0.mps && (buf[7] == 16 || buf[7] == 32 || buf[7] == 64)) {
            d->ep0.mps = buf[7];
            memset(s->in, 0, 33 * x->csz);
            ctl[1] = 2;
            fill_ep(x, ictx(x, s, 2), d, &d->ep0, s->ring[1]);
            if (xhci_cmd(x, s->in_phys, TYPE(TRB_EVAL_CTX) | SLOT_EP(id, 0), NULL) < 0) return -EIO;
        }
    }
    return 0;
}

/* Configure Endpoint: with eps, adds every endpoint of the interfaces;
 * without, only rewrites the slot context (hub fields) */
static int config_slot(struct usb_device *d, bool eps) {
    struct xhci *x = d->hcd->priv;
    struct xslot *s = d->hcpriv;
    if (!s) return -ENODEV;
    memset(s->in, 0, 33 * x->csz);
    uint32_t *ctl = ictx(x, s, 0), *sc = ictx(x, s, 1);
    memcpy(sc, octx(x, s, 0), x->csz);
    sc[3] = 0;
    ctl[1] = 1;
    int maxdci = (sc[0] >> 27) & 31;
    for (int i = 0; eps && i < d->nintf; i++) for (int k = 0; k < d->intf[i].nep; k++) {
        struct usb_endpoint *ep = &d->intf[i].ep[k];
        int dci = ep_dci(ep);
        if (!s->ring[dci] && !(s->ring[dci] = ring_new(x))) return -ENOMEM;
        fill_ep(x, ictx(x, s, dci + 1), d, ep, s->ring[dci]);
        ctl[1] |= 1u << dci;
        maxdci = MAX(maxdci, dci);
    }
    sc[0] = (sc[0] & ~(31u << 27)) | (uint32_t)maxdci << 27;
    if (d->hub_ports) {
        sc[0] |= 1u << 26;
        sc[1] = (sc[1] & 0x00FFFFFF) | (uint32_t)d->hub_ports << 24;
        if (d->speed == USB_SPEED_HIGH) sc[2] = (sc[2] & ~(3u << 16)) | (uint32_t)d->hub_ttt << 16;
    }
    return xhci_cmd(x, s->in_phys, TYPE(TRB_CONFIG_EP) | SLOT_EP(s->id, 0), NULL);
}
static int xhci_configure(struct usb_device *d) { return config_slot(d, true); }
static int xhci_hub_config(struct usb_device *d) { return config_slot(d, false); }

/* one Configure Endpoint: drop the previous setting's endpoints, add the new ones */
static int xhci_set_interface(struct usb_device *d, struct usb_interface *in, const struct usb_endpoint *old, int nold) {
    struct xhci *x = d->hcd->priv;
    mutex_lock(&x->ep_lock);
    struct xslot *s = d->hcpriv;
    if (!s) { mutex_unlock(&x->ep_lock); return -ENODEV; }
    memset(s->in, 0, 33 * x->csz);
    uint32_t *ctl = ictx(x, s, 0), *sc = ictx(x, s, 1);
    memcpy(sc, octx(x, s, 0), x->csz);
    sc[3] = 0;
    ctl[1] = 1;
    struct done_list dl = { NULL, &dl.head };
    uint64_t f = irq_save();
    spin_lock(&x->lock);
    for (int k = 0; k < nold; k++) {
        int dci = ep_dci(&old[k]);
        if (ep_state(x, s, dci)) ctl[0] |= 1u << dci;
        if (s->ring[dci]) flush(s->ring[dci], -EIO, &dl);
    }
    spin_unlock(&x->lock);
    complete_all(&dl);
    irq_restore(f);
    int r = 0, maxdci = 1;
    for (int k = 0; k < in->nep && r == 0; k++) {
        int dci = ep_dci(&in->ep[k]);
        if (!s->ring[dci] && !(s->ring[dci] = ring_new(x))) { r = -ENOMEM; break; }
        fill_ep(x, ictx(x, s, dci + 1), d, &in->ep[k], s->ring[dci]);
        ctl[1] |= 1u << dci;
    }
    for (int i = 0; i < d->nintf; i++) for (int k = 0; k < d->intf[i].nep; k++) maxdci = MAX(maxdci, ep_dci(&d->intf[i].ep[k]));
    sc[0] = (sc[0] & ~(31u << 27)) | (uint32_t)maxdci << 27;
    if (r == 0) r = xhci_cmd(x, s->in_phys, TYPE(TRB_CONFIG_EP) | SLOT_EP(s->id, 0), NULL);
    mutex_unlock(&x->ep_lock);
    return r;
}

/* Disable Slot first: buffers of the transfers failed here must not be
 * written by the controller once their owners free them */
static void xhci_free_device(struct usb_device *d) {
    struct xhci *x = d->hcd->priv;
    mutex_lock(&x->ep_lock);
    spin_lock(&x->lock);
    struct xslot *s = d->hcpriv;
    d->hcpriv = NULL;   /* submit fails from here on */
    spin_unlock(&x->lock);
    if (!s) { mutex_unlock(&x->ep_lock); return; }
    int r = xhci_cmd(x, 0, TYPE(TRB_DISABLE_SLOT) | SLOT_EP(s->id, 0), NULL);
    struct done_list dl = { NULL, &dl.head };
    uint64_t f = irq_save();
    spin_lock(&x->lock);
    for (int i = 0; i < 32; i++) if (s->ring[i]) flush(s->ring[i], -ENODEV, &dl);
    x->slots[s->id] = NULL;
    spin_unlock(&x->lock);
    complete_all(&dl);
    irq_restore(f);
    x->dcbaa[s->id] = 0;
    if (r == 0) slot_free(x, s);   /* if the controller did not answer, it may still write there */
    mutex_unlock(&x->ep_lock);
}

/* ---- root hub ---- */

static void xhci_port_status(struct usb_hcd *h, int port, struct usb_port_status *st) {
    struct xhci *x = h->priv;
    uint32_t v = rd(x->op, PORTSC(port));
    int spd = (v >> 10) & 15;
    st->connected = v & PORT_CCS; st->enabled = v & PORT_PED; st->changed = v & PORT_CSC;
    st->speed = spd >= 4 ? USB_SPEED_SUPER : spd ? spd : USB_SPEED_FULL;
    if (v & PORT_CHANGES) wr(x->op, PORTSC(port), (v & PORT_KEEP) | (v & PORT_CHANGES));
}

static int xhci_port_reset(struct usb_hcd *h, int port) {
    struct xhci *x = h->priv;
    uint32_t v = rd(x->op, PORTSC(port));
    if (!(v & PORT_CCS)) return -ENODEV;
    bool usb3 = x->port_major[port] == 3;
    if (usb3) {   /* USB 3 ports train the link and enable themselves */
        for (int i = 0; i < 50 && !(rd(x->op, PORTSC(port)) & PORT_PED); i++) thread_sleep_ms(10);
        if (rd(x->op, PORTSC(port)) & PORT_PED) return 0;
        wr(x->op, PORTSC(port), (rd(x->op, PORTSC(port)) & PORT_KEEP) | PORT_WPR);
    } else wr(x->op, PORTSC(port), (v & PORT_KEEP) | PORT_PR);
    for (int i = 0; i < 50; i++) {
        thread_sleep_ms(10);
        v = rd(x->op, PORTSC(port));
        if ((v & (PORT_PRC | PORT_WRC)) && !(v & PORT_PR)) break;
    }
    wr(x->op, PORTSC(port), (v & PORT_KEEP) | (v & (PORT_PRC | PORT_WRC)));
    return (rd(x->op, PORTSC(port)) & PORT_PED) ? 0 : -EIO;
}

static const struct usb_hcd_ops xhci_ops = {
    .port_status = xhci_port_status, .port_reset = xhci_port_reset,
    .enable_device = xhci_enable_device, .configure = xhci_configure, .hub_config = xhci_hub_config, .set_interface = xhci_set_interface, .free_device = xhci_free_device,
    .submit = xhci_submit, .abort = xhci_abort, .reset_ep = xhci_reset_ep,
};

/* ---- bring-up ---- */

/* extended capabilities: take the controller from the BIOS (which otherwise keeps
 * emulating a PS/2 keyboard through SMIs) and learn which ports are USB 2 or 3 */
static void ext_caps(struct xhci *x, size_t mapped) {
    uint32_t off = (rd(x->cap, 0x10) >> 16) << 2;
    for (int guard = 0; off && off + 12 <= mapped && guard < 64; guard++) {
        volatile uint8_t *p = x->cap + off;
        uint32_t v = rd(p, 0);
        if ((v & 0xFF) == 1) {
            if (v & (1 << 16)) {
                wr(p, 0, v | (1 << 24));   /* OS owned semaphore */
                for (int i = 0; i < 100 && (rd(p, 0) & (1 << 16)); i++) thread_sleep_ms(10);
                if (rd(p, 0) & (1 << 16)) { kprintf("xhci: BIOS did not release the controller, taking it\n"); wr(p, 0, (rd(p, 0) & ~(1u << 16)) | (1 << 24)); }
                else kprintf("xhci: took ownership from the BIOS\n");
            }
            wr(p, 4, (rd(p, 4) & ~((1u << 0) | (1u << 4) | (7u << 13))) | (7u << 29));   /* SMIs off, clear their status */
        } else if ((v & 0xFF) == 2) {
            uint32_t d2 = rd(p, 8);
            int first = d2 & 0xFF, count = (d2 >> 8) & 0xFF;
            for (int i = first; i < first + count && i < 256; i++) x->port_major[i] = v >> 24;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

/* Intel 7/8/9-series chipsets route their ports to the EHCI controllers until told otherwise */
static void intel_port_switch(struct pci_device *pd) {
    static const uint16_t ids[] = { 0x1E31, 0x8C31, 0x9C31, 0x8CB1, 0x9CB1 };
    bool match = false;
    for (size_t i = 0; i < ARRAY_LEN(ids); i++) if (pd->vendor == 0x8086 && pd->device == ids[i]) match = true;
    if (!match) return;
    pci_write32(pd->bus, pd->slot, pd->func, 0xD8, pci_read32(pd->bus, pd->slot, pd->func, 0xDC));   /* USB3_PSSEN <- USB3PRM */
    pci_write32(pd->bus, pd->slot, pd->func, 0xD0, pci_read32(pd->bus, pd->slot, pd->func, 0xD4));   /* XUSB2PR <- XUSB2PRM */
    kprintf("xhci: switched the chipset's USB ports from EHCI to xHCI\n");
}

static bool wait_reg(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t want, int ms) {
    for (int i = 0; i < ms; i++) { if ((rd(b, off) & mask) == want) return true; thread_sleep_ms(1); }
    return (rd(b, off) & mask) == want;
}

static void xhci_probe(struct pci_device *pd) {
    if (!pd->bar_addr[0] || !pd->bar_is_mmio[0]) return;
    pci_enable_bus_master(pd);
    struct xhci *x = kzalloc(sizeof *x);
    x->cap = vmm_map_mmio(pd->bar_addr[0], 0x1000);
    uint32_t caplen = rd(x->cap, 0) & 0xFF, hcs1 = rd(x->cap, 4), hcs2 = rd(x->cap, 8), hcc1 = rd(x->cap, 0x10);
    uint32_t dboff = rd(x->cap, 0x14) & ~3u, rtsoff = rd(x->cap, 0x18) & ~0x1Fu;
    x->version = rd(x->cap, 0) >> 16;
    x->maxslots = hcs1 & 0xFF; x->maxports = hcs1 >> 24;
    size_t size = MAX(MAX((size_t)0x10000, (size_t)dboff + 4 * 256), MAX((size_t)rtsoff + 0x40, (size_t)caplen + PORTSC(x->maxports) + 0x10));
    x->cap = vmm_map_mmio(pd->bar_addr[0], size);
    x->op = x->cap + caplen; x->ir = x->cap + rtsoff + 0x20; x->db = (volatile uint32_t *)(x->cap + dboff);
    x->csz = (hcc1 & 4) ? 64 : 32;
    x->hcd.dma32 = !(hcc1 & 1);
    kprintf("xhci: controller %04x:%04x at %lx, version %x.%02x, %d slots, %d ports%s\n", pd->vendor, pd->device, pd->bar_addr[0],
            x->version >> 8, x->version & 0xFF, x->maxslots, x->maxports, x->hcd.dma32 ? ", 32-bit DMA" : "");
    ext_caps(x, size);
    intel_port_switch(pd);

    wr(x->op, USBCMD, rd(x->op, USBCMD) & ~(CMD_RS | CMD_INTE));
    if (!wait_reg(x->op, USBSTS, STS_HCH, STS_HCH, 50)) kprintf("xhci: controller did not halt\n");
    wr(x->op, USBCMD, CMD_HCRST);
    thread_sleep_ms(1);
    if (!wait_reg(x->op, USBCMD, CMD_HCRST, 0, 1000) || !wait_reg(x->op, USBSTS, STS_CNR, 0, 1000)) { kprintf("xhci: reset timed out\n"); return; }
    if (!(rd(x->op, PAGESIZE) & 1)) { kprintf("xhci: 4 KiB pages not supported\n"); return; }

    wr(x->op, CONFIG, x->maxslots);
    uint64_t dcbaa = xalloc(x, 1);
    if (!dcbaa) return;
    x->dcbaa = P2V(dcbaa);
    int nscratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nscratch) {
        uint64_t arr = xalloc(x, PAGE_ALIGN_UP(nscratch * 8) / PAGE_SIZE);
        if (!arr) return;
        uint64_t *a = P2V(arr);
        for (int i = 0; i < nscratch; i++) if (!(a[i] = xalloc(x, 1))) return;
        x->dcbaa[0] = arr;
    }
    wr64(x->op, DCBAAP, dcbaa);
    if (!(x->cmd = ring_new(x))) return;
    wr64(x->op, CRCR, x->cmd->phys | 1);
    uint64_t erst = xalloc(x, 1);
    if (!erst || !(x->ev_phys = xalloc(x, 1))) return;
    x->ev = P2V(x->ev_phys); x->ev_cycle = 1;
    uint64_t *e = P2V(erst);
    e[0] = x->ev_phys; e[1] = EV_TRBS;
    wr(x->ir, ERSTSZ, 1);
    wr64(x->ir, ERDP, x->ev_phys);
    wr64(x->ir, ERSTBA, erst);
    wr(x->ir, IMOD, 160);   /* at most one interrupt per 40 us */
    wr(x->ir, IMAN, 3);
    mutex_init(&x->cmd_lock);
    mutex_init(&x->ep_lock);
    sem_init(&x->cmd_sem, 0);

    char how[64];
    if (!pci_irq_install(pd, xhci_irq, x, true, how, sizeof how)) kprintf("xhci: no interrupt (%s), polling\n", how);
    else kprintf("xhci: interrupt %s\n", how);
    wr(x->op, USBCMD, CMD_RS | CMD_INTE);
    if (!wait_reg(x->op, USBSTS, STS_HCH, 0, 100)) { kprintf("xhci: controller did not start\n"); return; }
    if (hcc1 & (1 << 3))   /* port power control */
        for (int p = 1; p <= x->maxports; p++) wr(x->op, PORTSC(p), (rd(x->op, PORTSC(p)) & PORT_KEEP) | PORT_PP);
    thread_sleep_ms(20);

    x->hcd.name = "xHCI"; x->hcd.ops = &xhci_ops; x->hcd.priv = x; x->hcd.pci = pd; x->hcd.nports = x->maxports;
    thread_create(kernel_process, "xhci", poll_thread, x);
    usb_register_hcd(&x->hcd);
}

void xhci_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->class_code == 0x0C && pd->subclass == 0x03 && pd->prog_if == 0x30) xhci_probe(pd);
    }
}
