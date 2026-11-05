/*
 * AMD Radeon RX 5000 "Navi 14" (RDNA1) display driver.
 *
 * Register offsets come from the Linux amdgpu driver (navi14_ip_offset.h,
 * dcn_2_0_0_offset.h/_sh_mask.h, nbio_2_3_offset.h, mmhub_2_0_0_offset.h)
 * and the programming sequences from its DC display core (dcn20_hubp.c,
 * dcn20_dpp.c, dcn10_dpp.c, dcn10_mpc.c, dcn10_hwseq.c). Copyright Advanced
 * Micro Devices, Inc.; MIT licence, see atom.h.
 *
 * The display engine is DCN 2.0. As the R700 driver does, this keeps the mode
 * the UEFI GOP set and needs no firmware:
 *   - maps the register BAR (BAR5) and the VRAM BAR (BAR0, which may be
 *     resizable and above 4 GiB)
 *   - reads the VRAM size (NBIO) and the MC framebuffer location (MMHUB)
 *   - finds every display pipe (HUBP) scanning out the bootloader
 *     framebuffer: the GOP clones one surface to every monitor it lights,
 *     one pipe each
 *   - page flipping: writes the new surface address to each of those pipes
 *     (latched by the hardware at the next VUPDATE) and waits for the flip;
 *     the first flip that does not complete in 100 ms turns flipping off
 *     and puts the bootloader framebuffer back on screen
 *   - a 64x64 ARGB hardware cursor on each of those pipes
 *   - parses the AtomFirmware video BIOS (ACPI VFCT or the expansion ROM):
 *     bootup message, part number, default clocks, VRAM reserved by firmware
 * Not handled: mode setting (DCN has no AtomBIOS mode-set tables; it needs
 * OTG timing, DIO encoders, DP link training and SMU clock requests) and
 * acceleration (the PSP has to load the signed GFX/SDMA firmware first).
 *
 * Every step logs with the "amdgpu:" prefix; `dmesg` shows them.
 */
#include "gpu.h"
#include "gfx.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

#define NAVI14_PIPES 5

/* Byte offsets in the register BAR, (IP_BASE[segment] + mmREG) * 4, generated
 * from navi14_ip_offset.h (DMU_BASE 0x12/0xC0/0x34C0/0x9000, NBIF0_BASE seg 2
 * 0xD20, MMHUB_BASE seg 0 0x1A000) and the DCN 2.0 register headers. Pipe i is
 * HUBP i / HUBPREQ i / CURSOR0_i / DPP i (CNVC_CUR i) / MPCC i; DPP 4 lives in
 * a separate register block, hence its irregular offset. */
struct dcn_pipe_regs {
    uint32_t surface_config, tiling_config, viewport_start, viewport_dim, dchubp_cntl, hubp_clk_cntl;
    uint32_t surface_pitch, pri_addr, pri_addr_high, surface_control, flip_control, inuse, inuse_high;
    uint32_t cursor_settings, aperture_low, aperture_high;
    uint32_t cur_control, cur_addr, cur_addr_high, cur_size, cur_position, cur_hot_spot, cur_dst_offset;
    uint32_t dpp_cur_control, mpcc_opp_id;
};
static const struct dcn_pipe_regs dcn20_pipe[NAVI14_PIPES] = {
    { 0x0ea94, 0x0ea9c, 0x0eaa4, 0x0eaa8, 0x0eacc, 0x0ead0, 0x0eb1c, 0x0eb28, 0x0eb2c, 0x0eb68, 0x0eb6c, 0x0eb94, 0x0eb98, 0x0ec78, 0x0ebdc, 0x0ebe0,
      0x0ece0, 0x0ece4, 0x0ece8, 0x0ecec, 0x0ecf0, 0x0ecf4, 0x0ecfc, 0x10680, 0x11ccc },
    { 0x0ee04, 0x0ee0c, 0x0ee14, 0x0ee18, 0x0ee3c, 0x0ee40, 0x0ee8c, 0x0ee98, 0x0ee9c, 0x0eed8, 0x0eedc, 0x0ef04, 0x0ef08, 0x0efe8, 0x0ef4c, 0x0ef50,
      0x0f050, 0x0f054, 0x0f058, 0x0f05c, 0x0f060, 0x0f064, 0x0f06c, 0x10c2c, 0x11d38 },
    { 0x0f174, 0x0f17c, 0x0f184, 0x0f188, 0x0f1ac, 0x0f1b0, 0x0f1fc, 0x0f208, 0x0f20c, 0x0f248, 0x0f24c, 0x0f274, 0x0f278, 0x0f358, 0x0f2bc, 0x0f2c0,
      0x0f3c0, 0x0f3c4, 0x0f3c8, 0x0f3cc, 0x0f3d0, 0x0f3d4, 0x0f3dc, 0x111d8, 0x11da4 },
    { 0x0f4e4, 0x0f4ec, 0x0f4f4, 0x0f4f8, 0x0f51c, 0x0f520, 0x0f56c, 0x0f578, 0x0f57c, 0x0f5b8, 0x0f5bc, 0x0f5e4, 0x0f5e8, 0x0f6c8, 0x0f62c, 0x0f630,
      0x0f730, 0x0f734, 0x0f738, 0x0f73c, 0x0f740, 0x0f744, 0x0f74c, 0x11784, 0x11e10 },
    { 0x0f854, 0x0f85c, 0x0f864, 0x0f868, 0x0f88c, 0x0f890, 0x0f8dc, 0x0f8e8, 0x0f8ec, 0x0f928, 0x0f92c, 0x0f954, 0x0f958, 0x0fa38, 0x0f99c, 0x0f9a0,
      0x0faa0, 0x0faa4, 0x0faa8, 0x0faac, 0x0fab0, 0x0fab4, 0x0fabc, 0x1aaac, 0x11e7c },
};
/* OTG (timing generator) i, indexed by the HUBP's HUBP_VTG_SEL */
static const uint32_t otg_control[NAVI14_PIPES] = { 0x14004, 0x14204, 0x14404, 0x14604, 0x14804 };
static const uint32_t otg_h_total[NAVI14_PIPES] = { 0x13fa8, 0x141a8, 0x143a8, 0x145a8, 0x147a8 };
static const uint32_t otg_v_total[NAVI14_PIPES] = { 0x13fbc, 0x141bc, 0x143bc, 0x145bc, 0x147bc };
/* MPC CUR_VUPDATE_LOCK_SET, indexed by OPP: holds cursor register updates until released */
static const uint32_t cur_vupdate_lock[NAVI14_PIPES] = { 0x12084, 0x12098, 0x120ac, 0x120c0, 0x120d4 };

