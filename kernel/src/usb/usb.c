/* USB core: transfers, enumeration, the hub class driver and the hub thread
 * that watches every root and external hub port for plugs and unplugs. */
#include "usb.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "input.h"

struct usb_hub {
    struct usb_hcd *hcd;
    struct usb_device *dev;        /* NULL for a root hub */
    int nports;
    bool usb3;
    struct usb_device **child;
    uint8_t *failed;               /* enumeration failed: leave the port alone until it changes */
    struct usb_xfer irq;           /* status change endpoint */
    uint8_t *irqbuf;
    volatile bool changed;
    int idle;                      /* hub thread passes since the ports were last read */
    struct usb_hub *next;
};

static const struct usb_driver usb_hub_driver;
static const struct usb_driver *drivers[] = { &usb_hub_driver, &usb_hid_driver, &usb_msc_driver, &usb_audio_driver };
static struct usb_hcd *hcds[8];
static int nhcds;
static struct usb_hub *hubs;
static struct mutex usb_lock = MUTEX_INIT;   /* device tree: taken by the hub thread and usb_print */
static struct semaphore hub_sem;
static bool started;

const char *usb_speed_name(int s) {
    switch (s) { case USB_SPEED_LOW: return "low-speed"; case USB_SPEED_FULL: return "full-speed"; case USB_SPEED_HIGH: return "high-speed"; case USB_SPEED_SUPER: return "SuperSpeed"; default: return "?"; }
}

/* ---- transfers ---- */

bool usb_ep_in(const struct usb_xfer *x) { return x->ep->type == USB_EP_CONTROL ? (x->setup[0] & USB_DIR_IN) != 0 : (x->ep->addr & USB_DIR_IN) != 0; }

/* Controllers get HHDM memory (physically contiguous), below 4 GiB if they need
 * it; anything else, like a buffer in the kernel image, goes through a bounce. */
static bool dma_ok(const void *p, uint32_t len, bool dma32) {
    uint64_t v = (uint64_t)p;
    if (v < hhdm_offset || v >= KERNEL_VIRT_BASE) return false;
    return !dma32 || V2P(p) + len <= (4ULL << 30);
}

int usb_submit(struct usb_xfer *x) {
    struct usb_hcd *h = x->dev->hcd;
    if (x->dev->gone) return x->status = -ENODEV;
    x->status = USB_PENDING; x->actual = 0; x->data = x->buf; x->bounce = 0;
    if (x->len && !dma_ok(x->buf, x->len, h->dma32)) {
        x->bounce_pages = PAGE_ALIGN_UP(x->len) / PAGE_SIZE;
        x->bounce = h->dma32 ? pmm_alloc_pages_low(x->bounce_pages) : pmm_alloc_pages(x->bounce_pages);
        if (!x->bounce) return x->status = -ENOMEM;
        x->data = P2V(x->bounce);
        if (!usb_ep_in(x)) memcpy(x->data, x->buf, x->len);
    }
    int r = h->ops->submit(x);
    if (r < 0) {
        if (x->bounce) pmm_free_pages(x->bounce, x->bounce_pages);
        x->bounce = 0; x->status = r;
    }
    return r;
}

void usb_complete(struct usb_xfer *x, int status, uint32_t actual) {
    if (actual > x->len) actual = x->len;
    if (x->bounce) {
        if (usb_ep_in(x) && actual) memcpy(x->buf, x->data, actual);
        pmm_free_pages(x->bounce, x->bounce_pages);
        x->bounce = 0;
    }
    x->actual = actual;
    x->status = status;
    if (x->done) x->done(x); else sem_post(&x->sem);
}

static int usb_wait(struct usb_xfer *x, int timeout_ms) {
    if (!sem_wait_timeout(&x->sem, timeout_ms)) {
        x->dev->hcd->ops->abort(x);   /* completes it, unless it finished meanwhile */
        sem_wait(&x->sem);
    }
    return x->status;
}

int usb_control(struct usb_device *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len) {
    struct usb_xfer x = { 0 };
    x.dev = d; x.ep = &d->ep0; x.buf = data; x.len = len;
    x.setup[0] = type; x.setup[1] = req;
    x.setup[2] = value; x.setup[3] = value >> 8; x.setup[4] = index; x.setup[5] = index >> 8;
    x.setup[6] = len; x.setup[7] = len >> 8;
    sem_init(&x.sem, 0);
    mutex_lock(&d->ctl_lock);
    int r = usb_submit(&x);
    if (r == 0) r = usb_wait(&x, 5000);
    if (r < 0 && r != -ENODEV && !d->gone) d->hcd->ops->reset_ep(d, &d->ep0);   /* a stall or error halts ep0 on xHCI */
    mutex_unlock(&d->ctl_lock);
    return r < 0 ? r : (int)x.actual;
}

