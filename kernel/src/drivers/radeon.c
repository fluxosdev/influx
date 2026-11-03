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
 *   - finds the video BIOS (ACPI VFCT table on UEFI, the C000:0000 shadow
 *     after a BIOS boot, VRAM on the IGP, or the PCI expansion ROM), keeps a
 *     copy and parses it: bootup message, part number, default clocks
 *   - provides a 64x64 ARGB hardware cursor
 *   - provides vblank-synchronised double-buffered page flipping of the
 *     primary surface (the compositor renders into the VRAM back buffer)
 *   - runtime mode setting through the AtomBIOS command tables, with the
 *     mode list from the monitor's EDID (radeon_kms.c, atom.c)
 *
 * The 3D engine (command processor + shaders) needs the R700 CP microcode
 * and a full command-stream backend; rendering therefore uses the software
 * rasteriser, which is the fallback for every other adapter as well.
 */
#include "radeon.h"
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
#define BUS_CNTL                 0x5420
#define BIOS_ROM_DIS             (1u << 1)
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
/* the R700 high-address registers are not at the usual CRTC2 stride */
#define D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH   0x6914
#define D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH 0x691C
#define D2GRPH_PRIMARY_SURFACE_ADDRESS_HIGH   0x6114
#define D2GRPH_SECONDARY_SURFACE_ADDRESS_HIGH 0x611C
#define D1CUR_CONTROL            0x6400
#define D1CUR_SURFACE_ADDRESS    0x6408
#define D1CUR_SIZE               0x6410
#define D1CUR_POSITION           0x6414
#define D1CUR_HOT_SPOT           0x6418
#define D1CUR_UPDATE             0x6424
#define D1MODE_VIEWPORT_SIZE     0x6584
#define GRPH_UPDATE_LOCK        (1u << 16)
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

static struct radeon rdev;

uint32_t radeon_rreg(struct radeon *r, uint32_t reg) {
    if (reg < r->mmio_len) return r->mmio[reg / 4];
    r->mmio[MM_INDEX / 4] = reg; return r->mmio[MM_DATA / 4];
}
void radeon_wreg(struct radeon *r, uint32_t reg, uint32_t v) {
    if (reg < r->mmio_len) { r->mmio[reg / 4] = v; return; }
    r->mmio[MM_INDEX / 4] = reg; r->mmio[MM_DATA / 4] = v;
}
#define rreg radeon_rreg
#define wreg radeon_wreg
void radeon_udelay(uint32_t us) { uint64_t t = rdtsc() + (uint64_t)us * (cpu_tsc_hz() ? cpu_tsc_hz() / 1000000 : 2000); while (rdtsc() < t) pause(); }
#define udelay radeon_udelay

/* ---- video BIOS ---- */
#define ROM_MAX     (128u << 10)
#define PCI_ROM_BAR 0x30

/* an x86 option ROM whose PCI data structure names ATI */
static bool rom_valid(const uint8_t *rom, size_t len) {
    if (len < 0x200 || rom[0] != 0x55 || rom[1] != 0xAA) return false;
    uint16_t pcir = *(const uint16_t *)(rom + 0x18);
    if ((size_t)pcir + 8 > len || memcmp(rom + pcir, "PCIR", 4)) return false;
    return *(const uint16_t *)(rom + pcir + 4) == 0x1002;
}
static bool rom_take(struct radeon *r, const uint8_t *src, size_t avail, const char *how) {
    if (!rom_valid(src, avail)) return false;
    size_t len = (size_t)src[2] * 512;   /* the first (legacy x86) image holds the AtomBIOS tables */
    if (len < 0x200 || len > avail) len = avail;
    len = MIN(len, (size_t)ROM_MAX);
    r->rom = kmalloc(len);
    if (!r->rom) return false;
    memcpy(r->rom, src, len); r->rom_len = len;
    kprintf("radeon: video BIOS from %s (%zu KiB)\n", how, len >> 10);
    return true;
}

/* UEFI firmware hands the GOP-era VBIOS to the OS in the ACPI VFCT table */
struct vfct { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6], oem_table[8]; uint32_t oem_rev, creator, creator_rev;
              uint8_t uuid[16]; uint32_t vbios_offset, lib1_offset, reserved[4]; } PACKED;
struct vfct_image { uint32_t bus, device, function; uint16_t vendor, device_id, ss_vendor, ss_id; uint32_t revision, length; } PACKED;
static bool rom_from_vfct(struct radeon *r, struct pci_device *pd) {
    const struct vfct *t = acpi_find_table("VFCT");
    if (!t) return false;
    for (uint32_t off = t->vbios_offset; off + sizeof(struct vfct_image) <= t->len; ) {
        const struct vfct_image *im = (const void *)((const uint8_t *)t + off);
        off += sizeof *im;
        if (im->length > t->len - off) break;
        if (im->length && im->bus == pd->bus && im->device == pd->slot && im->function == pd->func && im->vendor == pd->vendor && im->device_id == pd->device)
            return rom_take(r, (const uint8_t *)(im + 1), im->length, "the ACPI VFCT table");
        off += im->length;
    }
    return false;
}

