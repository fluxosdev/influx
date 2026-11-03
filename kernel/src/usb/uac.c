/* USB Audio Class 1 and 2 playback. The first streaming interface with an
 * isochronous OUT endpoint becomes an audio_device; the audio core hands it
 * 16-bit stereo frames, which wait in a ring until the completion callback of
 * one of the isochronous transfers converts them to the device's format
 * (channels, 16/24/32-bit subslots) and resubmits. Packets carry the nominal
 * rate's frames, or what the feedback endpoint asks for on asynchronous
 * devices. The interface switches to its zero-bandwidth setting after a
 * second without sound. Needs xHCI: EHCI has no isochronous support here. */
#include "usb.h"
#include "audio.h"
#include "mm.h"
#include "string.h"

#define MAX_ALTS    8
#define MAX_RATES   16
#define NXFER       6
#define MAX_PKTS    32
#define RING_FRAMES 16384    /* ~340 ms at 48 kHz */

#define CS_INTERFACE 0x24
#define UAC_CUR      0x01    /* UAC1 SET_CUR; UAC2 CUR */
#define UAC2_RANGE   0x02
#define UAC1_GET_MAX 0x83

struct uac_alt {
    uint8_t alt, channels, subslot, bits;
    uint8_t ep, fb, sync, synch_addr;
    uint16_t bytes;                       /* largest packet: max packet size * transactions */
    uint8_t terminal, interval;
    bool pcm, type1;
    bool cont; uint32_t rmin, rmax;       /* UAC1 continuous range, else rates[] */
    uint32_t rates[MAX_RATES]; int nrates;
};

struct ent { uint8_t sub, id, src, clk; uint16_t ttype; };

struct uac {
    struct audio_device adev;
    struct usb_device *dev;
    struct usb_interface *as;
    uint8_t ac_num;
    int ver;
    struct uac_alt alts[MAX_ALTS];
    int nalt;
    struct uac_alt *alt;
    uint32_t rate;
    struct ent ents[32];
    int nent;
    int clock;                            /* UAC2 clock source ID */
    uint32_t c2rates[3 * MAX_RATES];      /* UAC2 clock ranges: min, max, res */
    int nc2;
    /* streaming */
    spinlock_t lock;
    struct mutex ctl;                     /* start, stop and rate changes */
    int16_t *ring;
    uint64_t wpos, rpos;                  /* frames written by clients, taken by transfers */
    uint64_t inq;                         /* frames inside submitted transfers */
    struct usb_xfer xf[NXFER];
    uint8_t *xbuf[NXFER];
    uint16_t plen[NXFER][MAX_PKTS];
    uint32_t xframes[NXFER];
    int pkts, pps;                        /* packets per transfer, packets per second */
    uint32_t fpp, fpp_nom, acc;           /* frames per packet, 16.16 */
    struct usb_endpoint *ep, *fbep;
    struct usb_xfer fbx;
    uint16_t fbx_len;
    uint8_t *fbbuf;
    int fbshift; bool fbshift_known;
    volatile int inflight;
    volatile bool fb_busy, running, stopping, gone;
    uint64_t idle_since;
    struct semaphore room, wake;
};

/* ---- class requests ---- */

static int ac_req(struct uac *u, uint8_t type, uint8_t req, uint16_t value, uint8_t entity, void *data, uint16_t len) {
    return usb_control(u->dev, type | 0x21, req, value, entity << 8 | u->ac_num, data, len);   /* class, interface */
}

static struct ent *ent_find(struct uac *u, uint8_t id) {
    for (int i = 0; i < u->nent; i++) if (u->ents[i].id == id) return &u->ents[i];
    return NULL;
}