#define RCC_CONFIG_MEMSIZE        0x0378c   /* NBIO: VRAM size in MiB */
#define MC_VM_FB_LOCATION_BASE    0x6a1b0   /* MMHUB: VRAM in the MC address space, 16 MiB units */
#define MC_VM_FB_LOCATION_TOP     0x6a1b4
#define FB_LOCATION_MASK          0x00FFFFFFu
#define MMIO_SIZE                 0x80000   /* BAR5 is 512 KiB */
#define VRAM_BAR_MIN              (256ull << 20)   /* the smallest BAR0 a Navi exposes */

/* field masks (dcn_2_0_0_sh_mask.h) */
#define SURFACE_PIXEL_FORMAT_MASK 0x7Fu
#define SURFACE_PIXEL_FORMAT_8888 8u        /* ARGB8888 or ABGR8888 (the crossbar swaps) */
#define ROTATION_ANGLE_MASK       0x300u
#define H_MIRROR_EN               0x400u
#define SW_MODE_MASK              0x1Fu     /* 0 = linear */
#define PRIMARY_SURFACE_DCC_EN    0x2u
#define VIEWPORT_LO(v)            ((v) & 0x3FFF)
#define VIEWPORT_HI(v)            (((v) >> 16) & 0x3FFF)
#define HUBP_BLANK_EN             0x1u
#define HUBP_DISABLE              0x4u
#define HUBP_VTG_SEL(v)           (((v) >> 4) & 0xF)
#define HUBP_CLOCK_ENABLE         0x1u
#define PITCH_MASK                0x3FFFu   /* pixels - 1 */
#define SURFACE_FLIP_TYPE         0x2u      /* 1 = immediate, 0 = at VUPDATE */
#define SURFACE_FLIP_PENDING      0x100u
#define ADDR_HIGH_MASK            0xFFFFu
#define APERTURE_MASK             0x3FFFFFFFu
#define APERTURE_SHIFT            18        /* system aperture bounds are in 256 KiB units */
#define OTG_MASTER_EN             0x1u
#define MPCC_OPP_ID_MASK          0xFu
#define CURSOR_ENABLE             0x1u
#define CURSOR_MODE_SHIFT         8
#define CURSOR_MODE_MASK          (7u << 8)
#define CURSOR_PITCH_MASK         (3u << 16)   /* 0 = 64 pixels */
#define CURSOR_LPC_SHIFT          24
#define CURSOR_LPC_MASK           (0x1Fu << 24)
#define CURSOR_2X_MAGNIFY         0x10u
#define CURSOR_MODE_PREMUL        2u           /* CURSOR_MODE_COLOR_PRE_MULTIPLIED_ALPHA */
#define CURSOR_LINES_PER_CHUNK_8  3u           /* 33..64 pixels wide, colour modes (hubp2_get_lines_per_chunk) */
#define CURSOR_CHUNK_HDL_ADJUST(n) ((uint32_t)(n) << 8)
#define CUR0_ENABLE               0x1u
#define CUR0_EXPANSION_MODE       0x2u
#define CUR0_ROM_EN               0x8u
#define CUR0_MODE_SHIFT           4
#define CUR0_MODE_MASK            (7u << 4)

struct navi_model { uint16_t id; uint8_t rev; const char *name; int sps; };
static const struct navi_model models[] = {   /* libdrm amdgpu.ids */
    { 0x7340, 0xC1, "Radeon RX 5500M", 1408 }, { 0x7340, 0xC3, "Radeon RX 5300M", 1280 },
    { 0x7340, 0xC5, "Radeon RX 5500 XT", 1408 }, { 0x7340, 0xC7, "Radeon RX 5500", 1408 },
    { 0x7340, 0xC9, "Radeon RX 5500XTB", 1408 }, { 0x7340, 0xCF, "Radeon RX 5300", 1408 },
    { 0x7341, 0x00, "Radeon Pro W5500", 1408 }, { 0x7347, 0x00, "Radeon Pro W5500M", 1408 },
    { 0x734F, 0x00, "Radeon Pro W5300M", 1408 },
};
bool amdgpu_supported(uint16_t device) { return device == 0x7340 || device == 0x7341 || device == 0x7347 || device == 0x734F; }

