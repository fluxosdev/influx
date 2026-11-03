/*
 * Intel Haswell graphics display driver: HD Graphics 4600 and the rest of
 * the Gen7.5 family (HD 4200/4400/4600/5000, Iris 5100, Iris Pro 5200).
 *
 * Modelled on the Linux i915 driver (intel_display.c, i9xx_plane.c,
 * intel_cursor.c, intel_gmbus.c, intel_dp_aux.c, i915_gem_gtt.c) and the
 * Haswell PRM. The firmware (GOP on UEFI, the video BIOS on a BIOS boot)
 * lights the display; this driver takes over the scanout from there:
 *
 *   - finds the pipe whose primary plane shows the bootloader framebuffer
 *     (a linear 32bpp surface in the graphics aperture), the transcoder and
 *     DDI port behind it, and the timing's active size
 *   - double buffering: two scanout buffers in system memory, mapped into
 *     the GTT above the firmware's surfaces and written by the CPU through
 *     the write-combined aperture; a flip is a DSPSURF write, which the
 *     hardware latches at the next vblank
 *   - a 64x64 ARGB hardware cursor
 *   - the monitor's EDID over GMBUS (HDMI, DVI, VGA) or DisplayPort AUX
 *   - runtime resolution changes at or below the lit mode: the pipe keeps
 *     the timing the firmware trained the link for, the plane is resized
 *     and the panel fitter scales it up with the aspect ratio kept (what
 *     i915 does for non-native modes on fixed panels). New timings would
 *     need the DPLLs, DDI buffers and DP link training, which this driver
 *     does not program.
 *
 * The render engine (rings, contexts, the GPU's own page tables) is left
 * alone; OpenGL stays on the software rasteriser.
 */
#include "gpu.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "edid.h"
#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

/* ---- PCI configuration ---- */
#define PCI_GGC   0x50     /* GMCH graphics control: stolen memory and GTT size */
#define PCI_BDSM  0x5C     /* base of data stolen memory */

/* ---- registers ---- */
#define GFX_FLSH_CNTL        0x101008
#define GEN6_RP_STATE_CAP    0x145998
#define HSW_PWR_WELL_DRIVER  0x45404
#define PWR_WELL_REQ         (1u << 31)
#define PWR_WELL_STATE       (1u << 30)
#define ILK_DPFC_CONTROL     0x43208
#define DPFC_EN              (1u << 31)
#define PCH_RAWCLK_FREQ      0xC6204

#define TRANS_EDP 3
static uint32_t trans_base(int t) { return t == TRANS_EDP ? 0x6F000 : 0x60000 + (uint32_t)t * 0x1000; }
#define TRANS_HTOTAL(t)       (trans_base(t) + 0x000)
#define TRANS_VTOTAL(t)       (trans_base(t) + 0x00C)
#define TRANS_DDI_FUNC_CTL(t) (trans_base(t) + 0x400)
#define TRANS_DDI_ENABLE      (1u << 31)
enum { DDI_MODE_HDMI, DDI_MODE_DVI, DDI_MODE_DP, DDI_MODE_DP_MST, DDI_MODE_FDI };

#define PIPESRC(p)     (0x6001C + (p) * 0x1000)
#define PIPEDSL(p)     (0x70000 + (p) * 0x1000)
#define CURCNTR(p)     (0x70080 + (p) * 0x1000)
#define CURBASE(p)     (0x70084 + (p) * 0x1000)
#define CURPOS(p)      (0x70088 + (p) * 0x1000)
#define CUR_FBC_CTL(p) (0x700A0 + (p) * 0x1000)
#define DSPCNTR(p)     (0x70180 + (p) * 0x1000)
#define DSPSTRIDE(p)   (0x70188 + (p) * 0x1000)
#define DSPSURF(p)     (0x7019C + (p) * 0x1000)
#define DSPOFFSET(p)   (0x701A4 + (p) * 0x1000)
#define DSPSURFLIVE(p) (0x701AC + (p) * 0x1000)
#define PF_WIN_POS(p)  (0x68070 + (p) * 0x800)
#define PF_WIN_SZ(p)   (0x68074 + (p) * 0x800)
#define PF_CTL(p)      (0x68080 + (p) * 0x800)

#define DISP_ENABLE        (1u << 31)
#define DISP_GAMMA_ENABLE  (1u << 30)
#define DISP_FORMAT_SHIFT  26
#define DISP_FORMAT_BGRX8888 6u
#define DISP_CSC_ENABLE    (1u << 24)
#define DISP_TILED         (1u << 10)
#define CURSOR_MODE_64_ARGB 0x27u
#define CURSOR_GAMMA_ENABLE (1u << 26)
#define CURSOR_CSC_ENABLE   (1u << 24)
#define PF_ENABLE           (1u << 31)
#define PF_FILTER_MED_3x3   (1u << 23)
#define PF_PIPE_SEL(p)      ((uint32_t)(p) << 29)

/* GMBUS: the PCH's I2C controller for DDC */
#define GMBUS0 0xC5100     /* clock rate, pin pair */
#define GMBUS1 0xC5104     /* command */
#define GMBUS2 0xC5108     /* status */
#define GMBUS3 0xC510C     /* data */
#define GMBUS4 0xC5110     /* interrupt mask */
#define GMBUS_SW_CLR_INT   (1u << 31)
#define GMBUS_SW_RDY       (1u << 30)
#define GMBUS_CYCLE_WAIT   (1u << 25)
#define GMBUS_CYCLE_INDEX  (2u << 25)
#define GMBUS_CYCLE_STOP   (4u << 25)
#define GMBUS_READ         1u
#define GMBUS_HW_WAIT_PHASE (1u << 14)
#define GMBUS_HW_RDY       (1u << 11)
#define GMBUS_SATOER       (1u << 10)   /* the device did not acknowledge */
#define GMBUS_ACTIVE       (1u << 9)
enum { GMBUS_PIN_VGA = 2, GMBUS_PIN_DPC = 4, GMBUS_PIN_DPB = 5, GMBUS_PIN_DPD = 6 };