int usb_clear_halt(struct usb_device *d, struct usb_endpoint *ep) {
    if (d->gone) return -ENODEV;
    d->hcd->ops->reset_ep(d, ep);
    int r = usb_control(d, USB_RECIP_EP, USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */, ep->addr, NULL, 0);
    return r < 0 ? r : 0;
}

int usb_transfer(struct usb_device *d, struct usb_endpoint *ep, void *buf, uint32_t len, int timeout_ms) {
    struct usb_xfer x = { 0 };
    x.dev = d; x.ep = ep; x.buf = buf; x.len = len;
    sem_init(&x.sem, 0);
    int r = usb_submit(&x);
    if (r == 0) r = usb_wait(&x, timeout_ms);
    if (r == -EPIPE || r == -EIO) usb_clear_halt(d, ep);   /* stalls and transaction errors halt the endpoint */
    return r < 0 ? r : (int)x.actual;
}

/* buffers for interrupt endpoints, which are resubmitted from interrupt
 * context: below 4 GiB for controllers that need it, so they never bounce */
void *usb_buf_alloc(struct usb_device *d, size_t n) {
    if (!d->hcd->dma32) return kzalloc(n);
    uint64_t p = n <= PAGE_SIZE ? pmm_alloc_pages_low(1) : 0;
    return p ? P2V(p) : kzalloc(n);
}
void usb_buf_free(struct usb_device *d, void *p) {
    if (!p) return;
    if (d->hcd->dma32 && ((uint64_t)p & (PAGE_SIZE - 1)) == 0) pmm_free_page(V2P(p));
    else kfree(p);
}

static void parse_ep(struct usb_endpoint *ep, const uint8_t *p) {
    const struct usb_endpoint_desc *ed = (const void *)p;
    memset(ep, 0, sizeof *ep);
    ep->addr = ed->bEndpointAddress; ep->type = ed->bmAttributes & 3;
    ep->mps = ed->wMaxPacketSize & 0x7FF; ep->mult = (ed->wMaxPacketSize >> 11) & 3;
    ep->interval = ed->bInterval;
}

struct usb_endpoint *usb_ep_by_addr(struct usb_interface *i, uint8_t addr) {
    for (int k = 0; k < i->nep; k++) if (i->ep[k].addr == addr) return &i->ep[k];
    return NULL;
}

/* Switch an interface to another alternate setting: the controller first
 * (it may refuse for bandwidth), then the device. No transfers may be queued
 * on the interface's endpoints. */
int usb_set_interface(struct usb_interface *i, int alt) {
    struct usb_device *d = i->dev;
    if (!d->hcd->ops->set_interface) return -ENOTSUP;
    struct usb_endpoint eps[USB_MAX_EP], *last = NULL;
    int n = 0, found = 0;
    for (int off = 0; off + 2 <= d->config_len; ) {
        const uint8_t *p = d->config + off;
        if (p[0] < 2 || off + p[0] > d->config_len) break;
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            if (found == 1) break;
            if (p[2] == i->num && p[3] == alt) found = 1;
        } else if (found && p[1] == USB_DT_ENDPOINT && p[0] >= 7 && n < USB_MAX_EP) parse_ep(last = &eps[n++], p);
        else if (found && p[1] == USB_DT_SS_EP_COMP && p[0] >= 6 && last) last->burst = p[2];
        off += p[0];
    }
    if (!found) return -EINVAL;
    struct usb_endpoint old[USB_MAX_EP];
    int nold = i->nep;
    memcpy(old, i->ep, sizeof old);
    memcpy(i->ep, eps, sizeof eps);
    i->nep = n;
    int r = d->hcd->ops->set_interface(d, i, old, nold);
    if (r == 0) r = usb_control(d, USB_RECIP_INTF, USB_REQ_SET_INTERFACE, alt, i->num, NULL, 0);
    if (r == -EPIPE && alt == 0) r = 0;   /* devices with a single setting may stall the request */
    if (r < 0) {
        memcpy(i->ep, old, sizeof old); i->nep = nold;
        d->hcd->ops->set_interface(d, i, eps, n);
        return r;
    }
    i->alt = alt;
    return 0;
}