/* UAC2: follow the terminal's clock through selectors and multipliers to its source */
static int find_clock(struct uac *u, uint8_t terminal) {
    struct ent *t = ent_find(u, terminal);
    uint8_t id = t ? t->clk : 0;
    for (int hop = 0; hop < 6 && id; hop++) {
        struct ent *e = ent_find(u, id);
        if (!e) return 0;
        if (e->sub == 0x0A) return id;
        if (e->sub == 0x0B) { uint8_t pin = 1; ac_req(u, 0, UAC_CUR, 0x0100, id, &pin, 1); }
        id = e->src;
    }
    return 0;
}

static bool rate_ok(struct uac *u, const struct uac_alt *a, uint32_t hz) {
    if (u->ver == 2) {
        for (int i = 0; i < u->nc2; i++) {
            uint32_t lo = u->c2rates[i * 3], hi = u->c2rates[i * 3 + 1], res = u->c2rates[i * 3 + 2];
            if (hz >= lo && hz <= hi && (!res || (hz - lo) % res == 0)) return true;
        }
        return false;
    }
    if (a->cont) return hz >= a->rmin && hz <= a->rmax;
    for (int i = 0; i < a->nrates; i++) if (a->rates[i] == hz) return true;
    return false;
}

/* packets per second of an isochronous endpoint with this bInterval */
static int ep_pps(struct uac *u, int interval) {
    return MAX((u->dev->speed == USB_SPEED_FULL ? 1000 : 8000) >> (CLAMP(interval, 1, 16) - 1), 1);
}

/* the best setting for a rate: stereo, then 16-bit, with packets big enough for it
 * (fill() clamps the extra frame a feedback endpoint may ask for) */
static struct uac_alt *pick_alt(struct uac *u, uint32_t hz) {
    struct uac_alt *best = NULL;
    int best_score = -1;
    for (int i = 0; i < u->nalt; i++) {
        struct uac_alt *a = &u->alts[i];
        int pps = ep_pps(u, a->interval);
        if (!rate_ok(u, a, hz) || (hz + pps - 1) / pps * a->channels * a->subslot > a->bytes) continue;   /* nominal packets must fit */
        int score = (a->channels == 2) * 4 + (a->subslot == 2) * 2 + (a->channels >= 2);
        if (score > best_score) { best = a; best_score = score; }
    }
    return best;
}

/* unmute and set 0 dB (or the most the unit allows) on the feature unit fed by the stream */
static void setup_volume(struct uac *u, uint8_t terminal) {
    struct ent *fu = NULL;
    for (int i = 0; i < u->nent && !fu; i++) if (u->ents[i].sub == 0x06 && u->ents[i].src == terminal) fu = &u->ents[i];
    if (!fu) return;
    for (int ch = 0; ch <= 2; ch++) {
        uint8_t mute = 0;
        ac_req(u, 0, UAC_CUR, 0x0100 | ch, fu->id, &mute, 1);
        int16_t max = 0;
        uint8_t range[8];
        if (u->ver == 2) { if (ac_req(u, USB_DIR_IN, UAC2_RANGE, 0x0200 | ch, fu->id, range, 8) == 8) max = (int16_t)(range[4] | range[5] << 8); else continue; }
        else if (ac_req(u, USB_DIR_IN, UAC1_GET_MAX, 0x0200 | ch, fu->id, &max, 2) != 2) continue;
        int16_t vol = MIN(max, 0);
        ac_req(u, 0, UAC_CUR, 0x0200 | ch, fu->id, &vol, 2);
    }
}

static void set_device_rate(struct uac *u) {
    uint8_t f[4] = { u->rate, u->rate >> 8, u->rate >> 16, u->rate >> 24 };
    if (u->ver == 2) { if (u->clock) ac_req(u, 0, UAC_CUR, 0x0100, u->clock, f, 4); }
    else usb_control(u->dev, 0x22, UAC_CUR, 0x0100, u->alt->ep, f, 3);   /* sampling frequency control of the endpoint */
}

/* ---- streaming ---- */