/* DisplayPort AUX channels of DDI B-D (in the PCH on Haswell) */
#define PCH_DP_AUX_CTL(port) (0xE4110 + ((port) - 1) * 0x100)
#define AUX_SEND_BUSY      (1u << 31)
#define AUX_DONE           (1u << 30)
#define AUX_TIME_OUT_ERROR (1u << 28)
#define AUX_RECEIVE_ERROR  (1u << 25)
#define AUX_SIZE_SHIFT     20
#define AUX_PRECHARGE(n)   ((uint32_t)(n) << 16)

struct igfx {
    struct gpu_device gpu;
    struct pci_device *pd;
    volatile uint32_t *mmio, *gtt;
    uint64_t gmadr, aper_len, gtt_size, stolen;
    int gt;
    int pipe, trans, port, port_mode;
    int native_w, native_h;          /* the timing's active area */
    int mode_w, mode_h, stride;      /* the plane (pipe source) */
    uint32_t dspcntr;
    uint64_t buf_gtt[2], cursor_gtt, buf_size;
    uint8_t *buf_cpu[2];
    uint32_t *cursor_cpu;
    int back;
    bool live_ok;                    /* DSPSURFLIVE tracks flips */
    uint32_t cursor_cntl;
    bool have_edid;
    struct edid_info edid;
    int aux_div;
    bool ult;                        /* ULT/ULX part with the LPT-LP PCH */
    bool can_scale;                  /* the panel fitter is in the pipe's data path */
    bool pf_on;                      /* the panel fitter is enabled (by us or the firmware) */
};
static struct igfx igdev;

static inline uint32_t rd(struct igfx *ig, uint32_t reg) { return ig->mmio[reg / 4]; }
static inline void wr(struct igfx *ig, uint32_t reg, uint32_t v) { ig->mmio[reg / 4] = v; }
static void udelay(uint32_t us) {
    uint64_t hz = cpu_tsc_hz() ? cpu_tsc_hz() : 2000000000ull;
    uint64_t end = rdtsc() + hz / 1000000 * us;
    while (rdtsc() < end) pause();
}
static inline void wc_flush(void) { __asm__ volatile("sfence" ::: "memory"); }

static const char *port_name(int p) { static const char *n[] = { "A", "B", "C", "D", "E" }; return p >= 0 && p < 5 ? n[p] : "?"; }
static const char *connector_name(struct igfx *ig) {
    if (ig->trans == TRANS_EDP) return "eDP";
    switch (ig->port_mode) {
    case DDI_MODE_HDMI: return "HDMI";
    case DDI_MODE_DVI: return "DVI";
    case DDI_MODE_DP: case DDI_MODE_DP_MST: return "DisplayPort";
    case DDI_MODE_FDI: return "VGA";
    }
    return "unknown";
}

/* ---- PCI helpers ---- */
/* size a memory BAR the usual way, with decoding off while it reads back the mask */
static uint64_t bar_size(struct pci_device *pd, int bar) {
    uint8_t off = 0x10 + bar * 4;
    uint64_t f = irq_save();
    uint16_t cmd = pci_read16(pd->bus, pd->slot, pd->func, 4);
    pci_write16(pd->bus, pd->slot, pd->func, 4, cmd & ~3u);
    uint32_t lo = pci_read32(pd->bus, pd->slot, pd->func, off);
    bool is64 = ((lo >> 1) & 3) == 2;
    uint32_t hi = is64 ? pci_read32(pd->bus, pd->slot, pd->func, off + 4) : 0;
    pci_write32(pd->bus, pd->slot, pd->func, off, 0xFFFFFFFF);
    uint32_t mlo = pci_read32(pd->bus, pd->slot, pd->func, off);
    pci_write32(pd->bus, pd->slot, pd->func, off, lo);
    uint32_t mhi = 0xFFFFFFFF;
    if (is64) {
        pci_write32(pd->bus, pd->slot, pd->func, off + 4, 0xFFFFFFFF);
        mhi = pci_read32(pd->bus, pd->slot, pd->func, off + 4);
        pci_write32(pd->bus, pd->slot, pd->func, off + 4, hi);
    }
    pci_write16(pd->bus, pd->slot, pd->func, 4, cmd);
    irq_restore(f);
    uint64_t mask = ((uint64_t)mhi << 32) | (mlo & ~0xFu);
    return mask ? ~mask + 1 : 0;
}

