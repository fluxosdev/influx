/*
 * Intel High Definition Audio.
 *
 * Controller: reset, CORB/RIRB command rings (immediate command registers as
 * a fallback), one output stream descriptor running a cyclic 64 KiB DMA
 * buffer split into 16 BDL fragments. Codecs: the audio function group's
 * widget graph is read once; every analog output pin (speaker, headphone,
 * line out) gets a path back to a DAC through mixers and selectors, which is
 * powered up and unmuted, and all those DACs listen to the same stream so
 * sound comes out everywhere at once.
 *
 * Playback keeps two byte counters: written (w_pos) and played (hw_pos, from
 * LPIB). Whatever the hardware has played is zeroed right away, so running
 * dry plays silence instead of looping stale audio, and the stream stops
 * after a short idle period. A service thread tracks the position every
 * 10 ms; the stream interrupt (MSI, or INTx through ACPI _PRT) only wakes it
 * early.
 */
#include "audio.h"
#include "arch.h"
#include "io.h"
#include "mm.h"
#include "sched.h"
#include "fs.h"
#include "string.h"

/* controller registers */
#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define INTCTL    0x20
#define INTSTS    0x24
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E
#define ICOI      0x60
#define IRII      0x64
#define ICS       0x68
/* stream descriptor registers */
#define SD_CTL    0x00
#define SD_CTL2   0x02    /* stream tag in bits 4-7 */
#define SD_STS    0x03
#define SD_LPIB   0x04
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDPL   0x18
#define SD_BDPU   0x1C
#define SD_RST    0x01
#define SD_RUN    0x02
#define SD_IOCE   0x04
#define SD_STS_ALL 0x1C   /* BCIS, FIFOE, DESE */

/* verbs */
#define V_PARAM       0xF00
#define V_CONN_LIST   0xF02
#define V_SET_SEL     0x701
#define V_SET_POWER   0x705
#define V_SET_STREAM  0x706
#define V_SET_PIN     0x707
#define V_SET_EAPD    0x70C
#define V_CONFIG      0xF1C
#define V_FMT         0x2     /* 4-bit verbs carry 16-bit payloads */
#define V_AMP         0x3
/* parameters */
#define P_VENDOR      0x00
#define P_NODES       0x04
#define P_FG_TYPE     0x05
#define P_WCAPS       0x09
#define P_PCM         0x0A
#define P_PIN_CAPS    0x0C
#define P_IN_AMP      0x0D
#define P_CONN_LEN    0x0E
#define P_OUT_AMP     0x12

enum { W_OUTPUT = 0, W_INPUT, W_MIXER, W_SELECTOR, W_PIN, W_POWER, W_KNOB, W_BEEP };
#define WCAP_IN_AMP   (1u << 1)
#define WCAP_OUT_AMP  (1u << 2)
#define WCAP_AMP_OVR  (1u << 3)
#define WCAP_FMT_OVR  (1u << 4)
#define WCAP_CONN     (1u << 8)
#define WCAP_DIGITAL  (1u << 9)
#define WCAP_POWER    (1u << 10)
#define PIN_OUT       (1u << 4)
#define PIN_HP        (1u << 3)
#define PIN_HDMI      (1u << 7)
#define PIN_EAPD      (1u << 16)
#define PIN_DP        (1u << 24)

#define BUF_SIZE   (64 * 1024)
#define NFRAGS     16
#define FRAG_SIZE  (BUF_SIZE / NFRAGS)
#define MAX_PATHS  4
#define MAX_DEPTH  6

struct widget { uint8_t type, nconn; uint32_t caps, pin_caps, config, out_amp, in_amp, pcm; uint8_t conn[32]; };
struct path { int len; uint8_t nid[MAX_DEPTH + 1], sel[MAX_DEPTH + 1]; };   /* pin first, DAC last */
struct bdl_entry { uint64_t addr; uint32_t len, flags; } PACKED;