struct navi {
    struct gpu_device gpu;
    struct pci_device *pd;
    volatile uint32_t *mmio;
    uint64_t vram_bar, fb_base_mc;       /* CPU address of BAR0; MC address of VRAM offset 0 */
    uint8_t *win; uint64_t win_off;      /* CPU mapping of VRAM [win_off, win_off + mapped) */
    int pipes[NAVI14_PIPES], npipes;     /* pipes scanning out our surface */
    int opp[NAVI14_PIPES];               /* their OPP (MPC cursor lock index), -1 if unknown */
    uint32_t vp_x[NAVI14_PIPES], vp_y[NAVI14_PIPES], vp_w[NAVI14_PIPES], vp_h[NAVI14_PIPES];
    uint64_t apt_lo[NAVI14_PIPES], apt_hi[NAVI14_PIPES];   /* their system aperture (MC, inclusive): fetched without page tables */
    uint32_t pitch_px, width, height;
    uint64_t fb_size;                    /* bytes in one scanout buffer */
    uint64_t buf_off[2]; int back;       /* VRAM offsets of the two scanout buffers */
    uint64_t cursor_off;
    uint32_t cursor_img[64 * 64];
    bool cursor_valid;
    uint8_t *rom; size_t rom_len;
    uint64_t fw_resv_start, fw_resv_size;   /* VRAM reserved by the video BIOS, 0 if none */
};
static struct navi ndev;

static inline uint32_t rd(struct navi *n, uint32_t off) { return n->mmio[off / 4]; }
static inline void wr(struct navi *n, uint32_t off, uint32_t v) { n->mmio[off / 4] = v; }
static void udelay(uint32_t us) { uint64_t t = rdtsc() + (uint64_t)us * (cpu_tsc_hz() ? cpu_tsc_hz() / 1000000 : 2000); while (rdtsc() < t) pause(); }
static inline void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline uint8_t *vram_ptr(struct navi *n, uint64_t off) { return n->win + (off - n->win_off); }

/* ---- video BIOS ---- */
#define ROM_MAX     (256u << 10)
#define PCI_ROM_BAR 0x30

static bool rom_valid(const uint8_t *rom, size_t len) {
    if (len < 0x200 || rom[0] != 0x55 || rom[1] != 0xAA) return false;
    uint16_t pcir = *(const uint16_t *)(rom + 0x18);
    if ((size_t)pcir + 8 > len || memcmp(rom + pcir, "PCIR", 4)) return false;
    return *(const uint16_t *)(rom + pcir + 4) == 0x1002;
}
static bool rom_take(struct navi *n, const uint8_t *src, size_t avail, const char *how) {
    if (!rom_valid(src, avail)) return false;
    size_t len = (size_t)src[2] * 512;   /* the first (legacy) image holds the AtomFirmware tables */
    if (len < 0x200 || len > avail) len = avail;
    len = MIN(len, (size_t)ROM_MAX);
    n->rom = kmalloc(len);
    if (!n->rom) return false;
    memcpy(n->rom, src, len); n->rom_len = len;
    kprintf("amdgpu: video BIOS from %s (%zu KiB)\n", how, len >> 10);
    return true;
}
/* UEFI firmware hands the GOP-era VBIOS to the OS in the ACPI VFCT table */
struct vfct { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6], oem_table[8]; uint32_t oem_rev, creator, creator_rev;
              uint8_t uuid[16]; uint32_t vbios_offset, lib1_offset, reserved[4]; } PACKED;
struct vfct_image { uint32_t bus, device, function; uint16_t vendor, device_id, ss_vendor, ss_id; uint32_t revision, length; } PACKED;
static bool rom_from_vfct(struct navi *n, struct pci_device *pd) {
    const struct vfct *t = acpi_find_table("VFCT");
    if (!t) return false;
    for (uint32_t off = t->vbios_offset; off + sizeof(struct vfct_image) <= t->len; ) {
        const struct vfct_image *im = (const void *)((const uint8_t *)t + off);
        off += sizeof *im;
        if (im->length > t->len - off) break;
        if (im->length && im->bus == pd->bus && im->device == pd->slot && im->function == pd->func && im->vendor == pd->vendor && im->device_id == pd->device)
            return rom_take(n, (const uint8_t *)(im + 1), im->length, "the ACPI VFCT table");
        off += im->length;
    }
    return false;
}
/* the PCI expansion ROM, enabled only for the copy */
static bool rom_from_bar(struct navi *n, struct pci_device *pd) {
    uint32_t orig = pci_read32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR);
    uint32_t addr = orig & 0xFFFFF800;
    if (!addr) { kprintf("amdgpu: expansion ROM BAR not assigned\n"); return false; }
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, 0xFFFFF800);
    uint32_t mask = pci_read32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR) & 0xFFFFF800;
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, addr);
    if (!mask) return false;
    size_t size = MIN((size_t)(~mask + 1), (size_t)ROM_MAX);
    uint8_t *tmp = kmalloc(size);
    if (!tmp) { pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, orig); return false; }
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, addr | 1);
    const volatile uint32_t *src = vmm_map_mmio(addr, size);
    for (size_t i = 0; i < size / 4; i++) ((uint32_t *)tmp)[i] = src[i];
    pci_write32(pd->bus, pd->slot, pd->func, PCI_ROM_BAR, orig);
    bool ok = rom_take(n, tmp, size, "the PCI expansion ROM");
    kfree(tmp);
    return ok;
}
static bool find_rom(struct navi *n, struct pci_device *pd) {
    if (rom_from_vfct(n, pd)) return true;
    if (rom_take(n, acpi_map_phys(0xC0000, 0x20000), 0x20000, "the C000:0000 shadow")) return true;   /* after a legacy BIOS boot */
    if (rom_from_bar(n, pd)) return true;
    kprintf("amdgpu: no video BIOS found (VFCT, C000:0000 shadow, expansion ROM)\n");
    return false;
}

