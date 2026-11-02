/*
 * ATI Radeon R700 (HD 4000 series) display driver.
 *
 * Modelled on the Linux "radeon" driver (rv770.c, r600.c, avivod.h,
 * radeon_cursor.c, atombios.h). Supports RV770/RV790 (HD 4800), RV740
 * (HD 4700), RV730 (HD 4600), RV710 (HD 4300/4500) and the RS880 IGP.
 *
 * What it does without any firmware:
 *   - maps the register BAR and the VRAM aperture, reads the VRAM size and
 *     the MC framebuffer location
 *   - inspects the scanout the video BIOS (BIOS boot through VBE) or the
 *     GOP (UEFI boot) configured, and verifies it is the bootloader's
 *     framebuffer
 *   - parses the AtomBIOS ROM: bootup message, part number, default clocks
 *   - provides a 64x64 ARGB hardware cursor
 *   - provides vblank-synchronised double-buffered page flipping of the
 *     primary surface (the compositor renders into the VRAM back buffer)
 *
 * The 3D engine (command processor + shaders) needs the R700 CP microcode
 * and a full command-stream backend; rendering therefore uses the software
 * rasteriser, which is the fallback for every other adapter as well.
 */
#include "gpu.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

/* ---- registers ---- */
#define MM_INDEX                 0x0000
#define MM_DATA                  0x0004
#define VGA_RENDER_CONTROL       0x0300
#define D1VGA_CONTROL            0x0330
#define R700_MC_VM_FB_LOCATION   0x2024
#define R600_MC_VM_FB_LOCATION   0x2180
#define HDP_NONSURFACE_BASE      0x2C04
#define CONFIG_MEMSIZE           0x5428
#define GRBM_STATUS              0x8010
#define CC_GC_SHADER_PIPE_CONFIG 0x8950
#define D1CRTC_H_TOTAL           0x6000
#define D1CRTC_V_TOTAL           0x6020
#define D1CRTC_CONTROL           0x6080
#define D1CRTC_STATUS            0x609C
#define D1CRTC_STATUS_POSITION   0x60A0
#define D1GRPH_ENABLE            0x6100
#define D1GRPH_CONTROL           0x6104
#define D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define D1GRPH_PITCH             0x6120
#define D1GRPH_X_END             0x6134
#define D1GRPH_Y_END             0x6138
#define D1GRPH_UPDATE            0x6144
#define D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH   0x6914
#define D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH 0x691C
#define D1CUR_CONTROL            0x6400
#define D1CUR_SURFACE_ADDRESS    0x6408
#define D1CUR_SIZE               0x6410
#define D1CUR_POSITION           0x6414
#define D1CUR_HOT_SPOT           0x6418
#define D1CUR_UPDATE             0x6424
#define D1MODE_VIEWPORT_SIZE     0x6584
#define CRTC2_OFFSET             0x0800
#define GRPH_UPDATE_LOCK         (1u << 16)
#define GRPH_SURFACE_UPDATE_PENDING (1u << 2)
#define CRTC_V_BLANK             (1u << 0)
#define CURSOR_EN                (1u << 0)
#define CURSOR_MODE_SHIFT        8
#define CURSOR_MODE_ARGB_PREMUL  2
#define CURSOR_UPDATE_LOCK       (1u << 16)