/* ---- GTT ---- */
/* point GTT entries at system memory pages (Haswell PTE: address bits 38:12, uncached, valid) */
static void gtt_map(struct igfx *ig, uint64_t gtt_off, uint64_t phys, uint64_t len) {
    for (uint64_t o = 0; o < len; o += PAGE_SIZE) {
        uint64_t a = phys + o;
        ig->gtt[(gtt_off + o) >> 12] = (uint32_t)((a & 0xFFFFF000u) | ((a >> 28) & 0x7F0) | 1);
    }
    (void)ig->gtt[(gtt_off + len - PAGE_SIZE) >> 12];   /* post the writes */
    wr(ig, GFX_FLSH_CNTL, 1);                             /* invalidate the GTT TLBs */
    (void)rd(ig, GFX_FLSH_CNTL);
}
static void clflush_range(const void *p, size_t len) {
    for (uintptr_t a = (uintptr_t)p & ~63ull; a < (uintptr_t)p + len; a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* ---- vblank and flips ---- */
static void wait_vblank_start(struct igfx *ig) {
    /* out of any vblank in progress, then into the next one (bounded: ~2 frames at 24 Hz) */
    int v = ig->native_h;
    for (int i = 0; i < 50000 && (int)(rd(ig, PIPEDSL(ig->pipe)) & 0x1FFF) >= v; i++) udelay(1);
    for (int i = 0; i < 50000 && (int)(rd(ig, PIPEDSL(ig->pipe)) & 0x1FFF) < v; i++) udelay(1);
}
/* the plane latches DSPSURF (and the stride and offset with it) at the next vblank */
static bool wait_flip(struct igfx *ig, uint32_t surf) {
    if (!ig->live_ok) { wait_vblank_start(ig); return true; }
    for (int i = 0; i < 50000; i++) {
        if ((rd(ig, DSPSURFLIVE(ig->pipe)) & ~0xFFFu) == surf) return true;
        udelay(1);
    }
    return false;
}

static uint8_t *ig_back_buffer(struct gpu_device *g, int *pitch) {
    struct igfx *ig = g->priv;
    *pitch = ig->stride;
    return ig->buf_cpu[ig->back];
}
static void ig_flip(struct gpu_device *g) {
    struct igfx *ig = g->priv;
    uint32_t surf = (uint32_t)ig->buf_gtt[ig->back];
    wc_flush();
    wr(ig, DSPSURF(ig->pipe), surf);
    wait_flip(ig, surf);              /* the old front buffer becomes the back buffer only once it is off screen */
    ig->back ^= 1;
}

/* ---- hardware cursor ---- */
static bool ig_set_cursor(struct gpu_device *g, const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    struct igfx *ig = g->priv;
    if (w > 64 || h > 64) return false;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint32_t p = (x < w && y < h) ? argb[y * w + x] : 0;
        uint32_t a = p >> 24;
        /* the cursor plane blends premultiplied ARGB */
        uint32_t r = ((p >> 16) & 0xFF) * a / 255, gg = ((p >> 8) & 0xFF) * a / 255, b = (p & 0xFF) * a / 255;
        ig->cursor_cpu[y * 64 + x] = (a << 24) | (r << 16) | (gg << 8) | b;
    }
    wc_flush();
    return true;
}
/* (x, y) is where the image's top-left corner goes */
static void ig_move_cursor(struct gpu_device *g, int x, int y, bool visible) {
    struct igfx *ig = g->priv;
    bool on = visible && x > -64 && y > -64 && x < ig->mode_w && y < ig->mode_h;
    uint32_t pos = (y < 0 ? (1u << 31) | ((uint32_t)-y << 16) : (uint32_t)y << 16) | (x < 0 ? (1u << 15) | (uint32_t)-x : (uint32_t)x);
    wr(ig, CUR_FBC_CTL(ig->pipe), 0);    /* square cursor: no cursor compression */
    wr(ig, CURCNTR(ig->pipe), on ? ig->cursor_cntl : 0);
    wr(ig, CURPOS(ig->pipe), pos);
    wr(ig, CURBASE(ig->pipe), on ? (uint32_t)ig->cursor_gtt : 0);   /* arms the update */
}

/* ---- EDID over GMBUS ---- */
static bool gmbus_wait(struct igfx *ig, uint32_t bit) {
    for (int i = 0; i < 5000; i++) {        /* 50 ms */
        uint32_t s = rd(ig, GMBUS2);
        if (s & GMBUS_SATOER) return false;
        if (s & bit) return true;
        udelay(10);
    }
    return false;
}
static bool gmbus_read(struct igfx *ig, int pin, uint8_t offset, uint8_t *buf, int len) {
    wr(ig, GMBUS4, 0);
    wr(ig, GMBUS0, (uint32_t)pin);         /* 100 kHz */
    wr(ig, GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT | ((uint32_t)len << 16) | ((uint32_t)offset << 8) | (0x50u << 1) | GMBUS_READ);
    bool ok = true;
    for (int i = 0; i < len && ok; ) {
        if (!gmbus_wait(ig, GMBUS_HW_RDY)) { ok = false; break; }
        uint32_t v = rd(ig, GMBUS3);
        for (int k = 0; k < 4 && i < len; k++, i++) buf[i] = (uint8_t)(v >> (8 * k));
    }
    if (ok) ok = gmbus_wait(ig, GMBUS_HW_WAIT_PHASE);
    if (ok) wr(ig, GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_STOP);
    for (int i = 0; i < 1000 && (rd(ig, GMBUS2) & GMBUS_ACTIVE); i++) udelay(10);
    if (!ok) { wr(ig, GMBUS1, GMBUS_SW_CLR_INT); wr(ig, GMBUS1, 0); }   /* clears the NAK */
    wr(ig, GMBUS0, 0);
    return ok;
}

/* ---- EDID over DisplayPort AUX (I2C-over-AUX to address 0x50) ---- */
static int aux_xfer(struct igfx *ig, int port, const uint8_t *tx, int txlen, uint8_t *rx, int rxmax) {
    uint32_t ctl = PCH_DP_AUX_CTL(port);
    int divs[2] = { ig->aux_div, ig->ult ? ig->aux_div : 72 };   /* 63, then 72: LPT-H workaround */
    for (int d = 0; d < 2; d++) {
        if (d && divs[1] == divs[0]) break;
        for (int attempt = 0; attempt < 5; attempt++) {
            for (int i = 0; i < 300 && (rd(ig, ctl) & AUX_SEND_BUSY); i++) udelay(10);
            for (int r = 0; r < 5; r++) {
                uint32_t v = 0;
                for (int k = 0; k < 4 && r * 4 + k < txlen; k++) v |= (uint32_t)tx[r * 4 + k] << (24 - 8 * k);
                wr(ig, ctl + 4 + r * 4, v);
            }
            wr(ig, ctl, AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR |
                        ((uint32_t)txlen << AUX_SIZE_SHIFT) | AUX_PRECHARGE(5) | (uint32_t)divs[d]);
            uint32_t st = 0;
            for (int i = 0; i < 1000; i++) { st = rd(ig, ctl); if (!(st & AUX_SEND_BUSY)) break; udelay(10); }
            wr(ig, ctl, st | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR);
            if (st & AUX_SEND_BUSY) continue;
            if (st & AUX_TIME_OUT_ERROR) continue;
            if (st & AUX_RECEIVE_ERROR) { udelay(450); continue; }
            if (!(st & AUX_DONE)) continue;
            int n = (st >> AUX_SIZE_SHIFT) & 0x1F;
            if (n < 1 || n > 20) continue;
            for (int i = 0; i < n && i < rxmax; i++) rx[i] = (uint8_t)(rd(ig, ctl + 4 + (i / 4) * 4) >> (24 - 8 * (i % 4)));
            ig->aux_div = divs[d];
            return n;
        }
    }
    return -1;
}
/* one I2C-over-AUX request; for reads returns the byte count (a sink may answer short), -1 on error */
#define AUX_I2C_WRITE 0x0
#define AUX_I2C_READ  0x1
#define AUX_I2C_MOT   0x4     /* middle of transaction: no stop afterwards */
static int aux_i2c(struct igfx *ig, int port, uint8_t req, uint8_t *buf, int len) {
    for (int tries = 0; tries < 16; tries++) {
        uint8_t tx[20], rx[20];
        tx[0] = (uint8_t)(req << 4); tx[1] = 0; tx[2] = 0x50;
        int txlen = 3;
        if (len) {
            tx[3] = (uint8_t)(len - 1); txlen = 4;
            if (!(req & AUX_I2C_READ)) { memcpy(tx + 4, buf, len); txlen += len; }
        }
        int n = aux_xfer(ig, port, tx, txlen, rx, sizeof rx);
        if (n < 1) return -1;
        int native = (rx[0] >> 4) & 3, i2c = (rx[0] >> 6) & 3;
        if (native == 2 || i2c == 2) { udelay(500); continue; }   /* DEFER */
        if (native || i2c) return -1;                              /* NACK */
        if (!(req & AUX_I2C_READ)) return len;
        if (n - 1 == 0 && len) { udelay(500); continue; }
        memcpy(buf, rx + 1, MIN(n - 1, len));
        return MIN(n - 1, len);
    }
    return -1;
}
static bool aux_read_edid(struct igfx *ig, int port, uint8_t offset, uint8_t *buf, int len) {
    bool ok = aux_i2c(ig, port, AUX_I2C_WRITE | AUX_I2C_MOT, &offset, 1) == 1;
    for (int got = 0; ok && got < len; ) {
        int n = aux_i2c(ig, port, AUX_I2C_READ | AUX_I2C_MOT, buf + got, MIN(16, len - got));
        if (n <= 0) ok = false; else got += n;
    }
    aux_i2c(ig, port, AUX_I2C_READ, NULL, 0);   /* address-only: stop */
    return ok;
}

static bool read_edid_block(struct igfx *ig, int pin, int aux_port, uint8_t offset, uint8_t *buf) {
    for (int tries = 0; tries < 3; tries++) {
        bool ok = aux_port ? aux_read_edid(ig, aux_port, offset, buf, 128) : gmbus_read(ig, pin, offset, buf, 128);
        if (ok && edid_block_valid(buf) && (offset || (buf[0] == 0 && buf[1] == 0xFF))) return true;
    }
    return false;
}
static void read_edid(struct igfx *ig) {
    if (ig->trans == TRANS_EDP) return;   /* a built-in panel: its one timing is the lit one */
    uint32_t raw = rd(ig, PCH_RAWCLK_FREQ) & 0x3FF;
    ig->aux_div = raw ? (int)(raw + 1) / 2 : 63;
    int pins[4], npins = 0, aux = 0;
    bool known = true;
    if (ig->port_mode == DDI_MODE_FDI) pins[npins++] = GMBUS_PIN_VGA;
    else if (ig->port_mode == DDI_MODE_DP || ig->port_mode == DDI_MODE_DP_MST) { if (ig->port >= 1 && ig->port <= 3) aux = ig->port; else known = false; }
    else if (ig->port_mode == DDI_MODE_HDMI || ig->port_mode == DDI_MODE_DVI) {
        static const int pin_of_port[] = { 0, GMBUS_PIN_DPB, GMBUS_PIN_DPC, GMBUS_PIN_DPD };
        if (ig->port >= 1 && ig->port <= 3) pins[npins++] = pin_of_port[ig->port]; else known = false;
    } else known = false;
    if (!known && ig->ult) { pins[0] = GMBUS_PIN_DPB; pins[1] = GMBUS_PIN_DPC; npins = 2; }   /* LPT-LP has no VGA or DPD pins */
    else if (!known) { pins[0] = GMBUS_PIN_VGA; pins[1] = GMBUS_PIN_DPB; pins[2] = GMBUS_PIN_DPC; pins[3] = GMBUS_PIN_DPD; npins = 4; }
    uint8_t *buf = kmalloc(256);
    if (!buf) return;
    for (int i = 0; i < (aux ? 1 : npins); i++) {
        if (!read_edid_block(ig, aux ? 0 : pins[i], aux, 0, buf)) continue;
        size_t len = 128;
        if (buf[126] && read_edid_block(ig, aux ? 0 : pins[i], aux, 128, buf + 128)) len = 256;
        if (edid_parse(buf, len, &ig->edid)) {
            ig->have_edid = true;
            if (aux) kprintf("i915: EDID over DP AUX %s (divider %d): '%s' (%s), %d timings\n", port_name(aux), ig->aux_div, ig->edid.name, ig->edid.vendor, ig->edid.ntimings);
            else kprintf("i915: EDID over GMBUS pin %d: '%s' (%s), %d timings\n", pins[i], ig->edid.name, ig->edid.vendor, ig->edid.ntimings);
            break;
        }
    }
    kfree(buf);
    if (!ig->have_edid) kprintf("i915: no EDID from the %s connector\n", connector_name(ig));
}

/* ---- modes: the lit timing, with the panel fitter for anything smaller ---- */
static bool mode_ok(struct igfx *ig, int w, int h);
static void add_mode(struct display_mode *out, int *n, int max, int w, int h) {
    if (*n >= max) return;
    for (int j = 0; j < *n; j++) if (out[j].w == w && out[j].h == h) return;
    out[*n].w = w; out[*n].h = h; (*n)++;
}
static int ig_list_modes(struct gpu_device *g, struct display_mode *out, int max) {
    struct igfx *ig = g->priv;
    static const struct display_mode common[] = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 } };
    int n = 0;
    add_mode(out, &n, max, ig->native_w, ig->native_h);
    if (ig->have_edid) {
        for (int i = 0; i < ig->edid.ntimings; i++) {
            const struct display_timing *t = &ig->edid.timings[i];
            if (t->interlace || t->hdisplay < 640 || !mode_ok(ig, t->hdisplay, t->vdisplay)) continue;
            add_mode(out, &n, max, t->hdisplay, t->vdisplay);
        }
    } else {
        for (size_t i = 0; i < ARRAY_LEN(common); i++)
            if (mode_ok(ig, common[i].w, common[i].h)) add_mode(out, &n, max, common[i].w, common[i].h);
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && (uint64_t)out[j].w * out[j].h < (uint64_t)out[j - 1].w * out[j - 1].h; j--) { struct display_mode t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

/* a mode the panel fitter can scale up to the lit timing (intel_pch_pfit_check_src_size) */
static bool mode_ok(struct igfx *ig, int w, int h) {
    if (w < 320 || h < 200 || w > ig->native_w || h > ig->native_h || (w & 1)) return false;
    if (w == ig->native_w && h == ig->native_h) return true;
    return ig->can_scale && w <= (ig->pipe == 0 ? 4096 : 2048);
}
/* the centred window with the mode's aspect ratio, as i915's pch_panel_fitting computes it */
static void pfit_window(int w, int h, int nw, int nh, int *x, int *y, int *ww, int *wh) {
    uint64_t sw = (uint64_t)nw * h, sh = (uint64_t)nh * w;
    *x = 0; *y = 0; *ww = nw; *wh = nh;
    if (sw > sh) {            /* pillarbox */
        *ww = (int)(sh / h); if (*ww & 1) (*ww)++;
        *x = (nw - *ww + 1) / 2;
    } else if (sw < sh) {     /* letterbox */
        *wh = (int)(sw / w); if (*wh & 1) (*wh)++;
        *y = (nh - *wh + 1) / 2;
    }
    if (*x == 1) { *x = 0; *ww = nw; }   /* "the X position must not be programmed to be 1" */
}

static bool power_well_on(struct igfx *ig) {
    uint32_t v = rd(ig, HSW_PWR_WELL_DRIVER);
    if (!(v & PWR_WELL_REQ)) wr(ig, HSW_PWR_WELL_DRIVER, v | PWR_WELL_REQ);
    for (int i = 0; i < 2000; i++) { if (rd(ig, HSW_PWR_WELL_DRIVER) & PWR_WELL_STATE) return true; udelay(10); }
    return false;
}

static bool ig_set_mode(struct gpu_device *g, int w, int h) {
    struct igfx *ig = g->priv;
    int nw = ig->native_w, nh = ig->native_h, p = ig->pipe;
    if (!mode_ok(ig, w, h)) return false;
    bool scale = w != nw || h != nh;
    int wx = 0, wy = 0, ww = nw, wh = nh;
    if (scale) {
        pfit_window(w, h, nw, nh, &wx, &wy, &ww, &wh);
        if (!power_well_on(ig)) { kprintf("i915: display power well did not come up; panel fitter unavailable\n"); return false; }
    }
    int stride = (int)ALIGN_UP((uint64_t)w * 4, 64);
    int front = ig->back ^ 1;
    /* black, so the first frame at the new stride shows nothing stale */
    memset(ig->buf_cpu[0], 0, ig->buf_size);
    memset(ig->buf_cpu[1], 0, ig->buf_size);
    wc_flush();
    wait_vblank_start(ig);
    uint64_t f = irq_save();
    wr(ig, PIPESRC(p), ((uint32_t)(w - 1) << 16) | (uint32_t)(h - 1));
    if (scale) {
        wr(ig, PF_CTL(p), PF_ENABLE | PF_FILTER_MED_3x3 | PF_PIPE_SEL(p));
        wr(ig, PF_WIN_POS(p), ((uint32_t)wx << 16) | (uint32_t)wy);
        wr(ig, PF_WIN_SZ(p), ((uint32_t)ww << 16) | (uint32_t)wh);
    } else if (ig->pf_on) {
        /* only touch the fitter when it is in use: on Haswell it may sit in an unpowered well */
        wr(ig, PF_CTL(p), 0);
        wr(ig, PF_WIN_POS(p), 0);
        wr(ig, PF_WIN_SZ(p), 0);
    }
    ig->pf_on = scale;
    wr(ig, DSPSTRIDE(p), (uint32_t)stride);
    wr(ig, DSPOFFSET(p), 0);
    wr(ig, DSPSURF(p), (uint32_t)ig->buf_gtt[front]);
    irq_restore(f);
    wait_flip(ig, (uint32_t)ig->buf_gtt[front]);
    ig->mode_w = w; ig->mode_h = h; ig->stride = stride;
    g->fb_phys = ig->gmadr + ig->buf_gtt[front]; g->fb_pitch = stride; g->fb_width = w; g->fb_height = h;
    if (scale) kprintf("i915: %dx%d scaled to %dx%d at +%d+%d on the %dx%d timing\n", w, h, ww, wh, wx, wy, nw, nh);
    else kprintf("i915: %dx%d (native timing, panel fitter off)\n", w, h);
    return true;
}

/* ---- probe ---- */
static bool is_haswell(uint16_t id) {
    uint8_t hi = id >> 8, lo = id & 0xFF, v = lo & 0xF;
    if (hi != 0x04 && hi != 0x0A && hi != 0x0C && hi != 0x0D) return false;
    if ((lo & 0xC0) || ((lo >> 4) & 3) == 3) return false;
    return v == 0x2 || v == 0x6 || v == 0xA || v == 0xB || v == 0xE;
}
static const struct { uint16_t id; const char *name; } models[] = {
    { 0x0402, "HD Graphics" }, { 0x0406, "HD Graphics" }, { 0x040A, "HD Graphics" }, { 0x0A06, "HD Graphics" }, { 0x0A0E, "HD Graphics" },
    { 0x0412, "HD Graphics 4600" }, { 0x0416, "HD Graphics 4600" }, { 0x041A, "HD Graphics P4600/P4700" },
    { 0x041E, "HD Graphics 4400" }, { 0x0A16, "HD Graphics 4400" }, { 0x0A1E, "HD Graphics 4200" },
    { 0x0A26, "HD Graphics 5000" }, { 0x0A2E, "Iris Graphics 5100" }, { 0x0D22, "Iris Pro Graphics 5200" }, { 0x0D26, "Iris Pro Graphics 5200" },
};

static bool find_scanout(struct igfx *ig, uint64_t *fw_end) {
    struct limine_framebuffer *fb = framebuffer_request.response && framebuffer_request.response->framebuffer_count ? framebuffer_request.response->framebuffers[0] : NULL;
    uint64_t fb_phys = fb ? (uint64_t)fb->address - hhdm_offset : 0;
    ig->pipe = -1; *fw_end = 0;
    for (int p = 0; p < 3; p++) {
        uint32_t cntr = rd(ig, DSPCNTR(p));
        if (!(cntr & DISP_ENABLE)) continue;
        uint32_t surf = rd(ig, DSPSURF(p)) & ~0xFFFu, stride = rd(ig, DSPSTRIDE(p)), off = rd(ig, DSPOFFSET(p)), src = rd(ig, PIPESRC(p));
        int w = (int)((src >> 16) & 0x1FFF) + 1, h = (int)(src & 0xFFF) + 1;
        uint64_t start = surf + (uint64_t)(off >> 16) * stride + (uint64_t)(off & 0xFFFF) * 4;
        uint32_t fmt = (cntr >> DISP_FORMAT_SHIFT) & 0xF;
        *fw_end = MAX(*fw_end, (uint64_t)surf + (uint64_t)stride * (h + (off >> 16)));
        uint32_t cur = rd(ig, CURBASE(p));
        if (cur) *fw_end = MAX(*fw_end, (uint64_t)cur + 0x4000);
        kprintf("i915: pipe %c plane on: %dx%d, stride %u, surface %08x+%x, format %u%s\n", 'A' + p, w, h, stride, surf, (uint32_t)(start - surf), fmt, cntr & DISP_TILED ? ", X-tiled" : "");
        bool match = fb && fb_phys == ig->gmadr + start && fb->pitch == stride && (int)fb->width == w && (int)fb->height == h &&
                     fmt == DISP_FORMAT_BGRX8888 && !(cntr & DISP_TILED) && fb->bpp == 32;
        if (match && ig->pipe < 0) { ig->pipe = p; ig->dspcntr = cntr; ig->mode_w = w; ig->mode_h = h; ig->stride = (int)stride; }
    }
    return ig->pipe >= 0;
}

static void find_output(struct igfx *ig) {
    int p = ig->pipe;
    ig->trans = p; ig->port = -1; ig->port_mode = -1; ig->can_scale = true;
    uint32_t edp = rd(ig, TRANS_DDI_FUNC_CTL(TRANS_EDP));
    if (edp & TRANS_DDI_ENABLE) {
        int in = (edp >> 12) & 7;
        int edp_pipe = (in == 0 || in == 4) ? 0 : in == 5 ? 1 : in == 6 ? 2 : -1;
        if (edp_pipe == p) {
            ig->trans = TRANS_EDP; ig->port = 0; ig->port_mode = DDI_MODE_DP;
            /* pipe A feeds the eDP transcoder either straight (A_ON) or through the panel fitter
             * (A_ONOFF); switching the mux takes a full mode set, so with A_ON there is no scaling */
            if (in == 0) ig->can_scale = false;
        }
    }
    if (ig->trans != TRANS_EDP) {
        uint32_t ddi = rd(ig, TRANS_DDI_FUNC_CTL(ig->trans));
        if (ddi & TRANS_DDI_ENABLE) { ig->port = (ddi >> 28) & 7; ig->port_mode = (ddi >> 24) & 7; }
    }
    ig->native_w = (int)(rd(ig, TRANS_HTOTAL(ig->trans)) & 0x1FFF) + 1;
    ig->native_h = (int)(rd(ig, TRANS_VTOTAL(ig->trans)) & 0x1FFF) + 1;
    if (ig->native_w < ig->mode_w || ig->native_h < ig->mode_h || ig->native_w > 8192 || ig->native_h > 8192) { ig->native_w = ig->mode_w; ig->native_h = ig->mode_h; }
    uint32_t pf = rd(ig, PF_CTL(p));
    ig->pf_on = pf & PF_ENABLE;
    char tn[4] = "EDP";
    if (ig->trans != TRANS_EDP) { tn[0] = (char)('A' + ig->trans); tn[1] = 0; }
    kprintf("i915: pipe %c -> transcoder %s -> DDI %s (%s), timing %dx%d%s\n", 'A' + p, tn,
            port_name(ig->port), connector_name(ig), ig->native_w, ig->native_h, pf & PF_ENABLE ? ", panel fitter on" : "");
    if (!ig->can_scale) kprintf("i915: eDP transcoder bypasses the panel fitter; the resolution can only change at boot\n");
}

/* scanout buffers and the cursor in system memory, mapped at the top of the aperture */
static bool setup_buffers(struct igfx *ig, uint64_t fw_end) {
    uint64_t native_stride = ALIGN_UP((uint64_t)ig->native_w * 4, 64);
    ig->buf_size = ALIGN_UP(native_stride * ig->native_h, 0x10000);
    uint64_t total = 2 * ig->buf_size + 0x10000;
    uint64_t space = MIN(ig->aper_len, ig->gtt_size / 4 * PAGE_SIZE);
    fw_end = ALIGN_UP(fw_end, 0x10000);
    if (fw_end + total > space) { kprintf("i915: aperture too small for two %dx%d buffers (%lu MiB, firmware surfaces end at %lx)\n", ig->native_w, ig->native_h, space >> 20, fw_end); return false; }
    /* at the top, away from whatever the firmware placed low in the GTT */
    uint64_t base = ALIGN_DOWN(space - total, 0x100000);
    if (base < fw_end) base = fw_end;
    uint64_t phys = pmm_alloc_pages(total / PAGE_SIZE);
    if (!phys) { kprintf("i915: out of memory for scanout buffers\n"); return false; }
    if (phys + total > (1ull << 39)) { pmm_free_pages(phys, total / PAGE_SIZE); kprintf("i915: scanout memory above 512 GiB\n"); return false; }
    /* the pages were zeroed through the cache; the display reads memory directly */
    clflush_range(P2V(phys), total);
    gtt_map(ig, base, phys, total);
    uint8_t *cpu = vmm_map_wc(ig->gmadr + base, total);
    ig->buf_gtt[0] = base; ig->buf_gtt[1] = base + ig->buf_size; ig->cursor_gtt = base + 2 * ig->buf_size;
    ig->buf_cpu[0] = cpu; ig->buf_cpu[1] = cpu + ig->buf_size; ig->cursor_cpu = (uint32_t *)(cpu + 2 * ig->buf_size);
    kprintf("i915: scanout buffers at GTT %lx (%lu KiB each), system memory %lx\n", base, ig->buf_size >> 10, phys);
    return true;
}

/* move the scanout from the firmware's surface to buffer 0, carrying the picture over */
static bool take_over_scanout(struct igfx *ig) {
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    int p = ig->pipe, stride = (int)ALIGN_UP((uint64_t)ig->mode_w * 4, 64);
    const uint8_t *src = fb->address;
    for (int y = 0; y < ig->mode_h; y++) memcpy(ig->buf_cpu[0] + (size_t)y * stride, src + (size_t)y * fb->pitch, (size_t)ig->mode_w * 4);
    memcpy(ig->buf_cpu[1], ig->buf_cpu[0], (size_t)stride * ig->mode_h);
    wc_flush();
    uint32_t old_surf = rd(ig, DSPSURF(p)), old_stride = rd(ig, DSPSTRIDE(p)), old_off = rd(ig, DSPOFFSET(p));
    uint32_t surf = (uint32_t)ig->buf_gtt[0];
    if (rd(ig, ILK_DPFC_CONTROL) & DPFC_EN) { wr(ig, ILK_DPFC_CONTROL, rd(ig, ILK_DPFC_CONTROL) & ~DPFC_EN); kprintf("i915: frame buffer compression disabled\n"); }
    wr(ig, DSPSTRIDE(p), (uint32_t)stride);
    wr(ig, DSPOFFSET(p), 0);
    wr(ig, DSPSURF(p), surf);
    ig->live_ok = true;
    if (!wait_flip(ig, surf)) {
        if ((rd(ig, DSPSURF(p)) & ~0xFFFu) != surf) {
            kprintf("i915: plane did not take the new surface; restoring the firmware's\n");
            wr(ig, DSPSTRIDE(p), old_stride); wr(ig, DSPOFFSET(p), old_off); wr(ig, DSPSURF(p), old_surf);
            return false;
        }
        ig->live_ok = false;
        kprintf("i915: DSPSURFLIVE does not follow flips; timing them by scanline\n");
    }
    ig->stride = stride; ig->back = 1;
    return true;
}

static bool probe(struct pci_device *pd) {
    struct igfx *ig = &igdev;
    memset(ig, 0, sizeof *ig);
    struct gpu_device *g = &ig->gpu;
    g->priv = ig; ig->pd = pd;
    ig->gt = ((pd->device >> 4) & 3) + 1;
    ig->ult = (pd->device >> 8) == 0x0A;
    const char *model = NULL;
    for (size_t i = 0; i < ARRAY_LEN(models); i++) if (models[i].id == pd->device) model = models[i].name;
    if (model) snprintf(g->name, sizeof g->name, "Intel %s", model);
    else snprintf(g->name, sizeof g->name, "Intel Haswell GT%d [%04x]", ig->gt, pd->device);
    strcpy(g->driver, "i915");
    snprintf(g->chip, sizeof g->chip, "Haswell GT%d", ig->gt);
    snprintf(g->bus, sizeof g->bus, "PCI %02x:%02x.%d", pd->bus, pd->slot, pd->func);
    g->stream_processors = ig->gt == 1 ? 10 : ig->gt == 2 ? 20 : 40;   /* execution units (the full configuration; fuses may disable some) */

    /* BAR0: registers (2 MiB) + GTT (2 MiB); BAR2: the graphics aperture */
    if (!pd->bar_is_mmio[0] || !pd->bar_addr[0] || !pd->bar_is_mmio[2] || !pd->bar_addr[2]) { kprintf("i915: %s: BARs not assigned\n", g->name); return false; }
    uint64_t mmio_bar = pd->bar_addr[0];
    ig->gmadr = pd->bar_addr[2];
    ig->aper_len = bar_size(pd, 2);
    pci_enable_bus_master(pd);
    uint16_t ggc = pci_read16(pd->bus, pd->slot, pd->func, PCI_GGC);
    ig->stolen = (uint64_t)((ggc >> 3) & 0x1F) * (32ull << 20);
    ig->gtt_size = (uint64_t)((ggc >> 8) & 3) << 20;
    uint32_t bdsm = pci_read32(pd->bus, pd->slot, pd->func, PCI_BDSM) & 0xFFF00000u;
    if (!ig->gtt_size) ig->gtt_size = 2ull << 20;
    ig->mmio = vmm_map_mmio(mmio_bar, 2ull << 20);
    ig->gtt = vmm_map_mmio(mmio_bar + (2ull << 20), ig->gtt_size);
    g->mmio_base = mmio_bar; g->vram_base = ig->gmadr; g->vram_bytes = ig->stolen;
    uint32_t rp = rd(ig, GEN6_RP_STATE_CAP);
    if ((rp & 0xFF) && (rp & 0xFF) < 40) g->engine_clock_khz = (rp & 0xFF) * 50000;
    kprintf("i915: %s (%s, %d EUs) at %s, registers %lx, aperture %lx (%lu MiB), stolen %lu MiB at %x, GTT %lu KiB\n", g->name, g->chip,
            g->stream_processors, g->bus, mmio_bar, ig->gmadr, ig->aper_len >> 20, ig->stolen >> 20, bdsm, ig->gtt_size >> 10);
    if (g->engine_clock_khz) kprintf("i915: render clock up to %u MHz\n", g->engine_clock_khz / 1000);

    uint64_t fw_end;
    if (!find_scanout(ig, &fw_end)) {
        kprintf("i915: the bootloader framebuffer is not a linear 32bpp plane in the aperture; using the software path\n");
        gpu_register(g);
        return true;
    }
    g->scanout_verified = true;
    find_output(ig);
    read_edid(ig);
    if (setup_buffers(ig, fw_end) && take_over_scanout(ig)) {
        int p = ig->pipe;
        ig->cursor_cntl = CURSOR_MODE_64_ARGB | (ig->dspcntr & DISP_GAMMA_ENABLE ? CURSOR_GAMMA_ENABLE : 0) | (ig->dspcntr & DISP_CSC_ENABLE ? CURSOR_CSC_ENABLE : 0);
        wr(ig, CURCNTR(p), 0); wr(ig, CURBASE(p), 0);
        g->fb_phys = ig->gmadr + ig->buf_gtt[0]; g->fb_pitch = ig->stride; g->fb_width = ig->mode_w; g->fb_height = ig->mode_h;
        g->page_flip = true; g->back_buffer = ig_back_buffer; g->flip = ig_flip;
        g->hw_cursor = true; g->set_cursor = ig_set_cursor; g->move_cursor = ig_move_cursor;
        g->list_modes = ig_list_modes;
        if (ig->can_scale) { g->modeset = true; g->set_mode = ig_set_mode; }
        kprintf("i915: scanout taken over on pipe %c; page flipping, hardware cursor%s enabled\n", 'A' + p, ig->can_scale ? " and panel-fitter mode setting" : "");
    } else {
        kprintf("i915: keeping the firmware's scanout (software path)\n");
    }
    gpu_register(g);
    return true;
}

void intel_gfx_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->vendor != 0x8086 || pd->class_code != 0x03) continue;
        if (!is_haswell(pd->device)) { kprintf("i915: Intel graphics %04x is not a Haswell part; software path\n", pd->device); continue; }
        if (probe(pd)) return;
    }
}