/* AtomFirmware: atom_rom_header_v2_2 and atom_master_list_of_data_tables_v2_1 (atomfirmware.h) */
#define ATOM_DATA_FIRMWAREINFO   4
#define ATOM_DATA_VRAM_USAGE     11
static uint16_t atom_data_table(struct navi *n, uint16_t hdr, int index) {
    uint16_t master = *(const uint16_t *)(n->rom + hdr + 32);
    if (!master || master + 4 + 2 * (index + 1) > n->rom_len) return 0;
    uint16_t t = *(const uint16_t *)(n->rom + master + 4 + 2 * index);
    return t && t + 4u <= n->rom_len ? t : 0;
}
static void parse_atomfirmware(struct navi *n) {
    const uint8_t *rom = n->rom;
    size_t rom_size = n->rom_len;
    uint16_t hdr = *(const uint16_t *)(rom + 0x48);
    if (hdr + 36u > rom_size || memcmp(rom + hdr + 4, "ATOM", 4)) { kprintf("amdgpu: video BIOS is not AtomFirmware\n"); return; }
    uint16_t msg = *(const uint16_t *)(rom + hdr + 16);   /* vbios_bootupmessageoffset */
    if (msg && msg < rom_size) {
        const char *s = (const char *)rom + msg, *end = (const char *)rom + rom_size; size_t i = 0;
        while (s < end && (*s == '\r' || *s == '\n' || *s == ' ')) s++;
        while (s + i < end && s[i] && s[i] != '\n' && s[i] != '\r' && i < sizeof n->gpu.bios_message - 1) { n->gpu.bios_message[i] = s[i]; i++; }
        n->gpu.bios_message[i] = 0;
        while (i > 0 && n->gpu.bios_message[i - 1] == ' ') n->gpu.bios_message[--i] = 0;
    }
    /* part numbers look like "113-xxxxxxx-xxx" */
    for (size_t i = 0; i + 16 < rom_size; i++) if (rom[i] == '1' && rom[i + 1] == '1' && rom[i + 2] == '3' && rom[i + 3] == '-' && isalnum(rom[i + 4])) {
        size_t k = 0; while (k < sizeof n->gpu.bios_part - 1 && i + k < rom_size && isprint(rom[i + k]) && rom[i + k] != ' ') { n->gpu.bios_part[k] = rom[i + k]; k++; }
        n->gpu.bios_part[k] = 0; break;
    }
    uint16_t fw = atom_data_table(n, hdr, ATOM_DATA_FIRMWAREINFO);
    if (fw && fw + 16u <= rom_size) {   /* atom_firmware_info_v3_x: bootup sclk/mclk in 10 kHz */
        n->gpu.engine_clock_khz = *(const uint32_t *)(rom + fw + 8) * 10;
        n->gpu.memory_clock_khz = *(const uint32_t *)(rom + fw + 12) * 10;
    }
    uint16_t vu = atom_data_table(n, hdr, ATOM_DATA_VRAM_USAGE);
    if (vu && vu + 12u <= rom_size) {
        uint8_t frev = rom[vu + 2], crev = rom[vu + 3];
        uint32_t start_kb = *(const uint32_t *)(rom + vu + 4) & 0x3FFFFFFF;   /* top bits are ATOM_VRAM_OPERATION_FLAGS */
        uint64_t size_kb = *(const uint16_t *)(rom + vu + 8);
        if (frev == 2 && crev == 1) size_kb += *(const uint16_t *)(rom + vu + 10);   /* v2.1: + used_by_driver_in_kb */
        if (start_kb && size_kb) { n->fw_resv_start = (uint64_t)start_kb << 10; n->fw_resv_size = size_kb << 10; }
        kprintf("amdgpu: VRAM used by firmware (table v%u.%u): %lx + %lu KiB\n", frev, crev, n->fw_resv_start, n->fw_resv_size >> 10);
    }
    kprintf("amdgpu: AtomFirmware '%s' %s, engine %u MHz, memory %u MHz\n", n->gpu.bios_message, n->gpu.bios_part, n->gpu.engine_clock_khz / 1000, n->gpu.memory_clock_khz / 1000);
}

/* ---- page flipping ---- */
static uint8_t *navi_back_buffer(struct gpu_device *g, int *pitch) {
    struct navi *n = g->priv;
    *pitch = n->pitch_px * 4;
    return vram_ptr(n, n->buf_off[n->back]);
}
/* dcn20_hubp.c hubp2_program_surface_flip_and_addr: high half first, the low
 * write latches both; the flip happens at the pipe's next VUPDATE, or at once
 * if immediate */