struct chip_info { uint16_t lo, hi; const char *chip; int sps; };
static const struct chip_info chips[] = {
    { 0x9440, 0x945F, "RV770", 800 }, { 0x9460, 0x9462, "RV790", 800 },
    { 0x9480, 0x949F, "RV730", 320 }, { 0x94A0, 0x94B9, "RV740", 640 },
    { 0x9540, 0x955F, "RV710", 80 },  { 0x9710, 0x9715, "RS880", 40 },
};
struct model { uint16_t id; const char *name; };
static const struct model models[] = {
    { 0x9440, "Radeon HD 4870" }, { 0x9441, "Radeon HD 4870 X2" }, { 0x9442, "Radeon HD 4850" }, { 0x9443, "Radeon HD 4850 X2" },
    { 0x944C, "Radeon HD 4830" }, { 0x944E, "Radeon HD 4730" }, { 0x9450, "FireStream 9270" }, { 0x9452, "FireStream 9250" },
    { 0x9456, "FirePro V8700" }, { 0x945A, "Mobility Radeon HD 4870" }, { 0x9460, "Radeon HD 4890" }, { 0x9462, "Radeon HD 4860" },
    { 0x9480, "Mobility Radeon HD 4650" }, { 0x9488, "Mobility Radeon HD 4670" }, { 0x9490, "Radeon HD 4670" }, { 0x9491, "Radeon E4690" },
    { 0x9495, "Radeon HD 4600 AGP" }, { 0x9498, "Radeon HD 4650" }, { 0x949C, "FirePro V5700" }, { 0x949E, "FirePro V3700" }, { 0x949F, "FirePro V3750" },
    { 0x94A0, "Mobility Radeon HD 4830" }, { 0x94A1, "Mobility Radeon HD 4860" }, { 0x94B3, "Radeon HD 4770" }, { 0x94B4, "Radeon HD 4700" }, { 0x94B5, "Radeon HD 4770" },
    { 0x9540, "Radeon HD 4550" }, { 0x954F, "Radeon HD 4350" }, { 0x9552, "Mobility Radeon HD 4330" }, { 0x9553, "Mobility Radeon HD 4500" },
    { 0x9555, "Mobility Radeon HD 4350" }, { 0x9557, "FirePro RG220" }, { 0x955F, "Mobility Radeon HD 4330" },
    { 0x9710, "Radeon HD 4200" }, { 0x9712, "Mobility Radeon HD 4200" }, { 0x9713, "Mobility Radeon HD 4100" }, { 0x9714, "Radeon HD 4290" }, { 0x9715, "Radeon HD 4250" },
};

struct radeon {
    struct gpu_device gpu;
    volatile uint32_t *mmio; uint64_t mmio_phys, mmio_len;
    uint8_t *vram; uint64_t vram_phys, aper_len;
    uint64_t fb_start_gpu;               /* MC address of VRAM start */
    uint32_t scan_gpu_addr, pitch_px, width, height;
    uint64_t scan_off;                   /* VRAM offset of the bootloader framebuffer */
    uint64_t buf_off[2]; int back;       /* page-flip buffers (VRAM offsets); back = index being drawn */
    uint64_t cursor_off;
    bool is_rs880, igp_vram_unknown;
    int crtc;                            /* which CRTC scans out the framebuffer */
};
static struct radeon rdev;

static inline uint32_t rreg(struct radeon *r, uint32_t reg) {
    if (reg < r->mmio_len) return r->mmio[reg / 4];
    r->mmio[MM_INDEX / 4] = reg; return r->mmio[MM_DATA / 4];
}
static inline void wreg(struct radeon *r, uint32_t reg, uint32_t v) {
    if (reg < r->mmio_len) { r->mmio[reg / 4] = v; return; }
    r->mmio[MM_INDEX / 4] = reg; r->mmio[MM_DATA / 4] = v;
}
static void udelay(uint32_t us) { uint64_t t = rdtsc() + (uint64_t)us * (cpu_tsc_hz() ? cpu_tsc_hz() / 1000000 : 2000); while (rdtsc() < t) pause(); }