struct hda {
    struct pci_device *pci;
    volatile uint8_t *mmio, *sd;
    int sd_index, stream_tag;
    bool immediate, dma64;
    uint32_t *corb; volatile uint64_t *rirb;
    int corb_n, rirb_n, rirb_rp;
    int timeouts;
    /* the codec carrying the output */
    int cad;
    uint32_t vendor;
    struct widget *w;         /* indexed by node id */
    struct path paths[MAX_PATHS]; int npaths;
    uint8_t dacs[MAX_PATHS]; int ndacs;
    uint32_t rates;           /* PCM rate bits every DAC supports */
    /* stream */
    struct bdl_entry *bdl; uint64_t bdl_phys;
    uint8_t *buf; uint64_t buf_phys;
    uint16_t fmt; uint32_t rate;
    uint64_t w_pos, hw_pos; uint32_t last_lpib;
    bool running; uint64_t idle_since;
    uint64_t irqs, underruns;
    struct mutex lock;
    struct semaphore wake;
    struct audio_device dev;
};

static inline uint8_t r8(struct hda *h, int r) { return *(volatile uint8_t *)(h->mmio + r); }
static inline uint16_t r16(struct hda *h, int r) { return *(volatile uint16_t *)(h->mmio + r); }
static inline uint32_t r32(struct hda *h, int r) { return *(volatile uint32_t *)(h->mmio + r); }
static inline void w8(struct hda *h, int r, uint8_t v) { *(volatile uint8_t *)(h->mmio + r) = v; }
static inline void w16(struct hda *h, int r, uint16_t v) { *(volatile uint16_t *)(h->mmio + r) = v; }
static inline void w32(struct hda *h, int r, uint32_t v) { *(volatile uint32_t *)(h->mmio + r) = v; }
static inline uint8_t sd8(struct hda *h, int r) { return *(volatile uint8_t *)(h->sd + r); }
static inline uint32_t sd32(struct hda *h, int r) { return *(volatile uint32_t *)(h->sd + r); }
static inline void sdw8(struct hda *h, int r, uint8_t v) { *(volatile uint8_t *)(h->sd + r) = v; }
static inline void sdw16(struct hda *h, int r, uint16_t v) { *(volatile uint16_t *)(h->sd + r) = v; }
static inline void sdw32(struct hda *h, int r, uint32_t v) { *(volatile uint32_t *)(h->sd + r) = v; }

static void udelay(uint64_t us) {
    uint64_t hz = cpu_tsc_hz();
    if (!hz) { for (uint64_t i = 0; i < us; i++) io_wait(); return; }
    uint64_t end = rdtsc() + us * (hz / 1000000);
    while (rdtsc() < end) pause();
}
static bool wait8(struct hda *h, int r, uint8_t mask, uint8_t want, int ms) {
    for (int i = 0; i < ms * 10; i++) { if ((r8(h, r) & mask) == want) return true; udelay(100); }
    return false;
}

/* ---- codec commands ---- */
static uint32_t cmd_immediate(struct hda *h, uint32_t v) {
    for (int i = 0; i < 1000 && (r16(h, ICS) & 1); i++) udelay(10);
    w16(h, ICS, 2);   /* clear a stale result */
    w32(h, ICOI, v);
    w16(h, ICS, 1);
    for (int i = 0; i < 1000; i++) {
        if ((r16(h, ICS) & 3) == 2) { uint32_t r = r32(h, IRII); w16(h, ICS, 2); return r; }
        udelay(10);
    }
    h->timeouts++;
    return 0;
}

static uint32_t cmd_corb(struct hda *h, uint32_t v) {
    h->rirb_rp = (r16(h, RIRBWP) & 0xFF) % h->rirb_n;   /* one command in flight: whatever is queued is late or unsolicited */
    int wp = (r16(h, CORBWP) + 1) % h->corb_n;
    h->corb[wp] = v;
    w16(h, CORBWP, wp);
    for (int i = 0; i < 10000; i++) {   /* ~100 ms */
        int rwp = r16(h, RIRBWP) & 0xFF;
        while (h->rirb_rp != rwp) {
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_n;
            uint64_t e = h->rirb[h->rirb_rp];
            if ((e >> 32) & 0x10) continue;   /* unsolicited response */
            w8(h, RIRBSTS, 0x05);
            return (uint32_t)e;
        }
        udelay(10);
    }
    h->timeouts++;
    return 0;
}