/* the PCI expansion ROM, if the firmware assigned it an address. The ROM BAR
 * is sized and enabled only for the copy; BIOS_ROM_DIS gates the decode on R600+ */
static bool rom_from_bar(struct radeon *r, struct pci_device *pd) {
    if (pd->header_type & 0x7F) return false;
    uint32_t orig = pci_read32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR);
    uint32_t addr = orig & 0xFFFFF800;
    if (!addr) { kprintf("radeon: expansion ROM BAR not assigned\n"); return false; }
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, 0xFFFFF800);
    uint32_t mask = pci_read32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR) & 0xFFFFF800;
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, addr);
    if (!mask) return false;
    size_t size = MIN((size_t)(~mask + 1), (size_t)ROM_MAX);
    uint8_t *tmp = kmalloc(size);
    if (!tmp) { pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, orig); return false; }
    uint32_t bus_cntl = rreg(r, BUS_CNTL);
    wreg(r, BUS_CNTL, bus_cntl & ~BIOS_ROM_DIS);
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, addr | 1);
    const volatile uint32_t *src = vmm_map_mmio(addr, size);
    for (size_t i = 0; i < size / 4; i++) ((uint32_t *)tmp)[i] = src[i];
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, orig);
    wreg(r, BUS_CNTL, bus_cntl);
    bool ok = rom_take(r, tmp, size, "the PCI expansion ROM");
    kfree(tmp);
    return ok;
}

static bool find_rom(struct radeon *r, struct pci_device *pd) {
    if (rom_from_vfct(r, pd)) return true;
    if (rom_take(r, acpi_map_phys(0xC0000, 0x20000), 0x20000, "the C000:0000 shadow")) return true;   /* legacy option ROM shadow after a BIOS boot */
    if (r->is_rs880 && rom_take(r, r->vram, MIN(r->aper_len, (uint64_t)ROM_MAX), "VRAM")) return true;   /* IGPs keep it in stolen memory */
    if (rom_from_bar(r, pd)) return true;
    kprintf("radeon: no video BIOS found (VFCT, C000:0000 shadow, expansion ROM)\n");
    return false;
}