static void fill(struct uac *u, int k) {
    const struct uac_alt *a = u->alt;
    int fb = a->channels * a->subslot, maxf = a->bytes / fb;
    uint8_t *p = u->xbuf[k];
    uint32_t total = 0, real = 0;
    for (int i = 0; i < u->pkts; i++) {
        u->acc += u->fpp;
        int n = MIN((int)(u->acc >> 16), maxf);
        u->acc &= 0xFFFF;
        for (int j = 0; j < n; j++, p += fb) {
            int16_t l = 0, r = 0;
            if (u->rpos < u->wpos) {
                const int16_t *s = u->ring + (u->rpos % RING_FRAMES) * 2;
                l = s[0]; r = s[1];
                u->rpos++; real++;
            }
            for (int c = 0; c < a->channels; c++) {
                int16_t v = a->channels == 1 ? (l + r) / 2 : c == 0 ? l : c == 1 ? r : 0;
                uint8_t *q = p + c * a->subslot;
                for (int b = 0; b < a->subslot - 2; b++) q[b] = 0;   /* 16-bit audio in the top of a wider subslot */
                q[a->subslot - 2] = v; q[a->subslot - 1] = v >> 8;
            }
        }
        u->plen[k][i] = n * fb;
        total += n * fb;
    }
    u->xf[k].len = total;
    u->xframes[k] = real;
    u->inq += real;
}

static void xfer_done(struct usb_xfer *x) {
    struct uac *u = x->ctx;
    int k = x - u->xf;
    spin_lock(&u->lock);
    u->inq -= u->xframes[k];
    u->xframes[k] = 0;
    bool more = !u->stopping && !u->gone && x->status != -ENODEV;
    if (more) fill(u, k);
    spin_unlock(&u->lock);
    if (more && usb_submit(x) == 0) { sem_post(&u->room); return; }
    spin_lock(&u->lock);
    u->inq -= u->xframes[k]; u->xframes[k] = 0;
    u->inflight--;
    spin_unlock(&u->lock);
    sem_post(&u->room);
    sem_post(&u->wake);
}

/* Asynchronous devices report the rate they consume: 10.14 frames per frame
 * at full speed, 16.16 per microframe at high speed. Devices get the format
 * wrong often enough that the scale is found by shifting the value until it
 * lands near the nominal rate, then kept. */
static void fb_done(struct usb_xfer *x) {
    struct uac *u = x->ctx;
    if (u->stopping || u->gone || x->status == -ENODEV) { u->fb_busy = false; sem_post(&u->wake); return; }
    if (x->status == 0 && x->actual >= 3) {
        const uint8_t *b = u->fbbuf;
        uint64_t v = b[0] | b[1] << 8 | (uint32_t)b[2] << 16 | (x->actual >= 4 ? (uint32_t)b[3] << 24 : 0);
        uint64_t base = u->dev->speed == USB_SPEED_FULL ? (x->actual == 3 ? v << 2 : v) : v * (8000 / u->pps);
        static const int8_t shifts[] = { 0, 1, -1, 2, -2, 3, -3, 4, -4 };
        for (size_t i = 0; i < ARRAY_LEN(shifts); i++) {
            int sh = u->fbshift_known ? u->fbshift : shifts[i];
            uint64_t f = sh >= 0 ? base << sh : base >> -sh;
            if (f > u->fpp_nom - u->fpp_nom / 8 && f < u->fpp_nom + u->fpp_nom / 8) {
                u->fbshift = sh; u->fbshift_known = true;
                spin_lock(&u->lock); u->fpp = (uint32_t)f; spin_unlock(&u->lock);
                break;
            }
            if (u->fbshift_known) break;
        }
    }
    if (usb_submit(x) < 0) { u->fb_busy = false; sem_post(&u->wake); }
}

static void stream_stop(struct uac *u) {
    if (!u->running) return;
    u->stopping = true;
    for (int i = 0; i < 100 && (u->inflight || u->fb_busy) && !u->gone; i++) sem_wait_timeout(&u->wake, 20);
    if (!u->gone) usb_set_interface(u->as, 0);
    u->running = false;
    u->stopping = false;
}