struct usb_endpoint *usb_find_ep(struct usb_interface *i, int type, bool in) {
    for (int k = 0; k < i->nep; k++) if (i->ep[k].type == type && ((i->ep[k].addr & USB_DIR_IN) != 0) == in) return &i->ep[k];
    return NULL;
}
const uint8_t *usb_find_desc(struct usb_interface *i, uint8_t type) {
    for (int off = 0; off + 2 <= i->extra_len && i->extra[off] >= 2 && off + i->extra[off] <= i->extra_len; off += i->extra[off])
        if (i->extra[off + 1] == type) return i->extra + off;
    return NULL;
}

/* ---- enumeration ---- */

static void get_string(struct usb_device *d, int idx, uint16_t lang, char *out, size_t n) {
    out[0] = 0;
    if (!idx) return;
    uint8_t *buf = kmalloc(256);
    int r = usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) | idx, lang, buf, 255);
    if (r >= 2 && buf[1] == USB_DT_STRING) {
        int len = MIN(r, buf[0]); size_t o = 0;
        for (int i = 2; i + 1 < len && o + 1 < n; i += 2) {
            uint16_t c = buf[i] | (buf[i + 1] << 8);
            out[o++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
        }
        while (o && out[o - 1] == ' ') o--;
        out[o] = 0;
    }
    kfree(buf);
}

/* interfaces with alternate setting 0 and their endpoints */
static void parse_config(struct usb_device *d) {
    struct usb_interface *cur = NULL;
    struct usb_endpoint *last_ep = NULL;
    for (int off = 0; off + 2 <= d->config_len; ) {
        const uint8_t *p = d->config + off;
        if (p[0] < 2 || off + p[0] > d->config_len) break;
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            const struct usb_interface_desc *id = (const void *)p;
            if (cur) cur->extra_len = (int)(p - cur->extra);
            cur = NULL; last_ep = NULL;
            if (id->bAlternateSetting == 0 && d->nintf < USB_MAX_INTF) {
                cur = &d->intf[d->nintf++];
                cur->dev = d; cur->num = id->bInterfaceNumber;
                cur->cls = id->bInterfaceClass; cur->subcls = id->bInterfaceSubClass; cur->proto = id->bInterfaceProtocol;
                cur->extra = p + p[0];
            }
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && cur) {
            const struct usb_endpoint_desc *ed = (const void *)p;
            if (cur->nep < USB_MAX_EP) parse_ep(last_ep = &cur->ep[cur->nep++], (const uint8_t *)ed);
        } else if (p[1] == USB_DT_SS_EP_COMP && p[0] >= 6 && last_ep) {
            last_ep->burst = p[2];
        }
        off += p[0];
    }
    if (cur) cur->extra_len = (int)(d->config + d->config_len - cur->extra);
}

static const char *class_name(uint8_t c) {
    switch (c) {
    case 1: return "audio"; case 2: return "communications"; case 3: return "HID"; case 6: return "imaging"; case 7: return "printer";
    case 8: return "mass storage"; case 9: return "hub"; case 0x0A: return "CDC data"; case 0x0B: return "smart card"; case 0x0E: return "video";
    case 0xE0: return "wireless"; case 0xFF: return "vendor specific"; default: return "other";
    }
}