static uint32_t verb(struct hda *h, int nid, uint32_t vb, uint32_t payload) {
    uint32_t v = (uint32_t)h->cad << 28 | (uint32_t)(nid & 0xFF) << 20;
    v |= vb < 0x10 ? (vb << 16 | (payload & 0xFFFF)) : (vb << 8 | (payload & 0xFF));
    return h->immediate ? cmd_immediate(h, v) : cmd_corb(h, v);
}
static uint32_t param(struct hda *h, int nid, int p) { return verb(h, nid, V_PARAM, p); }

static uint64_t dma_alloc(struct hda *h, size_t pages) { return h->dma64 ? pmm_alloc_pages(pages) : pmm_alloc_pages_low(pages); }

static bool rings_init(struct hda *h) {
    w8(h, CORBCTL, 0); w8(h, RIRBCTL, 0);
    wait8(h, CORBCTL, 2, 0, 10); wait8(h, RIRBCTL, 2, 0, 10);
    uint64_t page = dma_alloc(h, 1);   /* CORB in the first 1 KiB, RIRB from 2 KiB */
    if (!page) return false;
    h->corb = P2V(page); h->rirb = P2V(page + 2048);
    uint8_t cs = r8(h, CORBSIZE) >> 4, rs = r8(h, RIRBSIZE) >> 4;
    int ccode = cs & 4 ? 2 : cs & 2 ? 1 : 0, rcode = rs & 4 ? 2 : rs & 2 ? 1 : 0;
    h->corb_n = ccode == 2 ? 256 : ccode == 1 ? 16 : 2;
    h->rirb_n = rcode == 2 ? 256 : rcode == 1 ? 16 : 2;
    w8(h, CORBSIZE, (r8(h, CORBSIZE) & ~3) | ccode);
    w8(h, RIRBSIZE, (r8(h, RIRBSIZE) & ~3) | rcode);
    w32(h, CORBLBASE, page); w32(h, CORBUBASE, page >> 32);
    w32(h, RIRBLBASE, page + 2048); w32(h, RIRBUBASE, (page + 2048) >> 32);
    /* reset the read pointer: set bit 15, see it, clear it */
    w16(h, CORBRP, 0x8000);
    for (int i = 0; i < 100 && !(r16(h, CORBRP) & 0x8000); i++) udelay(10);
    w16(h, CORBRP, 0);
    for (int i = 0; i < 100 && (r16(h, CORBRP) & 0x8000); i++) udelay(10);
    w16(h, CORBWP, 0);
    w16(h, RIRBWP, 0x8000);
    w16(h, RINTCNT, 1);
    h->rirb_rp = 0;
    /* the response-interrupt enable too: some controllers (QEMU's) only raise the
     * RINTCNT status flag with it set, and stop after RINTCNT responses until the
     * flag is cleared. INTCTL.CIE stays off, so no interrupt is delivered. */
    w8(h, CORBCTL, 2); w8(h, RIRBCTL, 3);
    return wait8(h, CORBCTL, 2, 2, 10) && wait8(h, RIRBCTL, 2, 2, 10);
}

/* ---- codec graph ---- */
static void read_conns(struct hda *h, int nid, struct widget *w) {
    uint32_t cl = param(h, nid, P_CONN_LEN);
    bool lng = cl & 0x80;
    int n = cl & 0x7F, per = lng ? 2 : 4, bits = lng ? 16 : 8;
    uint32_t mask = (1u << (bits - 1)) - 1, range = 1u << (bits - 1);
    int prev = 0;
    for (int i = 0; i < n && w->nconn < 32; i += per) {
        uint32_t r = verb(h, nid, V_CONN_LIST, i);
        for (int k = 0; k < per && i + k < n && w->nconn < 32; k++) {
            uint32_t e = (r >> (k * bits)) & ((1u << bits) - 1);
            int id = e & mask;
            if ((e & range) && prev) for (int x = prev + 1; x <= id && w->nconn < 32; x++) w->conn[w->nconn++] = x;
            else w->conn[w->nconn++] = id;
            prev = id;
        }
    }
}