/* the caller holds ctl */
static int stream_start(struct uac *u) {
    if (u->running) return 0;
    int r = usb_set_interface(u->as, u->alt->alt);
    if (r < 0) { kprintf("usb-audio: %s: setting %d not accepted (%d)\n", u->adev.name, u->alt->alt, r); return r; }
    set_device_rate(u);
    u->ep = usb_ep_by_addr(u->as, u->alt->ep);
    u->fbep = u->alt->fb ? usb_ep_by_addr(u->as, u->alt->fb) : NULL;
    if (!u->ep) { usb_set_interface(u->as, 0); return -EINVAL; }
    u->pps = ep_pps(u, u->ep->interval);
    u->pkts = CLAMP(u->pps / 250, 1, MAX_PKTS);   /* 4 ms per transfer */
    u->fpp = u->fpp_nom = (uint32_t)(((uint64_t)u->rate << 16) / u->pps);
    u->acc = 0;
    u->stopping = false;
    u->running = true;
    for (int k = 0; k < NXFER; k++) {
        struct usb_xfer *x = &u->xf[k];
        memset(x, 0, sizeof *x);
        x->dev = u->dev; x->ep = u->ep; x->buf = u->xbuf[k]; x->npackets = u->pkts; x->packet_len = u->plen[k];
        x->done = xfer_done; x->ctx = u;
        spin_lock(&u->lock);
        fill(u, k);
        u->inflight++;
        spin_unlock(&u->lock);
        if (usb_submit(x) < 0) {
            spin_lock(&u->lock); u->inflight--; u->inq -= u->xframes[k]; u->xframes[k] = 0; spin_unlock(&u->lock);
        }
    }
    if (u->fbep && u->alt->sync == 1) {
        memset(&u->fbx, 0, sizeof u->fbx);
        u->fbx.dev = u->dev; u->fbx.ep = u->fbep; u->fbx.buf = u->fbbuf; u->fbx.len = MIN(u->fbep->mps, 8);
        u->fbx.npackets = 1; u->fbx.packet_len = &u->fbx_len; u->fbx_len = u->fbx.len;
        u->fbx.done = fb_done; u->fbx.ctx = u;
        u->fb_busy = usb_submit(&u->fbx) == 0;
    }
    if (!u->inflight) { stream_stop(u); return -EIO; }
    return 0;
}

static void service(void *arg) {
    struct uac *u = arg;
    while (!u->gone) {
        sem_wait_timeout(&u->wake, 200);
        mutex_lock(&u->ctl);
        spin_lock(&u->lock);
        bool idle = u->running && u->rpos == u->wpos && !u->inq && time_ms() - u->idle_since > 1000;
        spin_unlock(&u->lock);
        if (idle && !u->gone) stream_stop(u);
        mutex_unlock(&u->ctl);
    }
    thread_exit(0);
}

/* ---- audio_ops ---- */

static int uac_set_rate(void *priv, uint32_t hz) {
    struct uac *u = priv;
    mutex_lock(&u->ctl);   /* u->dev is freed once gone is set and ctl released */
    if (u->gone) { mutex_unlock(&u->ctl); return -ENODEV; }
    if (hz == u->rate) { mutex_unlock(&u->ctl); return 0; }
    struct uac_alt *a = pick_alt(u, hz);
    if (!a) { mutex_unlock(&u->ctl); return -EINVAL; }
    stream_stop(u);
    spin_lock(&u->lock);
    u->rpos = u->wpos;
    spin_unlock(&u->lock);
    u->alt = a; u->rate = hz; u->adev.rate = hz;
    mutex_unlock(&u->ctl);
    return 0;
}

static int start(struct uac *u) {
    mutex_lock(&u->ctl);
    int r = u->running || u->gone ? 0 : stream_start(u);
    mutex_unlock(&u->ctl);
    return r;
}