/* ---- AtomBIOS ROM ---- */
static void parse_atombios(struct radeon *r) {
    const uint8_t *rom = P2V(0xC0000);   /* legacy option ROM shadow, present after a BIOS boot */
    if (rom[0] != 0x55 || rom[1] != 0xAA) { kprintf("radeon: no video BIOS shadow at C000:0000 (UEFI boot?)\n"); return; }
    size_t rom_size = (size_t)rom[2] * 512; if (rom_size > 0x20000) rom_size = 0x20000;
    uint16_t hdr = *(const uint16_t *)(rom + 0x48);
    if (hdr + 36 > rom_size || memcmp(rom + hdr + 4, "ATOM", 4)) { kprintf("radeon: video BIOS is not AtomBIOS\n"); return; }
    uint16_t msg = *(const uint16_t *)(rom + hdr + 16);
    if (msg && msg < rom_size) {
        const char *s = (const char *)rom + msg; size_t i = 0;
        while (*s == '\r' || *s == '\n') s++;
        while (s[i] && s[i] != '\n' && s[i] != '\r' && i < sizeof r->gpu.bios_message - 1 && msg + (s - (const char *)rom) + i < rom_size) { r->gpu.bios_message[i] = s[i]; i++; }
        r->gpu.bios_message[i] = 0;
        while (i > 0 && r->gpu.bios_message[i - 1] == ' ') r->gpu.bios_message[--i] = 0;
    }
    /* part numbers look like "113-xxxxxxx-xxx" */
    for (size_t i = 0; i + 16 < rom_size; i++) if (rom[i] == '1' && rom[i + 1] == '1' && rom[i + 2] == '3' && rom[i + 3] == '-' && isalnum(rom[i + 4])) {
        size_t n = 0; while (n < sizeof r->gpu.bios_part - 1 && isprint(rom[i + n]) && rom[i + n] != ' ') { r->gpu.bios_part[n] = rom[i + n]; n++; }
        r->gpu.bios_part[n] = 0; break;
    }
    uint16_t master = *(const uint16_t *)(rom + hdr + 32);
    if (master && master + 12 + 2 <= rom_size) {
        uint16_t fwinfo = *(const uint16_t *)(rom + master + 4 + 4 * 2);   /* FirmwareInfo is data table #4 */
        if (fwinfo && fwinfo + 16 <= rom_size) {
            r->gpu.engine_clock_khz = *(const uint32_t *)(rom + fwinfo + 8) * 10;
            r->gpu.memory_clock_khz = *(const uint32_t *)(rom + fwinfo + 12) * 10;
        }
    }
    kprintf("radeon: AtomBIOS '%s' %s, engine %u MHz, memory %u MHz\n", r->gpu.bios_message, r->gpu.bios_part, r->gpu.engine_clock_khz / 1000, r->gpu.memory_clock_khz / 1000);
}

/* ---- scanout / page flip ---- */
static uint32_t crtc_reg(struct radeon *r, uint32_t reg) { return reg + (r->crtc ? CRTC2_OFFSET : 0); }

static void wait_vblank(struct radeon *r) {
    /* wait for the start of the next vertical blank (bounded) */
    for (int i = 0; i < 40000 && (rreg(r, crtc_reg(r, D1CRTC_STATUS)) & CRTC_V_BLANK); i++) udelay(1);
    for (int i = 0; i < 40000 && !(rreg(r, crtc_reg(r, D1CRTC_STATUS)) & CRTC_V_BLANK); i++) udelay(1);
}
static void set_primary_surface(struct radeon *r, uint32_t gpu_addr) {
    uint32_t upd = crtc_reg(r, D1GRPH_UPDATE);
    uint32_t tmp = rreg(r, upd);
    wreg(r, upd, tmp | GRPH_UPDATE_LOCK);
    wreg(r, crtc_reg(r, D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH), 0);
    wreg(r, crtc_reg(r, D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH), 0);
    wreg(r, crtc_reg(r, D1GRPH_SECONDARY_SURFACE_ADDRESS), gpu_addr);
    wreg(r, crtc_reg(r, D1GRPH_PRIMARY_SURFACE_ADDRESS), gpu_addr);
    for (int i = 0; i < 10000 && !(rreg(r, upd) & GRPH_SURFACE_UPDATE_PENDING); i++) udelay(1);
    wreg(r, upd, tmp & ~GRPH_UPDATE_LOCK);
    /* the new surface becomes visible at the next vblank */
    for (int i = 0; i < 40000 && (rreg(r, upd) & GRPH_SURFACE_UPDATE_PENDING); i++) udelay(1);
}
static uint8_t *radeon_back_buffer(struct gpu_device *g, int *pitch) {
    struct radeon *r = g->priv;
    *pitch = r->pitch_px * 4;
    return r->vram + r->buf_off[r->back];
}
static void radeon_flip(struct gpu_device *g) {
    struct radeon *r = g->priv;
    set_primary_surface(r, (uint32_t)(r->fb_start_gpu + r->buf_off[r->back]));
    r->back ^= 1;
}