static int find_afg(struct hda *h) {
    uint32_t nodes = param(h, 0, P_NODES);
    int start = (nodes >> 16) & 0xFF, count = nodes & 0xFF;
    for (int fg = start; fg < start + count; fg++) if ((param(h, fg, P_FG_TYPE) & 0xFF) == 1) return fg;
    return 0;
}

static void read_graph(struct hda *h, int afg) {
    uint32_t nodes = param(h, afg, P_NODES);
    int start = (nodes >> 16) & 0xFF, count = nodes & 0xFF;
    uint32_t def_out = param(h, afg, P_OUT_AMP), def_in = param(h, afg, P_IN_AMP), def_pcm = param(h, afg, P_PCM);
    h->w = kzalloc(256 * sizeof *h->w);
    for (int nid = start; nid < start + count && nid < 256; nid++) {
        struct widget *w = &h->w[nid];
        w->caps = param(h, nid, P_WCAPS);
        w->type = (w->caps >> 20) & 0xF;
        w->out_amp = (w->caps & WCAP_AMP_OVR) ? param(h, nid, P_OUT_AMP) : def_out;
        w->in_amp = (w->caps & WCAP_AMP_OVR) ? param(h, nid, P_IN_AMP) : def_in;
        if (w->type == W_PIN) { w->pin_caps = param(h, nid, P_PIN_CAPS); w->config = verb(h, nid, V_CONFIG, 0); }
        if (w->type == W_OUTPUT) w->pcm = (w->caps & WCAP_FMT_OVR) ? param(h, nid, P_PCM) : def_pcm;
        if (w->caps & WCAP_CONN) read_conns(h, nid, w);
    }
}

/* depth-first search from a pin back to an analog DAC */
static bool find_path(struct hda *h, int nid, struct path *p) {
    if (p->len > MAX_DEPTH || !nid) return false;
    struct widget *w = &h->w[nid];
    for (int i = 0; i < p->len; i++) if (p->nid[i] == nid) return false;
    p->nid[p->len] = nid; p->sel[p->len] = 0; p->len++;
    if (w->type == W_OUTPUT) { if (!(w->caps & WCAP_DIGITAL)) return true; }
    else if (w->type == W_PIN || w->type == W_MIXER || w->type == W_SELECTOR)
        for (int i = 0; i < w->nconn; i++) { p->sel[p->len - 1] = i; if (find_path(h, w->conn[i], p)) return true; }
    p->len--;
    return false;
}

static void set_amp(struct hda *h, int nid, bool output, int index, uint32_t caps) {
    int offset = caps & 0x7F;   /* the step that is 0 dB */
    verb(h, nid, V_AMP, (output ? 0x8000 : 0x4000) | 0x3000 | (index & 0xF) << 8 | offset);
}

static void setup_path(struct hda *h, struct path *p) {
    for (int i = 0; i < p->len; i++) {
        int nid = p->nid[i];
        struct widget *w = &h->w[nid];
        if (w->caps & WCAP_POWER) verb(h, nid, V_SET_POWER, 0);
        if (w->type != W_MIXER && w->nconn > 1) verb(h, nid, V_SET_SEL, p->sel[i]);
        if (w->type != W_OUTPUT && (w->caps & WCAP_IN_AMP)) set_amp(h, nid, false, w->type == W_MIXER ? p->sel[i] : 0, w->in_amp);
        if (w->caps & WCAP_OUT_AMP) set_amp(h, nid, true, 0, w->out_amp);
        if (w->type == W_PIN) {
            int dev = (w->config >> 20) & 0xF;
            verb(h, nid, V_SET_PIN, 0x40 | (dev == 2 && (w->pin_caps & PIN_HP) ? 0x80 : 0));
            if (w->pin_caps & PIN_EAPD) verb(h, nid, V_SET_EAPD, 0x02);
        }
    }
}