static struct usb_device *enumerate(struct usb_hub *hub, int port, int speed) {
    struct usb_device *d = kzalloc(sizeof *d);
    struct usb_device *up = hub->dev;
    d->hcd = hub->hcd; d->parent = hub; d->port = port; d->speed = speed;
    mutex_init(&d->ctl_lock);
    if (up) {
        d->root_port = up->root_port; d->depth = up->depth + 1;
        d->route = up->route | ((uint32_t)MIN(port, 15) << (4 * up->depth));
        if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
            if (up->speed == USB_SPEED_HIGH) { d->tt_hub = up; d->tt_port = port; }
            else { d->tt_hub = up->tt_hub; d->tt_port = up->tt_port; }
        }
    } else d->root_port = port;
    d->ep0.type = USB_EP_CONTROL;
    d->ep0.mps = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;

    const char *step = "addressing";
    if (d->hcd->ops->enable_device(d) < 0) goto fail;
    step = "reading the device descriptor";
    if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &d->desc, 18) < 18) goto fail;
    step = "reading the configuration";
    struct usb_config_desc cd;
    if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &cd, 9) < 9) goto fail;
    d->config_len = CLAMP(cd.wTotalLength, 9, 4096);
    d->config = kmalloc(d->config_len);
    if ((d->config_len = usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, d->config, d->config_len)) < 9) goto fail;
    uint16_t langs[2] = { 0 };
    if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, langs, 4) >= 4) {
        get_string(d, d->desc.iManufacturer, langs[1], d->manufacturer, sizeof d->manufacturer);
        get_string(d, d->desc.iProduct, langs[1], d->product, sizeof d->product);
        get_string(d, d->desc.iSerialNumber, langs[1], d->serial, sizeof d->serial);
    }
    parse_config(d);
    step = "configuring";
    if (d->hcd->ops->configure(d) < 0) goto fail;   /* the controller first, so a bandwidth failure leaves the device alone */
    if (usb_control(d, 0, USB_REQ_SET_CONFIGURATION, d->config[5], 0, NULL, 0) < 0) goto fail;

    kprintf("usb: %d-%d%s: %04x:%04x %s%s%s (%s, address %d, %d interface%s)\n", d->hcd->index, d->root_port, d->depth ? "+" : "",
            d->desc.idVendor, d->desc.idProduct, d->manufacturer, d->manufacturer[0] ? " " : "", d->product[0] ? d->product : "device",
            usb_speed_name(speed), d->addr, d->nintf, d->nintf == 1 ? "" : "s");
    for (int i = 0; i < d->nintf; i++) {
        struct usb_interface *in = &d->intf[i];
        for (size_t k = 0; k < ARRAY_LEN(drivers) && !in->driver; k++)
            if (drivers[k]->probe(in)) in->driver = drivers[k];
        if (!in->driver) kprintf("usb:   interface %d: %s (class %02x:%02x:%02x), no driver\n", in->num, class_name(in->cls), in->cls, in->subcls, in->proto);
    }
    return d;
fail:
    kprintf("usb: %d-%d: device on port %d failed while %s\n", d->hcd->index, d->root_port, port, step);
    d->gone = true;
    d->hcd->ops->free_device(d);
    kfree(d->config); kfree(d);
    return NULL;
}

static void disconnect(struct usb_device *d) {
    d->gone = true;
    struct usb_hub *h = d->hub;
    if (h) {
        for (int p = 0; p < h->nports; p++) if (h->child[p]) { disconnect(h->child[p]); h->child[p] = NULL; }
        for (struct usb_hub **pp = &hubs; *pp; pp = &(*pp)->next) if (*pp == h) { *pp = h->next; break; }
    }
    d->hcd->ops->free_device(d);    /* pending transfers complete with -ENODEV */
    for (int i = 0; i < d->nintf; i++) if (d->intf[i].driver && d->intf[i].driver->disconnect) d->intf[i].driver->disconnect(&d->intf[i]);
    kprintf("usb: %d-%d%s: %s disconnected\n", d->hcd->index, d->root_port, d->depth ? "+" : "", d->product[0] ? d->product : "device");
    if (h) { kfree(h->child); kfree(h->failed); usb_buf_free(d, h->irqbuf); kfree(h); }
    kfree(d->config); kfree(d);
}

/* ---- hub ports (root hubs through the controller, others by class requests) ---- */

#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16

/* false when an external hub did not answer */
static bool port_status(struct usb_hub *h, int port, struct usb_port_status *st) {
    memset(st, 0, sizeof *st);
    if (!h->dev) { h->hcd->ops->port_status(h->hcd, port, st); return true; }
    uint16_t s[2];
    if (usb_control(h->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0, port, s, 4) < 4) return false;
    st->connected = s[0] & 1; st->enabled = s[0] & 2; st->changed = s[1] & 1;
    st->speed = h->usb3 ? USB_SPEED_SUPER : (s[0] & 0x200) ? USB_SPEED_LOW : (s[0] & 0x400) ? USB_SPEED_HIGH : USB_SPEED_FULL;
    /* change bits -> C_PORT_* features: connection, enable, suspend, over-current, reset; USB 3 adds BH reset, link state, config error */
    static const uint8_t feat[8] = { 16, 17, 18, 19, 20, 29, 25, 26 };
    for (int b = 0; b < 8; b++) if (s[1] & (1 << b)) usb_control(h->dev, USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_CLEAR_FEATURE, feat[b], port, NULL, 0);
    return true;
}