/* ---- hardware cursor ---- */
static bool radeon_set_cursor(struct gpu_device *g, const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    struct radeon *r = g->priv;
    if (w > 64 || h > 64) return false;
    uint32_t *dst = (uint32_t *)(r->vram + r->cursor_off);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint32_t p = (x < w && y < h) ? argb[y * w + x] : 0;
        uint32_t a = p >> 24;
        /* the AVIVO cursor expects premultiplied alpha */
        uint32_t rr = ((p >> 16) & 0xFF) * a / 255, gg = ((p >> 8) & 0xFF) * a / 255, bb = (p & 0xFF) * a / 255;
        dst[y * 64 + x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }
    wreg(r, crtc_reg(r, D1CUR_UPDATE), CURSOR_UPDATE_LOCK);
    wreg(r, crtc_reg(r, D1CUR_SURFACE_ADDRESS), (uint32_t)(r->fb_start_gpu + r->cursor_off));
    wreg(r, crtc_reg(r, D1CUR_SIZE), (63u << 16) | 63u);
    wreg(r, crtc_reg(r, D1CUR_HOT_SPOT), ((uint32_t)hot_x << 16) | (uint32_t)hot_y);
    wreg(r, crtc_reg(r, D1CUR_CONTROL), CURSOR_EN | (CURSOR_MODE_ARGB_PREMUL << CURSOR_MODE_SHIFT));
    wreg(r, crtc_reg(r, D1CUR_UPDATE), 0);
    return true;
}
static void radeon_move_cursor(struct gpu_device *g, int x, int y, bool visible) {
    struct radeon *r = g->priv;
    if (x < 0) x = 0; if (y < 0) y = 0;
    wreg(r, crtc_reg(r, D1CUR_UPDATE), CURSOR_UPDATE_LOCK);
    wreg(r, crtc_reg(r, D1CUR_POSITION), ((uint32_t)x << 16) | (uint32_t)y);
    wreg(r, crtc_reg(r, D1CUR_CONTROL), (visible ? CURSOR_EN : 0) | (CURSOR_MODE_ARGB_PREMUL << CURSOR_MODE_SHIFT));
    wreg(r, crtc_reg(r, D1CUR_UPDATE), 0);
}