/* ---- AtomBIOS ROM ---- */
static void parse_atombios(struct radeon *r) {
    const uint8_t *rom = r->rom;
    size_t rom_size = r->rom_len;
    uint16_t hdr = *(const uint16_t *)(rom + 0x48);
    if (hdr + 36 > rom_size || memcmp(rom + hdr + 4, "ATOM", 4)) { kprintf("radeon: video BIOS is not AtomBIOS\n"); return; }
    uint16_t msg = *(const uint16_t *)(rom + hdr + 16);
    if (msg && msg < rom_size) {
        const char *s = (const char *)rom + msg, *end = (const char *)rom + rom_size; size_t i = 0;
        while (s < end && (*s == '\r' || *s == '\n')) s++;
        while (s + i < end && s[i] && s[i] != '\n' && s[i] != '\r' && i < sizeof r->gpu.bios_message - 1) { r->gpu.bios_message[i] = s[i]; i++; }
        r->gpu.bios_message[i] = 0;
        while (i > 0 && r->gpu.bios_message[i - 1] == ' ') r->gpu.bios_message[--i] = 0;
    }
    /* part numbers look like "113-xxxxxxx-xxx" */
    for (size_t i = 0; i + 16 < rom_size; i++) if (rom[i] == '1' && rom[i + 1] == '1' && rom[i + 2] == '3' && rom[i + 3] == '-' && isalnum(rom[i + 4])) {
        size_t n = 0; while (n < sizeof r->gpu.bios_part - 1 && i + n < rom_size && isprint(rom[i + n]) && rom[i + n] != ' ') { r->gpu.bios_part[n] = rom[i + n]; n++; }
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
uint32_t radeon_crtc_reg(struct radeon *r, uint32_t reg) { return reg + (r->crtc ? CRTC2_OFFSET : 0); }
#define crtc_reg radeon_crtc_reg

static void wait_vblank(struct radeon *r) {
    /* wait for the start of the next vertical blank (bounded) */
    for (int i = 0; i < 40000 && (rreg(r, crtc_reg(r, D1CRTC_STATUS)) & CRTC_V_BLANK); i++) udelay(1);
    for (int i = 0; i < 40000 && !(rreg(r, crtc_reg(r, D1CRTC_STATUS)) & CRTC_V_BLANK); i++) udelay(1);
}
void radeon_set_surface_address(struct radeon *r, uint64_t gpu_addr) {
    uint32_t upd = crtc_reg(r, D1GRPH_UPDATE);
    uint32_t tmp = rreg(r, upd);
    wreg(r, upd, tmp | GRPH_UPDATE_LOCK);
    wreg(r, r->crtc ? D2GRPH_SECONDARY_SURFACE_ADDRESS_HIGH : D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH, (uint32_t)(gpu_addr >> 32));
    wreg(r, r->crtc ? D2GRPH_PRIMARY_SURFACE_ADDRESS_HIGH : D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(gpu_addr >> 32));
    wreg(r, crtc_reg(r, D1GRPH_SECONDARY_SURFACE_ADDRESS), (uint32_t)gpu_addr);
    wreg(r, crtc_reg(r, D1GRPH_PRIMARY_SURFACE_ADDRESS), (uint32_t)gpu_addr);
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
    radeon_set_surface_address(r, r->fb_start_gpu + r->buf_off[r->back]);
    r->back ^= 1;
}

/* ---- hardware cursor ---- */
void radeon_cursor_upload(struct radeon *r) {
    if (!r->cursor_valid) return;
    memcpy(r->vram + r->cursor_off, r->cursor_img, sizeof r->cursor_img);
    wreg(r, crtc_reg(r, D1CUR_UPDATE), CURSOR_UPDATE_LOCK);
    wreg(r, crtc_reg(r, D1CUR_SURFACE_ADDRESS), (uint32_t)(r->fb_start_gpu + r->cursor_off));
    wreg(r, crtc_reg(r, D1CUR_SIZE), (63u << 16) | 63u);
    /* HOT_SPOT is the image origin used for clipping at the top/left edge, not the pointer hot spot:
     * callers already pass the image's top-left to move_cursor (as for i915) */
    wreg(r, crtc_reg(r, D1CUR_HOT_SPOT), 0);
    wreg(r, crtc_reg(r, D1CUR_CONTROL), CURSOR_EN | (CURSOR_MODE_ARGB_PREMUL << CURSOR_MODE_SHIFT));
    wreg(r, crtc_reg(r, D1CUR_UPDATE), 0);
}
static bool radeon_set_cursor(struct gpu_device *g, const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    struct radeon *r = g->priv;
    if (w > 64 || h > 64) return false;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint32_t p = (x < w && y < h) ? argb[y * w + x] : 0;
        uint32_t a = p >> 24;
        /* the AVIVO cursor expects premultiplied alpha */
        uint32_t rr = ((p >> 16) & 0xFF) * a / 255, gg = ((p >> 8) & 0xFF) * a / 255, bb = (p & 0xFF) * a / 255;
        r->cursor_img[y * 64 + x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }
    r->cursor_hot_x = hot_x; r->cursor_hot_y = hot_y; r->cursor_valid = true;
    radeon_cursor_upload(r);
    return true;
}
static void radeon_move_cursor(struct gpu_device *g, int x, int y, bool visible) {
    struct radeon *r = g->priv;
    /* partly off the top/left edge: position at 0 and start the image further in (as Linux's avivo code) */
    int xo = 0, yo = 0;
    if (x < 0) { xo = MIN(-x, 63); x = 0; }
    if (y < 0) { yo = MIN(-y, 63); y = 0; }
    wreg(r, crtc_reg(r, D1CUR_UPDATE), CURSOR_UPDATE_LOCK);
    wreg(r, crtc_reg(r, D1CUR_HOT_SPOT), ((uint32_t)xo << 16) | (uint32_t)yo);
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
    r->pd = pd;
    r->is_rs880 = !strcmp(ci->chip, "RS880");
    /* display engine: RV770 and the RS880 IGP are DCE 3.0, RV710/RV730/RV740 DCE 3.2 */
    r->dce32 = !strcmp(ci->chip, "RV730") || !strcmp(ci->chip, "RV710") || !strcmp(ci->chip, "RV740");
    /* BAR0: VRAM aperture (prefetchable), BAR2: registers (64 KiB), an I/O BAR for AtomBIOS indirect I/O */
    uint64_t vram_bar = 0, mmio_bar = 0;
    for (int i = 0; i < 6; i++) {
        if (!pd->bar_is_mmio[i] && (pd->bar[i] & 1) && (pd->bar[i] & ~3u) && !r->io_port) r->io_port = (uint16_t)(pd->bar[i] & ~3u);
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
    if (find_rom(r, pd)) parse_atombios(r);
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
            if (r->rom) radeon_kms_init(r);
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