static const char *pin_name(struct widget *w) {
    switch ((w->config >> 20) & 0xF) { case 0: return "Line Out"; case 1: return "Speaker"; case 2: return "Headphones"; default: return "Output"; }
}

static bool probe_codec(struct hda *h, int cad) {
    h->cad = cad;
    h->vendor = param(h, 0, P_VENDOR);
    if (!h->vendor || h->vendor == 0xFFFFFFFF) return false;
    int afg = find_afg(h);
    if (!afg) return false;
    verb(h, afg, V_SET_POWER, 0);
    udelay(10000);
    read_graph(h, afg);
    h->npaths = 0;
    /* pass 0 trusts the pin configuration defaults; pass 1 takes any analog output pin */
    for (int pass = 0; pass < 2 && !h->npaths; pass++)
        for (int nid = 1; nid < 256 && h->npaths < MAX_PATHS; nid++) {
            struct widget *w = &h->w[nid];
            if (w->type != W_PIN || !(w->pin_caps & PIN_OUT) || (w->pin_caps & (PIN_HDMI | PIN_DP)) || (w->caps & WCAP_DIGITAL)) continue;
            int conn = w->config >> 30, dev = (w->config >> 20) & 0xF;
            if (pass == 0 && (conn == 1 || dev > 2)) continue;
            struct path p = { 0 };
            if (find_path(h, nid, &p)) h->paths[h->npaths++] = p;
        }
    if (!h->npaths) { kfree(h->w); h->w = NULL; return false; }
    h->ndacs = 0; h->rates = 0xFFF;
    for (int i = 0; i < h->npaths; i++) {
        int dac = h->paths[i].nid[h->paths[i].len - 1];
        bool seen = false;
        for (int k = 0; k < h->ndacs; k++) if (h->dacs[k] == dac) seen = true;
        if (!seen) { h->dacs[h->ndacs++] = dac; h->rates &= h->w[dac].pcm & 0xFFF; }
        setup_path(h, &h->paths[i]);
    }
    if (!(h->rates & (1 << 6))) h->rates |= 1 << 6;   /* 48 kHz is mandatory */
    return true;
}

/* ---- stream ---- */
static const struct { uint32_t hz; uint16_t fmt; int bit; } rate_table[] = {
    { 8000, 5 << 8, 0 }, { 11025, 1 << 14 | 3 << 8, 1 }, { 16000, 2 << 8, 2 }, { 22050, 1 << 14 | 1 << 8, 3 },
    { 32000, 1 << 11 | 2 << 8, 4 }, { 44100, 1 << 14, 5 }, { 48000, 0, 6 }, { 88200, 1 << 14 | 1 << 11, 7 },
    { 96000, 1 << 11, 8 }, { 176400, 1 << 14 | 3 << 11, 9 }, { 192000, 3 << 11, 10 },
};
#define FMT_16BIT_STEREO (1 << 4 | 1)

static void program_dacs(struct hda *h) {
    for (int i = 0; i < h->ndacs; i++) {
        verb(h, h->dacs[i], V_FMT, h->fmt);
        verb(h, h->dacs[i], V_SET_STREAM, h->stream_tag << 4);
    }
}

static void stream_reset(struct hda *h) {
    sdw8(h, SD_CTL, sd8(h, SD_CTL) & ~SD_RUN);
    for (int i = 0; i < 100 && (sd8(h, SD_CTL) & SD_RUN); i++) udelay(10);
    sdw8(h, SD_CTL, SD_RST);
    for (int i = 0; i < 100 && !(sd8(h, SD_CTL) & SD_RST); i++) udelay(10);
    sdw8(h, SD_CTL, 0);
    for (int i = 0; i < 100 && (sd8(h, SD_CTL) & SD_RST); i++) udelay(10);
    sdw8(h, SD_STS, SD_STS_ALL);
}