/* ---- probe ---- */
static bool probe(struct pci_device *pd, const struct chip_info *ci) {
    struct radeon *r = &rdev;
    memset(r, 0, sizeof *r);
    r->gpu.priv = r;
    strcpy(r->gpu.driver, "radeon");
    strlcpy(r->gpu.chip, ci->chip, sizeof r->gpu.chip);
    r->gpu.stream_processors = ci->sps;
    const char *model = NULL;
    for (size_t i = 0; i < ARRAY_LEN(models); i++) if (models[i].id == pd->device) model = models[i].name;
    if (model) snprintf(r->gpu.name, sizeof r->gpu.name, "ATI %s", model);
    else snprintf(r->gpu.name, sizeof r->gpu.name, "ATI %s [%04x]", ci->chip, pd->device);
    snprintf(r->gpu.bus, sizeof r->gpu.bus, "PCI %02x:%02x.%d", pd->bus, pd->slot, pd->func);
    r->is_rs880 = !strcmp(ci->chip, "RS880");
    /* BAR0: VRAM aperture (prefetchable), BAR2: registers (64 KiB) */
    uint64_t vram_bar = 0, mmio_bar = 0;
    for (int i = 0; i < 6; i++) {
        if (!pd->bar_is_mmio[i] || !pd->bar_addr[i]) continue;
        bool prefetch = pd->bar[i] & 8;
        if (prefetch && !vram_bar) vram_bar = pd->bar_addr[i];
        else if (!prefetch && !mmio_bar) mmio_bar = pd->bar_addr[i];
    }
    if (!vram_bar || !mmio_bar) { kprintf("radeon: %s: BARs not assigned (vram %lx, mmio %lx)\n", r->gpu.name, vram_bar, mmio_bar); return false; }
    pci_enable_bus_master(pd);
    r->mmio_phys = mmio_bar; r->mmio_len = 0x10000;
    r->mmio = vmm_map_mmio(mmio_bar, r->mmio_len);
    r->gpu.mmio_base = mmio_bar; r->gpu.vram_base = vram_bar;
    /* VRAM size */
    uint64_t memsize = rreg(r, CONFIG_MEMSIZE);
    if (memsize && memsize < 4096) memsize <<= 20;             /* some IGPs report MiB */
    if (memsize == 0xFFFFFFFFull) memsize = 0;
    r->gpu.vram_bytes = memsize;
    uint32_t fbloc = rreg(r, R700_MC_VM_FB_LOCATION);
    if (fbloc == 0 || fbloc == 0xFFFFFFFF) fbloc = rreg(r, R600_MC_VM_FB_LOCATION);
    r->fb_start_gpu = (uint64_t)(fbloc & 0xFFFF) << 24;
    uint64_t fb_end_gpu = ((uint64_t)(fbloc >> 16) << 24) | 0xFFFFFF;
    if (!memsize) memsize = fb_end_gpu - r->fb_start_gpu + 1;
    r->aper_len = MIN(memsize, 256ull << 20);
    r->vram = vmm_map_wc(vram_bar, r->aper_len);
    /* which CRTC drives the primary surface the firmware set up? */
    r->crtc = (rreg(r, D1CRTC_CONTROL) & 1) ? 0 : ((rreg(r, D1CRTC_CONTROL + CRTC2_OFFSET) & 1) ? 1 : 0);
    r->scan_gpu_addr = rreg(r, crtc_reg(r, D1GRPH_PRIMARY_SURFACE_ADDRESS));
    r->pitch_px = rreg(r, crtc_reg(r, D1GRPH_PITCH));
    r->width = rreg(r, crtc_reg(r, D1GRPH_X_END));
    r->height = rreg(r, crtc_reg(r, D1GRPH_Y_END));
    uint32_t grph = rreg(r, crtc_reg(r, D1GRPH_CONTROL));
    r->scan_off = r->scan_gpu_addr - r->fb_start_gpu;
    kprintf("radeon: %s (%s, %d SPs) at %s, VRAM %lu MiB (aperture %lu MiB), MC fb %lx-%lx\n", r->gpu.name, ci->chip, ci->sps, r->gpu.bus, memsize >> 20, r->aper_len >> 20, r->fb_start_gpu, fb_end_gpu);
    kprintf("radeon: crtc%d scanout %ux%u pitch %u px, surface %08x (VRAM offset %lx), format %s\n", r->crtc, r->width, r->height, r->pitch_px, r->scan_gpu_addr, r->scan_off, (grph & 3) == 2 ? "32bpp" : "non-32bpp");
    parse_atombios(r);
    /* verify the bootloader framebuffer is this scanout surface */
    struct limine_framebuffer *fb = framebuffer_request.response && framebuffer_request.response->framebuffer_count ? framebuffer_request.response->framebuffers[0] : NULL;
    uint64_t fb_phys = fb ? (uint64_t)fb->address - hhdm_offset : 0;
    bool match = fb && fb_phys >= vram_bar && fb_phys < vram_bar + r->aper_len && (fb_phys - vram_bar) == r->scan_off && fb->width == r->width && fb->height == r->height && (grph & 3) == 2;
    r->gpu.scanout_verified = match;
    if (match) {
        uint64_t fb_size = (uint64_t)r->pitch_px * 4 * r->height;
        uint64_t next = ALIGN_UP(r->scan_off + fb_size, 0x10000);
        if (next + fb_size + 0x10000 <= r->aper_len && (grph & 3) == 2) {
            r->buf_off[0] = r->scan_off;          /* currently visible */
            r->buf_off[1] = next;                 /* first back buffer */
            r->back = 1;
            memcpy(r->vram + r->buf_off[1], r->vram + r->buf_off[0], fb_size);
            r->cursor_off = ALIGN_UP(next + fb_size, 0x1000);
            r->gpu.page_flip = true; r->gpu.hw_cursor = true;
            r->gpu.back_buffer = radeon_back_buffer; r->gpu.flip = radeon_flip;
            r->gpu.set_cursor = radeon_set_cursor; r->gpu.move_cursor = radeon_move_cursor;
            kprintf("radeon: scanout verified; page flipping (back buffer at %lx) and hardware cursor enabled\n", next);
        } else kprintf("radeon: scanout verified; not enough aperture for a back buffer\n");
    } else kprintf("radeon: bootloader framebuffer is not this adapter's scanout; using the software path\n");
    gpu_register(&r->gpu);
    return true;
}

void radeon_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->vendor != 0x1002 || pd->class_code != 0x03) continue;
        const struct chip_info *ci = NULL;
        for (size_t k = 0; k < ARRAY_LEN(chips); k++) if (pd->device >= chips[k].lo && pd->device <= chips[k].hi) ci = &chips[k];
        if (!ci) { kprintf("radeon: ATI device %04x is not an R700; software path\n", pd->device); continue; }
        if (probe(pd, ci)) return;
    }
}