static int port_reset(struct usb_hub *h, int port, struct usb_port_status *st) {
    if (!h->dev) {
        int r = h->hcd->ops->port_reset(h->hcd, port);
        h->hcd->ops->port_status(h->hcd, port, st);
        return r;
    }
    usb_control(h->dev, USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_SET_FEATURE, PORT_RESET, port, NULL, 0);
    for (int i = 0; i < 50; i++) {
        thread_sleep_ms(10);
        uint16_t s[2];
        if (usb_control(h->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0, port, s, 4) < 4) return -EIO;
        if (!(s[0] & 0x10)) break;   /* reset done */
    }
    if (!port_status(h, port, st)) return -EIO;
    return st->enabled ? 0 : -EIO;
}

/* returns true when the device tree changed */
static bool check_port(struct usb_hub *h, int p) {
    struct usb_port_status st;
    if (!port_status(h, p, &st)) return false;   /* no answer is not an unplug: try again next pass */
    struct usb_device *c = h->child[p - 1];
    bool topo = false;
    if (st.changed) h->failed[p - 1] = 0;
    if (c && (!st.connected || st.changed)) { disconnect(c); h->child[p - 1] = c = NULL; topo = true; }
    if (!st.connected || c || h->failed[p - 1] || (h->dev && h->dev->gone)) return topo;
    thread_sleep_ms(100);              /* debounce, let power settle */
    if (!port_status(h, p, &st) || !st.connected) return topo;
    if (port_reset(h, p, &st) < 0 || !st.enabled) {
        h->failed[p - 1] = 1;          /* includes ports an EHCI handed to its companion controller */
        return topo;
    }
    thread_sleep_ms(10);               /* reset recovery */
    c = enumerate(h, p, st.speed);
    if (!c) h->failed[p - 1] = 1;
    h->child[p - 1] = c;
    return true;
}

static struct usb_hub *hub_new(struct usb_hcd *hcd, struct usb_device *dev, int nports) {
    struct usb_hub *h = kzalloc(sizeof *h);
    h->hcd = hcd; h->dev = dev; h->nports = nports;
    h->child = kzalloc(sizeof(struct usb_device *) * nports);
    h->failed = kzalloc(nports);
    h->changed = true;
    struct usb_hub **pp = &hubs;
    while (*pp) pp = &(*pp)->next;
    *pp = h;
    return h;
}

static void hub_thread(void *arg) {
    for (;;) {
        sem_wait_timeout(&hub_sem, 500);
        mutex_lock(&usb_lock);
    again:
        for (struct usb_hub *h = hubs; h; h = h->next) {
            /* external hubs report changes on their interrupt endpoint; read them every few seconds regardless */
            if (h->dev && !h->changed && ++h->idle < 6) continue;
            h->changed = false; h->idle = 0;
            for (int p = 1; p <= h->nports; p++) if (check_port(h, p)) goto again;
        }
        mutex_unlock(&usb_lock);
    }
}

void usb_hub_event(void) { if (started) sem_post(&hub_sem); }

static void hub_irq_done(struct usb_xfer *x) {
    struct usb_hub *h = x->ctx;
    if (x->status == -ENODEV || x->dev->gone) return;
    h->changed = true;
    usb_hub_event();
    if (x->status == 0) usb_submit(x);   /* on errors the periodic reads take over */
}

static bool hub_probe(struct usb_interface *i) {
    if (i->cls != 9) return false;
    struct usb_device *d = i->dev;
    if (d->depth >= 5) { kprintf("usb: hub nested too deeply, ignored\n"); return false; }
    bool usb3 = d->speed == USB_SPEED_SUPER;
    if (usb3) usb_control(d, USB_TYPE_CLASS, 12 /* SET_HUB_DEPTH */, d->depth, 0, NULL, 0);
    uint8_t desc[16] = { 0 };
    if (usb_control(d, USB_DIR_IN | USB_TYPE_CLASS, USB_REQ_GET_DESCRIPTOR, (usb3 ? USB_DT_SS_HUB : USB_DT_HUB) << 8, 0, desc, sizeof desc) < 7) return false;
    int nports = MIN(desc[2], 15), chars = desc[3] | desc[4] << 8, pwr_ms = desc[5] * 2;
    d->hub_ports = nports; d->hub_ttt = (chars >> 5) & 3;
    if (d->hcd->ops->hub_config && d->hcd->ops->hub_config(d) < 0) return false;
    for (int p = 1; p <= nports; p++) usb_control(d, USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_SET_FEATURE, PORT_POWER, p, NULL, 0);
    thread_sleep_ms(MAX(pwr_ms, 100));
    struct usb_hub *h = hub_new(d->hcd, d, nports);
    h->usb3 = usb3;
    d->hub = h;
    struct usb_endpoint *ep = usb_find_ep(i, USB_EP_INTR, true);
    if (ep) {
        h->irqbuf = usb_buf_alloc(d, 8);
        h->irq.dev = d; h->irq.ep = ep; h->irq.buf = h->irqbuf; h->irq.len = MIN((nports + 8) / 8, (int)ep->mps);
        h->irq.done = hub_irq_done; h->irq.ctx = h;
        usb_submit(&h->irq);
    }
    kprintf("usb:   %s hub, %d ports\n", usb3 ? "USB 3" : d->speed == USB_SPEED_HIGH ? "USB 2.0" : "USB 1.1", nports);
    return true;
}