static int64_t uac_write(void *priv, const int16_t *frames, size_t count, volatile bool *cancel) {
    struct uac *u = priv;
    size_t done = 0;
    while (done < count && !(cancel && *cancel)) {
        if (u->gone) return done ? (int64_t)done : -ENODEV;
        spin_lock(&u->lock);
        size_t n = MIN(RING_FRAMES - (size_t)(u->wpos - u->rpos), count - done);
        uint64_t w = u->wpos;
        spin_unlock(&u->lock);
        /* only this writer touches the ring past wpos */
        for (size_t i = 0; i < n; i++) {
            int16_t *d = u->ring + ((w + i) % RING_FRAMES) * 2;
            d[0] = frames[(done + i) * 2]; d[1] = frames[(done + i) * 2 + 1];
        }
        spin_lock(&u->lock);
        u->wpos += n;
        u->idle_since = time_ms();
        spin_unlock(&u->lock);
        done += n;
        /* start once 40 ms are queued, so the first transfers do not run dry */
        if (!u->running && (u->wpos - u->rpos >= u->rate / 25 || !n)) {
            int r = start(u);
            if (r < 0) return done ? (int64_t)done : r;
        }
        if (!n) sem_wait_timeout(&u->room, 20);
    }
    return done;
}

static size_t uac_queued(void *priv) {
    struct uac *u = priv;
    if (u->gone) return 0;
    spin_lock(&u->lock);
    size_t q = (size_t)(u->wpos - u->rpos + u->inq);
    spin_unlock(&u->lock);
    return q;
}

static void uac_drain(void *priv, volatile bool *cancel) {
    struct uac *u = priv;
    if (!u->running && uac_queued(u) && start(u) < 0) return;   /* a sound shorter than the start threshold */
    while (uac_queued(u) && !u->gone && !(cancel && *cancel)) thread_sleep_ms(5);
}

static void uac_stop(void *priv) {
    struct uac *u = priv;
    spin_lock(&u->lock);
    u->rpos = u->wpos;   /* what is already in transfers plays out, a few ms */
    spin_unlock(&u->lock);
}

static const struct audio_ops uac_ops = { uac_set_rate, uac_write, uac_drain, uac_stop, uac_queued };

/* ---- descriptors ---- */

static uint32_t rd24(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16; }

static void parse_cs_ac(struct uac *u, const uint8_t *p) {
    if (p[0] < 5 || u->nent >= (int)ARRAY_LEN(u->ents)) return;
    struct ent e = { .sub = p[2], .id = p[3] };
    switch (p[2]) {
    case 0x02: if (p[0] < 8) return; e.ttype = p[4] | p[5] << 8; if (u->ver == 2) e.clk = p[7]; break;   /* input terminal */
    case 0x03: if (p[0] < 9) return; e.ttype = p[4] | p[5] << 8; e.src = p[7]; if (u->ver == 2) e.clk = p[8]; break;   /* output terminal */
    case 0x06: e.src = p[4]; break;   /* feature unit */
    case 0x0A: if (u->ver != 2) return; break;   /* clock source */
    case 0x0B: if (u->ver != 2 || p[0] < 6) return; e.src = p[5]; break;   /* clock selector: first input */
    case 0x0C: if (u->ver != 2) return; e.src = p[4]; break;   /* clock multiplier */
    default: return;
    }
    u->ents[u->nent++] = e;
}