static void program_address(struct navi *n, int p, uint64_t mc, bool immediate) {
    const struct dcn_pipe_regs *r = &dcn20_pipe[p];
    wr(n, r->flip_control, (rd(n, r->flip_control) & ~SURFACE_FLIP_TYPE) | (immediate ? SURFACE_FLIP_TYPE : 0));
    wr(n, r->pri_addr_high, (uint32_t)(mc >> 32) & ADDR_HIGH_MASK);
    wr(n, r->pri_addr, (uint32_t)mc);
}
/* Copy what the screen shows into a scanout buffer. fb.c composes every frame
 * in a RAM canvas, which is far faster to read than VRAM through the BAR; the
 * VRAM buffer at src_off is the fallback if the canvas does not match. */
static void fill_buffer(struct navi *n, uint64_t dst_off, uint64_t src_off) {
    uint8_t *dst = vram_ptr(n, dst_off);
    size_t pitch = (size_t)n->pitch_px * 4;
    if (screen && screen->px && screen->w == (int)n->width && screen->h == (int)n->height) {
        for (uint32_t y = 0; y < n->height; y++) memcpy(dst + y * pitch, screen->px + (size_t)y * screen->stride, (size_t)n->width * 4);
    } else if (dst_off != src_off) memcpy(dst, vram_ptr(n, src_off), n->fb_size);
    wmb();
}
/* hubp2_is_flip_pending: done once nothing is pending and the pipe fetches from the new address */
static bool flip_pending(struct navi *n, int p, uint64_t mc) {
    const struct dcn_pipe_regs *r = &dcn20_pipe[p];
    if (rd(n, r->flip_control) & SURFACE_FLIP_PENDING) return true;
    uint64_t inuse = ((uint64_t)(rd(n, r->inuse_high) & ADDR_HIGH_MASK) << 32) | rd(n, r->inuse);
    return inuse != mc;
}
/* A flip that never completes would stall every frame and leave the pipes
 * showing whichever buffer they last latched. Give up on page flipping: show
 * buffer 0 (the bootloader's, which fb.c draws into directly without page
 * flipping) on every pipe, holding the frame just finished. */
static void flip_give_up(struct navi *n, int p, uint64_t mc) {
    const struct dcn_pipe_regs *r = &dcn20_pipe[p];
    kprintf("amdgpu: pipe %d: flip to %lx not complete after 100 ms (flip control %08x, in use %04x%08x); page flipping off\n",
            p, mc, rd(n, r->flip_control), rd(n, r->inuse_high), rd(n, r->inuse));
    fill_buffer(n, n->buf_off[0], n->buf_off[n->back]);
    uint64_t front = n->fb_base_mc + n->buf_off[0];
    for (int i = 0; i < n->npipes; i++) program_address(n, n->pipes[i], front, true);   /* immediate: needs no VUPDATE */
    n->back = 0;   /* fb_present refreshes stale regions here: the visible buffer, from the same canvas */
    n->gpu.page_flip = false;
}
static void navi_flip(struct gpu_device *g) {
    struct navi *n = g->priv;
    if (!n->gpu.page_flip) return;
    uint64_t mc = n->fb_base_mc + n->buf_off[n->back];
    wmb();   /* the back buffer's contents reach VRAM before the pipes switch to it */
    for (int i = 0; i < n->npipes; i++) program_address(n, n->pipes[i], mc, false);
    /* the old front buffer is drawn into next, so wait until no pipe reads it
     * (bounded: 100 ms is more than a frame at 24 Hz) */
    for (int i = 0; i < n->npipes; i++) {
        int p = n->pipes[i], us = 0;
        while (flip_pending(n, p, mc) && us < 100000) { udelay(10); us += 10; }
        if (us >= 100000) { flip_give_up(n, p, mc); return; }
    }
    n->back ^= 1;
}

/* ---- hardware cursor ---- */
/* dcn10_mpc.c mpc1_cursor_lock: cursor register writes take effect together at the next VUPDATE */
static void cursor_lock(struct navi *n, int i, bool lock) {
    if (n->opp[i] >= 0) wr(n, cur_vupdate_lock[n->opp[i]], lock ? 1 : 0);
}
/* hubp2_cursor_set_attributes + dpp2_set_cursor_attributes */
static void cursor_upload(struct navi *n) {
    if (!n->cursor_valid) return;
    memcpy(vram_ptr(n, n->cursor_off), n->cursor_img, sizeof n->cursor_img);
    wmb();
    uint64_t mc = n->fb_base_mc + n->cursor_off;
    for (int i = 0; i < n->npipes; i++) {
        const struct dcn_pipe_regs *r = &dcn20_pipe[n->pipes[i]];
        cursor_lock(n, i, true);
        wr(n, r->cur_addr_high, (uint32_t)(mc >> 32) & ADDR_HIGH_MASK);
        wr(n, r->cur_addr, (uint32_t)mc);
        wr(n, r->cur_size, (64u << 16) | 64u);   /* CURSOR_WIDTH, CURSOR_HEIGHT */
        uint32_t ctl = rd(n, r->cur_control) & ~(CURSOR_MODE_MASK | CURSOR_2X_MAGNIFY | CURSOR_PITCH_MASK | CURSOR_LPC_MASK);
        wr(n, r->cur_control, ctl | (CURSOR_MODE_PREMUL << CURSOR_MODE_SHIFT) | (CURSOR_LINES_PER_CHUNK_8 << CURSOR_LPC_SHIFT));
        wr(n, r->cursor_settings, CURSOR_CHUNK_HDL_ADJUST(3));
        uint32_t dpp = rd(n, r->dpp_cur_control) & ~(CUR0_MODE_MASK | CUR0_EXPANSION_MODE | CUR0_ROM_EN);
        wr(n, r->dpp_cur_control, dpp | (CURSOR_MODE_PREMUL << CUR0_MODE_SHIFT));
        cursor_lock(n, i, false);
    }
}
static bool navi_set_cursor(struct gpu_device *g, const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    struct navi *n = g->priv;
    (void)hot_x; (void)hot_y;   /* callers pass the image's top-left to move_cursor */
    if (w > 64 || h > 64) return false;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint32_t p = (x < w && y < h) ? argb[y * w + x] : 0;
        uint32_t a = p >> 24;
        uint32_t rr = ((p >> 16) & 0xFF) * a / 255, gg = ((p >> 8) & 0xFF) * a / 255, bb = (p & 0xFF) * a / 255;
        n->cursor_img[y * 64 + x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }
    n->cursor_valid = true;
    cursor_upload(n);
    return true;
}
/* hubp2_cursor_set_position / dpp1_set_cursor_position. The position is in
 * surface coordinates; a cursor partly off the top/left edge sits at 0 with
 * the hot spot moved into the image (dcn10_set_cursor_position). */