static void stream_start(struct hda *h) {
    stream_reset(h);
    sdw32(h, SD_CBL, BUF_SIZE);
    sdw16(h, SD_LVI, NFRAGS - 1);
    sdw16(h, SD_FMT, h->fmt);
    sdw32(h, SD_BDPL, h->bdl_phys); sdw32(h, SD_BDPU, h->bdl_phys >> 32);
    sdw8(h, SD_CTL2, h->stream_tag << 4);
    sdw8(h, SD_CTL, SD_IOCE);
    h->last_lpib = 0; h->hw_pos = 0;
    sdw8(h, SD_CTL, SD_IOCE | SD_RUN);
    h->running = true;
}

static void stream_halt(struct hda *h) {   /* lock held */
    stream_reset(h);
    h->running = false;
    memset(h->buf, 0, BUF_SIZE);
    h->w_pos = h->hw_pos = 0; h->last_lpib = 0;
}

static void ring_zero(struct hda *h, uint64_t from, uint64_t n) {
    while (n) {
        size_t off = from % BUF_SIZE, k = MIN(n, (uint64_t)(BUF_SIZE - off));
        memset(h->buf + off, 0, k);
        from += k; n -= k;
    }
}

/* advance hw_pos from LPIB and silence what has been played (lock held) */
static void update_position(struct hda *h) {
    if (!h->running) return;
    uint32_t lpib = sd32(h, SD_LPIB) % BUF_SIZE;
    uint32_t delta = (lpib + BUF_SIZE - h->last_lpib) % BUF_SIZE;
    ring_zero(h, h->hw_pos, delta);
    h->hw_pos += delta; h->last_lpib = lpib;
}

static void hda_irq(struct regs *r, void *ctx) {
    struct hda *h = ctx;
    uint32_t sts = r32(h, INTSTS);
    if (!(sts & (1u << h->sd_index))) return;   /* shared line, not us */
    sdw8(h, SD_STS, SD_STS_ALL);
    h->irqs++;
    sem_post(&h->wake);
}

static void hda_service(void *arg) {
    struct hda *h = arg;
    for (;;) {
        sem_wait_timeout(&h->wake, 10);
        mutex_lock(&h->lock);
        update_position(h);
        if (h->running && h->w_pos <= h->hw_pos && time_ms() - h->idle_since > 500) stream_halt(h);
        mutex_unlock(&h->lock);
    }
}

/* ---- audio_ops ---- */
static int hda_set_rate(void *priv, uint32_t hz) {
    struct hda *h = priv;
    for (size_t i = 0; i < ARRAY_LEN(rate_table); i++) {
        if (rate_table[i].hz != hz) continue;
        if (!(h->rates & (1u << rate_table[i].bit))) return -EINVAL;
        mutex_lock(&h->lock);
        uint16_t fmt = rate_table[i].fmt | FMT_16BIT_STEREO;
        if (fmt != h->fmt) {
            if (h->running) stream_halt(h);
            h->fmt = fmt; h->rate = hz; h->dev.rate = hz;
            program_dacs(h);
        }
        mutex_unlock(&h->lock);
        return 0;
    }
    return -EINVAL;
}