static void parse_cs_as(struct uac *u, struct uac_alt *a, const uint8_t *p) {
    if (p[2] == 0x01 && p[0] >= 7) {   /* AS_GENERAL */
        a->terminal = p[3];
        if (u->ver == 2) { if (p[0] >= 11) { a->pcm = p[5] == 1 && (p[6] & 1); a->channels = p[10]; } }
        else a->pcm = (p[5] | p[6] << 8) == 0x0001;
    } else if (p[2] == 0x02 && p[0] >= 6 && p[3] == 1) {   /* FORMAT_TYPE I */
        a->type1 = true;
        if (u->ver == 2) { a->subslot = p[4]; a->bits = p[5]; return; }
        if (p[0] < 8) return;
        a->channels = p[4]; a->subslot = p[5]; a->bits = p[6];
        int n = p[7];
        if (n == 0 && p[0] >= 14) { a->cont = true; a->rmin = rd24(p + 8); a->rmax = rd24(p + 11); }
        for (int i = 0; i < n && i < MAX_RATES && 8 + i * 3 + 3 <= p[0]; i++) a->rates[a->nrates++] = rd24(p + 8 + i * 3);
    }
}

/* the first streaming interface with an isochronous OUT endpoint, all its usable settings */
static void parse(struct uac *u) {
    struct usb_device *d = u->dev;
    int num = -1, cls = 0, sub = 0, as_num = -1;
    struct uac_alt cur, *a = NULL;
    for (int off = 0; ; ) {
        const uint8_t *p = d->config + off;
        bool end = off + 2 > d->config_len || p[0] < 2 || off + p[0] > d->config_len;
        if (end || p[1] == USB_DT_INTERFACE) {
            if (a && a->ep && a->pcm && a->type1 && a->channels && a->subslot >= 2 && a->subslot <= 4 && u->nalt < MAX_ALTS) {
                u->alts[u->nalt++] = *a;
                as_num = num;
            }
            a = NULL;
            if (end) break;
            if (p[0] >= 9) {
                num = p[2]; cls = p[5]; sub = p[6];
                if (cls == 1 && sub == 2 && p[3] && (as_num < 0 || as_num == num)) { memset(&cur, 0, sizeof cur); cur.alt = p[3]; a = &cur; }
            }
        } else if (p[1] == CS_INTERFACE && p[0] >= 3) {
            if (cls == 1 && sub == 1 && num == u->ac_num) parse_cs_ac(u, p);
            else if (a) parse_cs_as(u, a, p);
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && a && (p[3] & 3) == USB_EP_ISOC) {
            uint16_t mps = p[4] | p[5] << 8;
            if (!(p[2] & USB_DIR_IN)) {
                a->ep = p[2]; a->sync = (p[3] >> 2) & 3;
                a->bytes = (mps & 0x7FF) * (((mps >> 11) & 3) + 1);
                a->synch_addr = p[0] >= 9 ? p[8] : 0;
                a->interval = p[6];
            } else if (!a->fb) a->fb = p[2];
        }
        off += p[0];
    }
    for (int i = 0; i < u->nalt; i++) {   /* a feedback endpoint must be the one the data endpoint names, if it names one */
        struct uac_alt *x = &u->alts[i];
        if (x->fb && x->synch_addr && (x->synch_addr | USB_DIR_IN) != x->fb) x->fb = 0;
    }
    u->as = NULL;
    for (int i = 0; i < d->nintf; i++) if (d->intf[i].num == as_num) u->as = &d->intf[i];
}

/* UAC2: the clock's rate ranges */
static void read_clock_rates(struct uac *u) {
    uint8_t *b = kzalloc(2 + 12 * MAX_RATES);
    if (ac_req(u, USB_DIR_IN, UAC2_RANGE, 0x0100, u->clock, b, 2) == 2) {
        int n = MIN(b[0] | b[1] << 8, MAX_RATES);
        int got = ac_req(u, USB_DIR_IN, UAC2_RANGE, 0x0100, u->clock, b, 2 + 12 * n);
        for (int i = 0; i < n && 2 + 12 * i + 12 <= got; i++) {
            const uint8_t *r = b + 2 + 12 * i;
            for (int k = 0; k < 3; k++) u->c2rates[i * 3 + k] = r[k * 4] | r[k * 4 + 1] << 8 | r[k * 4 + 2] << 16 | (uint32_t)r[k * 4 + 3] << 24;
            u->nc2++;
        }
    }
    kfree(b);
}