static void navi_move_cursor(struct gpu_device *g, int x, int y, bool visible) {
    struct navi *n = g->priv;
    if (!n->cursor_valid) return;
    int hx = 0, hy = 0;
    if (x < 0) { hx = MIN(-x, 63); x = 0; }
    if (y < 0) { hy = MIN(-y, 63); y = 0; }
    for (int i = 0; i < n->npipes; i++) {
        const struct dcn_pipe_regs *r = &dcn20_pipe[n->pipes[i]];
        /* hide it on a pipe whose viewport (a slice of the surface under pipe split) it misses */
        int sx = x - (int)n->vp_x[i] - hx, sy = y - (int)n->vp_y[i] - hy;
        bool en = visible && sx < (int)n->vp_w[i] && sx + 64 > 0 && sy < (int)n->vp_h[i] && sy + 64 > 0;
        cursor_lock(n, i, true);
        wr(n, r->cur_control, (rd(n, r->cur_control) & ~CURSOR_ENABLE) | (en ? CURSOR_ENABLE : 0));
        wr(n, r->cur_position, ((uint32_t)x << 16) | (uint32_t)y);
        wr(n, r->cur_hot_spot, ((uint32_t)hx << 16) | (uint32_t)hy);
        wr(n, r->cur_dst_offset, 0);
        wr(n, r->dpp_cur_control, (rd(n, r->dpp_cur_control) & ~CUR0_ENABLE) | (en ? CUR0_ENABLE : 0));
        cursor_lock(n, i, false);
    }
}

/* ---- probe ---- */
static uint8_t pci_revision(struct pci_device *pd) { return (uint8_t)pci_read32(pd->bus, pd->slot, pd->func, 0x08); }

/* log one display pipe; true if it is a linear 32bpp surface at mc */
static bool inspect_pipe(struct navi *n, int p, uint64_t want_mc, uint32_t want_pitch_px, uint32_t fb_w, uint32_t fb_h) {
    const struct dcn_pipe_regs *r = &dcn20_pipe[p];
    uint32_t clk = rd(n, r->hubp_clk_cntl), cntl = rd(n, r->dchubp_cntl);
    if (!(clk & HUBP_CLOCK_ENABLE)) { kprintf("amdgpu: pipe %d: off\n", p); return false; }
    uint64_t mc = ((uint64_t)(rd(n, r->pri_addr_high) & ADDR_HIGH_MASK) << 32) | rd(n, r->pri_addr);
    uint32_t cfg = rd(n, r->surface_config), tiling = rd(n, r->tiling_config), sctl = rd(n, r->surface_control);
    uint32_t vs = rd(n, r->viewport_start), vd = rd(n, r->viewport_dim);
    uint32_t pitch = (rd(n, r->surface_pitch) & PITCH_MASK) + 1;
    int otg = HUBP_VTG_SEL(cntl), opp = rd(n, r->mpcc_opp_id) & MPCC_OPP_ID_MASK;
    uint32_t otg_on = otg < NAVI14_PIPES ? rd(n, otg_control[otg]) & OTG_MASTER_EN : 0;
    /* hubp2_set_vm_system_aperture_settings: [low, high] in 256 KiB units */
    uint64_t apt_lo = (uint64_t)(rd(n, r->aperture_low) & APERTURE_MASK) << APERTURE_SHIFT;
    uint64_t apt_hi = ((uint64_t)(rd(n, r->aperture_high) & APERTURE_MASK) << APERTURE_SHIFT) | ((1ull << APERTURE_SHIFT) - 1);
    kprintf("amdgpu: pipe %d: %s, otg%d %s (%u x %u total), opp %d, surface %lx, viewport %ux%u+%u+%u, pitch %u px, format %u, swizzle %u%s%s, aperture %lx-%lx\n",
            p, cntl & (HUBP_BLANK_EN | HUBP_DISABLE) ? "blanked" : "scanning out", otg, otg_on ? "on" : "off",
            otg < NAVI14_PIPES ? (rd(n, otg_h_total[otg]) & 0x7FFF) + 1 : 0, otg < NAVI14_PIPES ? (rd(n, otg_v_total[otg]) & 0x7FFF) + 1 : 0,
            opp, mc, VIEWPORT_LO(vd), VIEWPORT_HI(vd), VIEWPORT_LO(vs), VIEWPORT_HI(vs), pitch, cfg & SURFACE_PIXEL_FORMAT_MASK, tiling & SW_MODE_MASK,
            sctl & PRIMARY_SURFACE_DCC_EN ? ", DCC" : "", cfg & (ROTATION_ANGLE_MASK | H_MIRROR_EN) ? ", rotated/mirrored" : "", apt_lo, apt_hi);
    if ((cntl & (HUBP_BLANK_EN | HUBP_DISABLE)) || !otg_on || mc != want_mc) return false;
    const char *why = NULL;
    if ((cfg & SURFACE_PIXEL_FORMAT_MASK) != SURFACE_PIXEL_FORMAT_8888) why = "not 32bpp";
    else if (tiling & SW_MODE_MASK) why = "not linear";
    else if (sctl & PRIMARY_SURFACE_DCC_EN) why = "compressed (DCC)";
    else if (cfg & (ROTATION_ANGLE_MASK | H_MIRROR_EN)) why = "rotated or mirrored";
    else if (pitch != want_pitch_px) why = "pitch differs from the bootloader's";
    else if (VIEWPORT_LO(vs) + VIEWPORT_LO(vd) > fb_w || VIEWPORT_HI(vs) + VIEWPORT_HI(vd) > fb_h) why = "viewport exceeds the framebuffer";
    if (why) { kprintf("amdgpu: pipe %d shows the bootloader framebuffer but it is %s\n", p, why); return false; }
    int k = n->npipes++;
    n->pipes[k] = p;
    n->opp[k] = opp < NAVI14_PIPES ? opp : -1;
    n->vp_x[k] = VIEWPORT_LO(vs); n->vp_y[k] = VIEWPORT_HI(vs); n->vp_w[k] = VIEWPORT_LO(vd); n->vp_h[k] = VIEWPORT_HI(vd);
    n->apt_lo[k] = apt_lo; n->apt_hi[k] = apt_hi;
    return true;
}

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) { return blen && a < b + blen && b < a + alen; }
static bool within(uint64_t a, uint64_t alen, uint64_t lo, uint64_t hi) { return alen && lo <= hi && a >= lo && a + alen - 1 <= hi; }