static int64_t hda_write(void *priv, const int16_t *frames, size_t count, volatile bool *cancel) {
    struct hda *h = priv;
    const uint8_t *src = (const uint8_t *)frames;
    size_t bytes = count * 4, done = 0;
    mutex_lock(&h->lock);
    while (done < bytes && !(cancel && *cancel)) {
        update_position(h);
        if (h->running && h->w_pos < h->hw_pos + 512) {   /* ran dry: resume a fragment ahead of the DMA engine */
            if (h->w_pos < h->hw_pos) h->underruns++;
            h->w_pos = h->hw_pos + FRAG_SIZE;
        }
        uint64_t queued = h->w_pos - h->hw_pos;
        uint64_t space = queued + FRAG_SIZE < BUF_SIZE ? BUF_SIZE - FRAG_SIZE - queued : 0;
        if (space < 4) {
            if (!h->running) stream_start(h);
            mutex_unlock(&h->lock);
            thread_sleep_ms(5);
            mutex_lock(&h->lock);
            continue;
        }
        size_t n = MIN(space, bytes - done) & ~3ul;
        for (size_t left = n; left; ) {
            size_t off = (h->w_pos) % BUF_SIZE, k = MIN(left, (size_t)(BUF_SIZE - off));
            memcpy(h->buf + off, src + done, k);
            h->w_pos += k; done += k; left -= k;
        }
    }
    if (!h->running && h->w_pos > h->hw_pos) stream_start(h);
    h->idle_since = time_ms();
    mutex_unlock(&h->lock);
    return done / 4;
}

static void hda_drain(void *priv, volatile bool *cancel) {
    struct hda *h = priv;
    for (;;) {
        mutex_lock(&h->lock);
        update_position(h);
        bool done = !h->running || h->hw_pos >= h->w_pos;
        mutex_unlock(&h->lock);
        if (done || (cancel && *cancel)) return;
        thread_sleep_ms(10);
    }
}

static void hda_stop(void *priv) {
    struct hda *h = priv;
    mutex_lock(&h->lock);
    if (h->running) stream_halt(h);
    mutex_unlock(&h->lock);
}

static size_t hda_queued(void *priv) {
    struct hda *h = priv;
    mutex_lock(&h->lock);
    update_position(h);
    size_t q = h->running && h->w_pos > h->hw_pos ? (h->w_pos - h->hw_pos) / 4 : 0;
    mutex_unlock(&h->lock);
    return q;
}

static const struct audio_ops hda_ops = { hda_set_rate, hda_write, hda_drain, hda_stop, hda_queued };

static const char *codec_vendor(uint16_t v) {
    switch (v) {
    case 0x10EC: return "Realtek"; case 0x14F1: return "Conexant"; case 0x11D4: return "Analog Devices";
    case 0x111D: case 0x8384: return "IDT/SigmaTel"; case 0x1106: return "VIA"; case 0x1013: return "Cirrus Logic";
    case 0x434D: return "C-Media"; case 0x8086: return "Intel"; case 0x1002: return "AMD"; case 0x10DE: return "NVIDIA";
    case 0x1AF4: return "QEMU"; case 0x17E8: return "Chrontel"; case 0x1057: return "Motorola";
    default: return NULL;
    }
}