static const struct usb_driver usb_hub_driver = { "hub", hub_probe, NULL };

/* ---- controllers ---- */

void usb_register_hcd(struct usb_hcd *h) {
    if (nhcds >= (int)ARRAY_LEN(hcds)) return;
    h->index = nhcds;
    hcds[nhcds++] = h;
    mutex_lock(&usb_lock);
    h->root = hub_new(h, NULL, h->nports);
    mutex_unlock(&usb_lock);
    usb_hub_event();
}

void usb_init(void) {
    extern void fb_boot_progress(const char *);
    fb_boot_progress("Starting USB");
    sem_init(&hub_sem, 0);
    started = true;
    xhci_init();
    ehci_init();
    if (!nhcds) { kprintf("usb: no host controllers\n"); return; }
    thread_create(kernel_process, "usbhub", hub_thread, NULL);
    /* give devices present at boot (keyboards, a stick holding /home) up to 2 s to
     * enumerate: the hub thread holds usb_lock for as long as a scan finds work */
    for (int i = 0; i < 40; i++) {
        thread_sleep_ms(50);
        if (i >= 8 && mutex_trylock(&usb_lock)) { mutex_unlock(&usb_lock); break; }
    }
}

/* ---- lsusb ---- */

static void print_dev(struct usb_device *d, bool verbose, void (*line)(void *, const char *), void *ctx) {
    char s[200];
    char indent[16]; int n = MIN(d->depth * 2 + 2, 14); memset(indent, ' ', n); indent[n] = 0;
    snprintf(s, sizeof s, "%sPort %d: %04x:%04x %s%s%s, %s, address %d", indent, d->port, d->desc.idVendor, d->desc.idProduct,
             d->manufacturer, d->manufacturer[0] ? " " : "", d->product[0] ? d->product : "(no name)", usb_speed_name(d->speed), d->addr);
    line(ctx, s);
    for (int i = 0; i < d->nintf; i++) {
        struct usb_interface *in = &d->intf[i];
        if (!verbose && in->driver == &usb_hub_driver) continue;
        snprintf(s, sizeof s, "%s  interface %d: %s (%02x:%02x:%02x) -> %s", indent, in->num, class_name(in->cls), in->cls, in->subcls, in->proto, in->driver ? in->driver->name : "no driver");
        line(ctx, s);
        if (verbose) for (int k = 0; k < in->nep; k++) {
            static const char *types[] = { "control", "isochronous", "bulk", "interrupt" };
            snprintf(s, sizeof s, "%s    endpoint %02x %s %s, max packet %d, interval %d", indent, in->ep[k].addr, types[in->ep[k].type],
                     in->ep[k].addr & USB_DIR_IN ? "IN" : "OUT", in->ep[k].mps, in->ep[k].interval);
            line(ctx, s);
        }
    }
    if (verbose && d->serial[0]) { snprintf(s, sizeof s, "%s  serial %s", indent, d->serial); line(ctx, s); }
    if (d->hub) for (int p = 0; p < d->hub->nports; p++) if (d->hub->child[p]) print_dev(d->hub->child[p], verbose, line, ctx);
}

void usb_print(bool verbose, void (*line)(void *, const char *), void *ctx) {
    char s[160];
    if (!nhcds) { line(ctx, "no USB host controllers"); return; }
    mutex_lock(&usb_lock);
    for (int i = 0; i < nhcds; i++) {
        struct usb_hcd *h = hcds[i];
        snprintf(s, sizeof s, "Bus %d: %s controller %04x:%04x at %02x:%02x.%d, %d ports", i, h->name, h->pci->vendor, h->pci->device,
                 h->pci->bus, h->pci->slot, h->pci->func, h->nports);
        line(ctx, s);
        for (int p = 0; p < h->root->nports; p++) if (h->root->child[p]) print_dev(h->root->child[p], verbose, line, ctx);
    }
    mutex_unlock(&usb_lock);
}