/* A pipe fetches addresses inside its system aperture directly and the rest
 * through GPUVM page tables, which nothing set up for us. If the bootloader
 * framebuffer is inside, the new buffers must be too, or the first flip shows
 * garbage with nothing in the log. If it is outside, the firmware reaches VRAM
 * some other way, presumably the same way for the rest of it. */
static const char *check_aperture(struct navi *n, uint64_t want_mc, uint64_t fb_size, uint64_t new_mc, uint64_t new_len) {
    for (int i = 0; i < n->npipes; i++) {
        if (!within(want_mc, fb_size, n->apt_lo[i], n->apt_hi[i])) {
            kprintf("amdgpu: pipe %d: the bootloader framebuffer is outside the system aperture %lx-%lx; assuming the rest of VRAM is reachable the same way\n",
                    n->pipes[i], n->apt_lo[i], n->apt_hi[i]);
            continue;
        }
        if (!within(new_mc, new_len, n->apt_lo[i], n->apt_hi[i])) return "they would fall outside a display pipe's system aperture";
    }
    return NULL;
}

static bool probe(struct pci_device *pd) {
    struct navi *n = &ndev;
    memset(n, 0, sizeof *n);
    n->pd = pd;
    n->gpu.priv = n;
    strcpy(n->gpu.driver, "amdgpu");
    strcpy(n->gpu.chip, "Navi 14");
    uint8_t rev = pci_revision(pd);
    const struct navi_model *m = NULL;
    for (size_t i = 0; i < ARRAY_LEN(models); i++) if (models[i].id == pd->device && (models[i].rev == rev || (!m && models[i].rev == 0))) m = &models[i];
    if (m) snprintf(n->gpu.name, sizeof n->gpu.name, "AMD %s", m->name);
    else snprintf(n->gpu.name, sizeof n->gpu.name, "AMD Navi 14 [%04x rev %02x]", pd->device, rev);
    n->gpu.stream_processors = m ? m->sps : 1408;
    snprintf(n->gpu.bus, sizeof n->gpu.bus, "PCI %02x:%02x.%d", pd->bus, pd->slot, pd->func);
    /* BAR0: VRAM (64-bit, prefetchable); BAR2: doorbells; BAR5: registers */
    if (!pd->bar_is_mmio[0] || !pd->bar_addr[0] || !pd->bar_is_mmio[5] || !pd->bar_addr[5]) {
        kprintf("amdgpu: %s: BARs not assigned (vram %lx, mmio %lx)\n", n->gpu.name, pd->bar_addr[0], pd->bar_addr[5]);
        return false;
    }
    pci_enable_bus_master(pd);
    n->vram_bar = pd->bar_addr[0];
    n->mmio = vmm_map_mmio(pd->bar_addr[5], MMIO_SIZE);
    n->gpu.mmio_base = pd->bar_addr[5]; n->gpu.vram_base = n->vram_bar;
    kprintf("amdgpu: %s (Navi 14, rev %02x) at %s, registers at %lx, VRAM BAR at %lx\n", n->gpu.name, rev, n->gpu.bus, pd->bar_addr[5], n->vram_bar);

    uint32_t memsize = rd(n, RCC_CONFIG_MEMSIZE);
    uint32_t fbloc = rd(n, MC_VM_FB_LOCATION_BASE), fbtop = rd(n, MC_VM_FB_LOCATION_TOP);
    if (memsize == 0 || memsize == 0xFFFFFFFF || fbloc == 0xFFFFFFFF) {
        kprintf("amdgpu: registers read back %08x/%08x: the GPU is not responding; software path\n", memsize, fbloc);
        gpu_register(&n->gpu);
        return true;
    }
    n->gpu.vram_bytes = (uint64_t)memsize << 20;
    n->fb_base_mc = (uint64_t)(fbloc & FB_LOCATION_MASK) << 24;
    uint64_t fb_top_mc = ((uint64_t)(fbtop & FB_LOCATION_MASK) << 24) | 0xFFFFFF;
    kprintf("amdgpu: VRAM %u MiB, MC framebuffer %lx-%lx\n", memsize, n->fb_base_mc, fb_top_mc);

    if (find_rom(n, pd)) parse_atomfirmware(n);

    /* the bootloader's framebuffer: a VRAM offset inside BAR0 (the BAR starts at VRAM offset 0) */
    struct limine_framebuffer *fb = framebuffer_request.response && framebuffer_request.response->framebuffer_count ? framebuffer_request.response->framebuffers[0] : NULL;
    uint64_t fb_phys = fb ? (uint64_t)fb->address - hhdm_offset : 0;
    if (!fb || fb->bpp != 32 || fb_phys < n->vram_bar || fb_phys >= n->vram_bar + VRAM_BAR_MIN) {
        kprintf("amdgpu: the bootloader framebuffer (%lx) is not in this GPU's VRAM BAR; software path\n", fb_phys);
        gpu_register(&n->gpu);
        return true;
    }
    uint64_t scan_off = fb_phys - n->vram_bar;
    uint64_t want_mc = n->fb_base_mc + scan_off;
    n->pitch_px = (uint32_t)(fb->pitch / 4); n->width = (uint32_t)fb->width; n->height = (uint32_t)fb->height;
    kprintf("amdgpu: bootloader framebuffer %ux%u, pitch %u px, at VRAM offset %lx (MC %lx)\n", n->width, n->height, n->pitch_px, scan_off, want_mc);
    for (int p = 0; p < NAVI14_PIPES; p++) inspect_pipe(n, p, want_mc, n->pitch_px, n->width, n->height);
    if (!n->npipes) {
        kprintf("amdgpu: no display pipe scans out the bootloader framebuffer; software path\n");
        gpu_register(&n->gpu);
        return true;
    }
    n->gpu.scanout_verified = true;

    /* a second buffer right after the firmware's, then the cursor image */
    uint64_t fb_size = (uint64_t)fb->pitch * n->height;
    n->fb_size = fb_size;
    n->buf_off[0] = scan_off;
    n->buf_off[1] = ALIGN_UP(scan_off + fb_size, 0x10000);
    n->cursor_off = ALIGN_UP(n->buf_off[1] + fb_size, 0x1000);
    uint64_t end = n->cursor_off + sizeof n->cursor_img;
    const char *why = NULL;
    if (end > VRAM_BAR_MIN) why = "they would run past the first 256 MiB of the VRAM BAR";
    else if (end > n->gpu.vram_bytes) why = "VRAM is too small";
    else if (overlaps(n->buf_off[1], end - n->buf_off[1], n->fw_resv_start, n->fw_resv_size)) why = "they would overlap the VRAM the firmware reserved";
    else why = check_aperture(n, want_mc, fb_size, n->fb_base_mc + n->buf_off[1], end - n->buf_off[1]);
    if (why) {
        kprintf("amdgpu: scanout verified, but no room for a back buffer and cursor at %lx-%lx: %s; software path\n", n->buf_off[1], end, why);
        gpu_register(&n->gpu);
        return true;
    }
    n->win_off = scan_off;
    n->win = vmm_map_wc(n->vram_bar + n->win_off, end - n->win_off);
    fill_buffer(n, n->buf_off[1], n->buf_off[0]);
    n->back = 1;
    n->gpu.fb_phys = fb_phys; n->gpu.fb_pitch = (int)fb->pitch; n->gpu.fb_width = (int)n->width; n->gpu.fb_height = (int)n->height;
    n->gpu.page_flip = true; n->gpu.back_buffer = navi_back_buffer; n->gpu.flip = navi_flip;
    n->gpu.hw_cursor = true; n->gpu.set_cursor = navi_set_cursor; n->gpu.move_cursor = navi_move_cursor;
    char list[32] = ""; size_t len = 0;
    for (int i = 0; i < n->npipes; i++) len += snprintf(list + len, sizeof list - len, "%s%d", i ? "," : "", n->pipes[i]);
    kprintf("amdgpu: scanout verified on pipe%s %s; page flipping (back buffer at VRAM %lx) and hardware cursor enabled\n", n->npipes > 1 ? "s" : "", list, n->buf_off[1]);
    gpu_register(&n->gpu);
    return true;
}

void amdgpu_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->vendor != 0x1002 || pd->class_code != 0x03 || !amdgpu_supported(pd->device)) continue;
        if (probe(pd)) return;
    }
}