/* ---- probe ---- */
static bool hda_probe(struct pci_device *d) {
    struct hda *h = kzalloc(sizeof *h);
    h->pci = d;
    mutex_init(&h->lock);
    sem_init(&h->wake, 0);
    pci_enable_bus_master(d);
    if (d->vendor == 0x8086) pci_write32(d->bus, d->slot, d->func, 0x44, pci_read32(d->bus, d->slot, d->func, 0x44) & ~7u);   /* TCSEL: traffic class 0 */
    if (!d->bar_is_mmio[0] || !d->bar_addr[0]) { kfree(h); return false; }
    h->mmio = vmm_map_mmio(d->bar_addr[0], 0x4000);
    uint16_t gcap = r16(h, GCAP);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    h->dma64 = gcap & 1;
    if (!oss) { kprintf("hda: %02x:%02x.%d has no output streams\n", d->bus, d->slot, d->func); kfree(h); return false; }

    /* controller reset */
    w32(h, INTCTL, 0);
    w8(h, CORBCTL, 0); w8(h, RIRBCTL, 0);
    w16(h, STATESTS, 0x7FFF);
    w32(h, GCTL, r32(h, GCTL) & ~1u);
    for (int i = 0; i < 1000 && (r32(h, GCTL) & 1); i++) udelay(100);
    udelay(1000);
    w32(h, GCTL, r32(h, GCTL) | 1);
    for (int i = 0; i < 1000 && !(r32(h, GCTL) & 1); i++) udelay(100);
    if (!(r32(h, GCTL) & 1)) { kprintf("hda: controller stuck in reset\n"); kfree(h); return false; }
    uint16_t codecs = 0;
    for (int i = 0; i < 20 && !(codecs = r16(h, STATESTS) & 0x7FFF); i++) thread_sleep_ms(5);
    if (!codecs) { kprintf("hda: %02x:%02x.%d: no codecs\n", d->bus, d->slot, d->func); kfree(h); return false; }

    if (!rings_init(h)) h->immediate = true;
    for (int cad = 0; cad < 15; cad++) {
        if (!(codecs & (1 << cad))) continue;
        if (!h->immediate) {   /* first contact: if the rings stay silent, fall back to immediate commands */
            h->cad = cad;
            int before = h->timeouts;
            param(h, 0, P_VENDOR);
            if (h->timeouts > before) { kprintf("hda: CORB/RIRB unresponsive, using immediate commands\n"); h->immediate = true; }
        }
        if (probe_codec(h, cad)) break;
        h->cad = -1;
    }
    if (h->cad < 0 || !h->npaths) { kprintf("hda: %02x:%02x.%d: no analog output on any codec\n", d->bus, d->slot, d->func); kfree(h); return false; }

    /* stream: the first output descriptor, tag 1, 48 kHz to begin with */
    h->sd_index = iss;
    h->sd = h->mmio + 0x80 + iss * 0x20;
    h->stream_tag = 1;
    h->bdl_phys = dma_alloc(h, 1); h->bdl = P2V(h->bdl_phys);
    h->buf_phys = dma_alloc(h, BUF_SIZE / PAGE_SIZE); h->buf = P2V(h->buf_phys);
    if (!h->bdl_phys || !h->buf_phys) { kprintf("hda: no DMA memory%s\n", h->dma64 ? "" : " below 4 GiB"); return false; }
    for (int i = 0; i < NFRAGS; i++) { h->bdl[i].addr = h->buf_phys + i * FRAG_SIZE; h->bdl[i].len = FRAG_SIZE; h->bdl[i].flags = 1; }
    h->fmt = FMT_16BIT_STEREO; h->rate = 48000;
    program_dacs(h);
    stream_reset(h);

    char how[64];
    bool msi_ok = d->vendor != 0x1002 && d->vendor != 0x1022;   /* older AMD/ATI controllers mishandle MSI */
    if (pci_irq_install(d, hda_irq, h, msi_ok, how, sizeof how)) w32(h, INTCTL, (1u << 31) | (1u << h->sd_index));
    else kprintf("hda: no interrupt (%s), polling\n", how);
    thread_create(kernel_process, "hda", hda_service, h);

    const char *cv = codec_vendor(h->vendor >> 16);
    char outs[64] = ""; size_t ol = 0;
    for (int i = 0; i < h->npaths; i++) ol += snprintf(outs + ol, sizeof outs - ol, "%s%s", i ? ", " : "", pin_name(&h->w[h->paths[i].nid[0]]));
    snprintf(h->dev.name, sizeof h->dev.name, "Intel HD Audio (%04x:%04x)", d->vendor, d->device);
    if (cv) snprintf(h->dev.detail, sizeof h->dev.detail, "%s codec %04x, %s", cv, h->vendor & 0xFFFF, outs);
    else snprintf(h->dev.detail, sizeof h->dev.detail, "codec %08x, %s", h->vendor, outs);
    h->dev.ops = &hda_ops; h->dev.priv = h; h->dev.rate = h->rate;
    kprintf("hda: %02x:%02x.%d %s: %s; %d DAC%s, %s%s\n", d->bus, d->slot, d->func, h->dev.name, h->dev.detail, h->ndacs, h->ndacs == 1 ? "" : "s",
            h->immediate ? "immediate commands, " : "", how);
    audio_register(&h->dev);
    return true;
}

void hda_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->class_code == 0x04 && d->subclass == 0x03) hda_probe(d);
    }
}