static bool uac_probe(struct usb_interface *i) {
    if (i->cls != 1 || i->subcls != 1) return false;   /* AudioControl; streaming interfaces are taken here */
    struct usb_device *d = i->dev;
    struct uac *u = kzalloc(sizeof *u);
    u->dev = d; u->ac_num = i->num;
    u->ver = i->proto == 0x20 ? 2 : 1;
    parse(u);
    if (!u->as || !u->nalt) { kprintf("usb:   interface %d: audio without a playback stream\n", i->num); kfree(u); return false; }
    for (int k = 0; k < d->nintf; k++) if (d->intf[k].cls == 1 && d->intf[k].subcls == 2) d->intf[k].driver = &usb_audio_driver;
    if (!d->hcd->ops->set_interface) {
        kprintf("usb:   interface %d: USB audio needs isochronous transfers, which the %s driver lacks\n", i->num, d->hcd->name);
        kfree(u);
        return true;
    }
    if (u->ver == 2 && (u->clock = find_clock(u, u->alts[0].terminal))) read_clock_rates(u);
    static const uint32_t pref[] = { 48000, 44100, 96000, 32000, 22050, 16000, 8000 };
    for (size_t k = 0; k < ARRAY_LEN(pref) && !u->alt; k++) if ((u->alt = pick_alt(u, pref[k]))) u->rate = pref[k];
    if (!u->alt) { kprintf("usb:   interface %d: no usable audio format\n", i->num); kfree(u); return true; }
    setup_volume(u, u->alt->terminal);

    int maxbytes = 0;
    for (int k = 0; k < u->nalt; k++) maxbytes = MAX(maxbytes, u->alts[k].bytes);
    for (int k = 0; k < NXFER; k++) u->xbuf[k] = kzalloc(maxbytes * MAX_PKTS);
    u->fbbuf = usb_buf_alloc(d, 8);
    u->ring = kzalloc(RING_FRAMES * 4);
    mutex_init(&u->ctl);
    sem_init(&u->room, 0); sem_init(&u->wake, 0);
    i->drvdata = u;

    const struct uac_alt *a = u->alt;
    static const char *syncs[] = { "no sync", "asynchronous", "adaptive", "synchronous" };
    snprintf(u->adev.name, sizeof u->adev.name, "%s (USB %04x:%04x)", d->product[0] ? d->product : "USB Audio", d->desc.idVendor, d->desc.idProduct);
    snprintf(u->adev.detail, sizeof u->adev.detail, "USB Audio Class %d, %d-bit %s, %s%s, %d setting%s", u->ver, a->bits,
             a->channels == 1 ? "mono" : a->channels == 2 ? "stereo" : "multichannel", syncs[a->sync], a->sync == 1 && a->fb ? " with feedback" : "",
             u->nalt, u->nalt == 1 ? "" : "s");
    u->adev.ops = &uac_ops; u->adev.priv = u; u->adev.rate = u->rate;
    kprintf("usb:   interface %d: audio output, %u Hz\n", i->num, u->rate);
    thread_create(kernel_process, "usbaudio", service, u);
    audio_register(&u->adev);
    return true;
}

/* the struct stays allocated: an open audio stream may still point at it.
 * Taking ctl waits out a start or stop still using the device, which the
 * caller frees next; everyone checks gone under ctl before touching it. */
static void uac_disconnect(struct usb_interface *i) {
    struct uac *u = i->drvdata;
    if (!u) return;
    u->gone = true;
    sem_post(&u->room); sem_post(&u->wake);
    mutex_lock(&u->ctl);
    mutex_unlock(&u->ctl);
    audio_unregister(&u->adev);
}

const struct usb_driver usb_audio_driver = { "usb-audio", uac_probe, uac_disconnect };
