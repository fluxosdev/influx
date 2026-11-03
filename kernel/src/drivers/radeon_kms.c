/*
 * ATI Radeon R700 mode setting through the AtomBIOS command tables.
 *
 * The sequence, the parameter blocks and the PLL search follow the Linux
 * radeon driver (atombios_crtc.c, atombios_encoders.c, radeon_atombios.c,
 * radeon_display.c, radeon_i2c.c; Copyright 2007-8 Advanced Micro Devices,
 * Inc., 2008 Red Hat Inc., 2009 Jerome Glisse; MIT licence, see atom.h),
 * restricted to the DCE 3.0/3.2 display engine of the HD 4000 series:
 *   - the output the firmware lit is found in the BIOS scratch registers and
 *     the object tables (connector, encoder, DDC line)
 *   - EDID is read over the DDC GPIO lines (bit-banged I2C) for the mode list
 *   - PLL dividers (radeon_compute_pll_avivo), AdjustDisplayPll, SetPixelClock
 *   - CRTC timing (SetCRTC_UsingDTDTiming), surface registers, scaler off
 *   - DAC (VGA) and DIG/UNIPHY/LVTMA (DVI; HDMI is driven in DVI mode) encoders
 * Not handled: DisplayPort (needs link training), LVDS panels, TV-out,
 * external encoders and a second display; those keep the boot mode.
 */
#include "radeon.h"
#include "atom.h"
#include "edid.h"
#include "mm.h"
#include "string.h"
#include "io.h"

/* ---- registers ---- */
#define MM_INDEX                   0x0000
#define MM_DATA                    0x0004
#define D1VGA_CONTROL              0x0330
#define D2VGA_CONTROL              0x0338
#define BIOS_0_SCRATCH             0x1724
#define BIOS_2_SCRATCH             0x172C
#define BIOS_3_SCRATCH             0x1730
#define BIOS_6_SCRATCH             0x173C
#define MC_INDEX                   0x28F8
#define MC_DATA                    0x28FC
#define D1CRTC_CONTROL             0x6080
#define D1CRTC_STATUS_POSITION     0x60A0
#define D1MODE_MASTER_UPDATE_MODE  0x60E4
#define D1GRPH_ENABLE              0x6100
#define D1GRPH_CONTROL             0x6104
#define D1GRPH_LUT_SEL             0x6108
#define D1GRPH_SWAP_CONTROL        0x610C
#define D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define D1GRPH_PITCH               0x6120
#define D1GRPH_SURFACE_OFFSET_X    0x6124
#define D1GRPH_SURFACE_OFFSET_Y    0x6128
#define D1GRPH_X_START             0x612C
#define D1GRPH_Y_START             0x6130
#define D1GRPH_X_END               0x6134
#define D1GRPH_Y_END               0x6138
#define D1GRPH_FLIP_CONTROL        0x6148
#define D1MODE_DATA_FORMAT         0x6528
#define D1MODE_DESKTOP_HEIGHT      0x652C
#define D1MODE_VIEWPORT_START      0x6580
#define D1MODE_VIEWPORT_SIZE       0x6584
#define D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH   0x6914
#define D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH 0x691C
#define D2GRPH_PRIMARY_SURFACE_ADDRESS_HIGH   0x6114
#define D2GRPH_SECONDARY_SURFACE_ADDRESS_HIGH 0x611C
#define GRPH_DEPTH_32BPP           2
#define LUT_10BIT_BYPASS_EN        (1u << 8)

/* ---- AtomBIOS constants (atombios.h, ObjectID.h) ---- */
#define ATOM_DISABLE 0
#define ATOM_ENABLE  1
#define ATOM_TRANSMITTER_ACTION_DISABLE 0
#define ATOM_TRANSMITTER_ACTION_ENABLE  1
#define ATOM_TRANSMITTER_ACTION_INIT    7
#define ATOM_TRANSMITTER_ACTION_SETUP   10
#define ATOM_ENCODER_MODE_DVI 2
#define ATOM_ENCODER_MODE_CRT 15
#define ATOM_DAC1_PS2 1
#define ATOM_SCALER_DISABLE 0
#define ATOM_HSYNC_POLARITY 0x02
#define ATOM_VSYNC_POLARITY 0x04
#define ATOM_ENCODER_CONFIG_LINKB          0x04
#define ATOM_ENCODER_CONFIG_V2_TRANSMITTER1 0x00
#define ATOM_ENCODER_CONFIG_V2_TRANSMITTER2 0x08
#define ATOM_ENCODER_CONFIG_V2_TRANSMITTER3 0x10
#define ATOM_TRANSMITTER_CONFIG_8LANE_LINK   0x01
#define ATOM_TRANSMITTER_CONFIG_COHERENT     0x02
#define ATOM_TRANSMITTER_CONFIG_LINKB        0x04
#define ATOM_TRANSMITTER_CONFIG_DIG2_ENCODER 0x08
#define PIXEL_CLOCK_MISC_CRTC_SEL_CRTC2 0x04
#define DISPPLL_CONFIG_COHERENT_MODE 0x0020
#define DISPPLL_CONFIG_DUAL_LINK     0x0040
#define PANEL_8BIT_PER_COLOR 0x02
#define ASIC_INT_DAC1_ENCODER_ID 0x00
#define ASIC_INT_DAC2_ENCODER_ID 0x04
#define ASIC_INT_DIG1_ENCODER_ID 0x03
#define ASIC_INT_DIG2_ENCODER_ID 0x09
#define ATOM_S2_VRI_BRIGHT_ENABLE        0x20000000u
#define ATOM_S6_ACC_MODE                 0x00000010u
#define ATOM_S6_CRITICAL_STATE           0x00000100u
#define ATOM_S6_ACC_BLOCK_DISPLAY_SWITCH 0x20000000u
#define ATOM_DEVICE_CRT_SUPPORT 0x0011u
#define ATOM_DEVICE_LCD_SUPPORT 0x0022u
#define ATOM_DEVICE_TV_SUPPORT  0x0004u
#define ATOM_DEVICE_CV_SUPPORT  0x0100u
#define ATOM_DEVICE_DFP_SUPPORT 0x0EC8u
#define GRAPH_OBJECT_TYPE_ENCODER 2
#define GRAPH_OBJECT_TYPE_ROUTER  4
#define ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1  0x15
#define ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC2  0x16
#define ENCODER_OBJECT_ID_INTERNAL_UNIPHY       0x1E
#define ENCODER_OBJECT_ID_INTERNAL_KLDSCP_LVTMA 0x1F
#define ENCODER_OBJECT_ID_INTERNAL_UNIPHY1      0x20
#define ENCODER_OBJECT_ID_INTERNAL_UNIPHY2      0x21
#define CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I 0x02
#define CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D 0x04
#define CONNECTOR_OBJECT_ID_VGA             0x05
#define CONNECTOR_OBJECT_ID_HDMI_TYPE_B     0x0D
#define CONNECTOR_OBJECT_ID_LVDS            0x0E
#define CONNECTOR_OBJECT_ID_PCIE_CONNECTOR  0x10
#define CONNECTOR_OBJECT_ID_DISPLAYPORT     0x13
#define CONNECTOR_OBJECT_ID_eDP             0x14
#define CONNECTOR_OBJECT_ID_LVDS_eDP        0x16
#define ATOM_I2C_RECORD_TYPE 1
#define ATOM_MAX_OBJECT_RECORD_NUMBER 21

/* ---- command table parameter blocks; layouts match atombios.h (checked on the host) ---- */
struct PACKED pixel_clock_v2 { uint16_t pixel_clock, ref_div, fb_div; uint8_t post_div, frac_fb_div, ppll, ref_div_src, crtc, misc; };
struct PACKED pixel_clock_v3 { uint16_t pixel_clock, ref_div, fb_div; uint8_t post_div, frac_fb_div, ppll, transmitter_id, encoder_mode, misc; };
struct PACKED adjust_pll_v1 { uint16_t pixel_clock; uint8_t transmitter_id, encode_mode, config, reserved[3]; };
struct PACKED adjust_pll_v3_in { uint16_t pixel_clock; uint8_t transmitter_id, encode_mode, disp_pll_config, ext_transmitter_id, reserved[2]; };
struct PACKED adjust_pll_v3_out { uint32_t disp_pll_freq; uint8_t ref_div, post_div, reserved[2]; };
struct PACKED dtd_timing { uint16_t h_size, h_blank, v_size, v_blank, h_sync_offset, h_sync_width, v_sync_offset, v_sync_width, misc; uint8_t h_border, v_border, crtc, padding[3]; };
struct PACKED crtc_enable { uint8_t crtc, enable, padding[2]; };
struct PACKED crtc_blank { uint8_t crtc, blanking; uint16_t black_r, black_g, black_b; };
struct PACKED scaler_enable { uint8_t scaler, enable, tv_standard, padding; };
struct PACKED crtc_overscan { uint16_t right, left, bottom, top; uint8_t crtc, padding[3]; };
struct PACKED dig_encoder { uint16_t pixel_clock; uint8_t config, action, encoder_mode, lane_num, bpc, reserved; };   /* v1; v3 reads bpc */
struct PACKED dig_transmitter { uint16_t pixel_clock; uint8_t config, action, lane_num, reserved[3]; };    /* v1/v2; v3 reads lane_num */
struct PACKED dac_encoder { uint16_t pixel_clock; uint8_t dac_standard, action; };
struct PACKED output_control { uint8_t action, padding[3]; };
struct PACKED crtc_source_v2 { uint8_t crtc, encoder_id, encode_mode, padding; };
struct PACKED yuv_enable { uint8_t enable, crtc, padding[2]; };
struct PACKED ss_on_ppll { uint16_t percentage; uint8_t type, step, enable, delay, range, ppll; };

/* ---- state ---- */
struct ddc_gpio {
    uint32_t mask_clk_reg, mask_data_reg, en_clk_reg, en_data_reg, y_clk_reg, y_data_reg, a_clk_reg, a_data_reg;   /* byte offsets */
    uint32_t mask_clk, mask_data, en_clk, en_data, y_clk, y_data, a_clk, a_data;                                   /* bit masks */
    bool hw_capable, valid;
};
struct output {
    uint16_t device;            /* ATOM_DEVICE_*_SUPPORT bit of this display path */
    uint16_t conn_obj;          /* connector object id (type, enum, id) */
    uint16_t enc_obj;           /* internal encoder object id */
    uint8_t conn_type, enc_id;
    bool linkb, external, router;
    struct ddc_gpio ddc;
};
struct pll {
    uint32_t reference_freq, reference_div, post_div;      /* reference in 10 kHz */
    uint32_t pll_in_min, pll_in_max, pll_out_min, pll_out_max;
    uint32_t min_ref_div, max_ref_div, min_post_div, max_post_div, min_feedback_div, max_feedback_div;
    uint32_t flags;
};
#define PLL_USE_REF_DIV            (1 << 2)
#define PLL_USE_FRAC_FB_DIV        (1 << 10)
#define PLL_USE_POST_DIV           (1 << 12)
#define PLL_PREFER_MINM_OVER_MAXP  (1 << 14)

struct radeon_kms {
    struct atom_ctx atom;
    struct atom_card card;
    struct output outputs[16]; int noutputs;
    struct output *out;                  /* the output being driven */
    struct pll pll;
    uint32_t max_pixel_clock;            /* kHz */
    uint64_t vram_limit;                 /* usable VRAM bytes from offset 0 */
    struct edid_info edid;
    struct display_timing cur;           /* timing currently programmed (if known) */
    bool cur_valid, transmitter_inited;
    int dig_encoder;
};

/* ---- AtomBIOS card callbacks ---- */
static uint32_t cb_reg_read(void *p, uint32_t reg) { return radeon_rreg(p, reg * 4); }
static void cb_reg_write(void *p, uint32_t reg, uint32_t v) { radeon_wreg(p, reg * 4, v); }
static uint32_t cb_io_read(void *p, uint32_t reg) {
    struct radeon *r = p;
    if (!r->io_port) return radeon_rreg(r, reg * 4);   /* as Linux: fall back to MMIO without an I/O BAR */
    if (reg * 4 < 0x100) return inl((uint16_t)(r->io_port + reg * 4));
    outl(r->io_port + MM_INDEX, reg * 4); return inl((uint16_t)(r->io_port + MM_DATA));
}
static void cb_io_write(void *p, uint32_t reg, uint32_t v) {
    struct radeon *r = p;
    if (!r->io_port) { radeon_wreg(r, reg * 4, v); return; }
    if (reg * 4 < 0x100) { outl((uint16_t)(r->io_port + reg * 4), v); return; }
    outl(r->io_port + MM_INDEX, reg * 4); outl(r->io_port + MM_DATA, v);
}
static uint32_t cb_mc_read(void *p, uint32_t reg) {
    struct radeon *r = p;
    if (!r->is_rs880) { kprintf("radeon: AtomBIOS read MC register %x (unsupported)\n", reg); return 0; }
    radeon_wreg(r, MC_INDEX, reg & 0x1FF);
    uint32_t v = radeon_rreg(r, MC_DATA);
    radeon_wreg(r, MC_INDEX, ~0xFFFFFE00u);
    return v;
}
static void cb_mc_write(void *p, uint32_t reg, uint32_t v) {
    struct radeon *r = p;
    if (!r->is_rs880) { kprintf("radeon: AtomBIOS write MC register %x (unsupported)\n", reg); return; }
    radeon_wreg(r, MC_INDEX, (reg & 0x1FF) | 1u << 9);
    radeon_wreg(r, MC_DATA, v);
    radeon_wreg(r, MC_INDEX, 0x7F);
}
static uint32_t cb_pll_read(void *p, uint32_t reg) { kprintf("radeon: AtomBIOS read PLL register %x (unsupported)\n", reg); return 0; }
static void cb_pll_write(void *p, uint32_t reg, uint32_t v) { kprintf("radeon: AtomBIOS write PLL register %x (unsupported)\n", reg); }
static void cb_delay(void *p, uint32_t us) { radeon_udelay(us); }
static uint64_t cb_now(void *p) { uint64_t hz = cpu_tsc_hz(); return hz ? rdtsc() / (hz / 1000) : system_ticks; }

static int run(struct radeon_kms *k, int index, void *args, size_t n, const char *what) {
    int ret = atom_execute(&k->atom, index, args, n);
    if (ret) kprintf("radeon: AtomBIOS %s (table %d) failed\n", what, index);
    return ret;
}

/* ---- DDC: bit-banged I2C on the GPIO pads (radeon_i2c.c) ---- */
static void ddc_prepare(struct radeon *r, const struct ddc_gpio *d) {
    if (d->hw_capable) radeon_wreg(r, d->mask_clk_reg, radeon_rreg(r, d->mask_clk_reg) & ~(1u << 16));   /* switch the pads to DDC mode */
    radeon_wreg(r, d->a_clk_reg, radeon_rreg(r, d->a_clk_reg) & ~d->a_clk);    /* outputs drive low when enabled */
    radeon_wreg(r, d->a_data_reg, radeon_rreg(r, d->a_data_reg) & ~d->a_data);
    radeon_wreg(r, d->en_clk_reg, radeon_rreg(r, d->en_clk_reg) & ~d->en_clk); /* released (input) */
    radeon_wreg(r, d->en_data_reg, radeon_rreg(r, d->en_data_reg) & ~d->en_data);
    radeon_wreg(r, d->mask_clk_reg, radeon_rreg(r, d->mask_clk_reg) | d->mask_clk);   /* software owns the pins */
    radeon_rreg(r, d->mask_clk_reg);
    radeon_wreg(r, d->mask_data_reg, radeon_rreg(r, d->mask_data_reg) | d->mask_data);
    radeon_rreg(r, d->mask_data_reg);
}
static void ddc_finish(struct radeon *r, const struct ddc_gpio *d) {
    radeon_wreg(r, d->mask_clk_reg, radeon_rreg(r, d->mask_clk_reg) & ~d->mask_clk);
    radeon_rreg(r, d->mask_clk_reg);
    radeon_wreg(r, d->mask_data_reg, radeon_rreg(r, d->mask_data_reg) & ~d->mask_data);
    radeon_rreg(r, d->mask_data_reg);
}
#define I2C_DELAY 10   /* us per half clock, as Linux (50 kHz) */
static void set_scl(struct radeon *r, const struct ddc_gpio *d, int v) { radeon_wreg(r, d->en_clk_reg, (radeon_rreg(r, d->en_clk_reg) & ~d->en_clk) | (v ? 0 : d->en_clk)); }
static void set_sda(struct radeon *r, const struct ddc_gpio *d, int v) { radeon_wreg(r, d->en_data_reg, (radeon_rreg(r, d->en_data_reg) & ~d->en_data) | (v ? 0 : d->en_data)); }
static int get_scl(struct radeon *r, const struct ddc_gpio *d) { return (radeon_rreg(r, d->y_clk_reg) & d->y_clk) != 0; }
static int get_sda(struct radeon *r, const struct ddc_gpio *d) { return (radeon_rreg(r, d->y_data_reg) & d->y_data) != 0; }
static bool scl_high(struct radeon *r, const struct ddc_gpio *d) {
    set_scl(r, d, 1);
    for (int i = 0; i < 2000 && !get_scl(r, d); i++) radeon_udelay(1);   /* the slave may stretch the clock */
    radeon_udelay(I2C_DELAY);
    return get_scl(r, d);
}
static void i2c_start(struct radeon *r, const struct ddc_gpio *d) { set_sda(r, d, 0); radeon_udelay(I2C_DELAY); set_scl(r, d, 0); radeon_udelay(I2C_DELAY); }
static void i2c_restart(struct radeon *r, const struct ddc_gpio *d) { set_sda(r, d, 1); scl_high(r, d); i2c_start(r, d); }
static void i2c_stop(struct radeon *r, const struct ddc_gpio *d) { set_sda(r, d, 0); radeon_udelay(I2C_DELAY); scl_high(r, d); set_sda(r, d, 1); radeon_udelay(I2C_DELAY); }
static bool i2c_write_byte(struct radeon *r, const struct ddc_gpio *d, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        set_sda(r, d, (b >> i) & 1); radeon_udelay(I2C_DELAY / 2);
        if (!scl_high(r, d)) return false;
        set_scl(r, d, 0); radeon_udelay(I2C_DELAY / 2);
    }
    set_sda(r, d, 1); radeon_udelay(I2C_DELAY / 2);
    if (!scl_high(r, d)) return false;
    bool ack = !get_sda(r, d);
    set_scl(r, d, 0); radeon_udelay(I2C_DELAY / 2);
    return ack;
}
static uint8_t i2c_read_byte(struct radeon *r, const struct ddc_gpio *d, bool ack) {
    uint8_t b = 0;
    set_sda(r, d, 1);
    for (int i = 0; i < 8; i++) {
        scl_high(r, d);
        b = (uint8_t)(b << 1 | get_sda(r, d));
        set_scl(r, d, 0); radeon_udelay(I2C_DELAY);
    }
    set_sda(r, d, !ack); radeon_udelay(I2C_DELAY / 2);
    scl_high(r, d);
    set_scl(r, d, 0); set_sda(r, d, 1); radeon_udelay(I2C_DELAY / 2);
    return b;
}
static bool ddc_read_block(struct radeon *r, const struct ddc_gpio *d, uint8_t offset, uint8_t *buf) {
    bool ok = false;
    i2c_start(r, d);
    if (i2c_write_byte(r, d, 0xA0) && i2c_write_byte(r, d, offset)) {
        i2c_restart(r, d);
        if (i2c_write_byte(r, d, 0xA1)) {
            for (int i = 0; i < 128; i++) buf[i] = i2c_read_byte(r, d, i < 127);
            ok = true;
        }
    }
    i2c_stop(r, d);
    return ok;
}
/* EDID base block plus at most one extension; 0 bytes if nothing answers */
static size_t ddc_read_edid(struct radeon *r, const struct ddc_gpio *d, uint8_t *buf) {
    if (!d->valid) return 0;
    size_t len = 0;
    ddc_prepare(r, d);
    set_scl(r, d, 1); set_sda(r, d, 1); radeon_udelay(I2C_DELAY);
    for (int tries = 0; tries < 3 && !len; tries++)
        if (ddc_read_block(r, d, 0, buf) && edid_block_valid(buf) && buf[0] == 0 && buf[1] == 0xFF) len = 128;
    if (len && buf[126])
        for (int tries = 0; tries < 3; tries++) if (ddc_read_block(r, d, 128, buf + 128) && edid_block_valid(buf + 128)) { len = 256; break; }
    ddc_finish(r, d);
    return len;
}

/* ---- BIOS object tables (radeon_atombios.c) ---- */
static struct ddc_gpio lookup_i2c(struct radeon_kms *k, uint8_t id) {
    struct ddc_gpio d = { 0 };
    uint16_t size, off;
    if (!atom_data_header(&k->atom, ATOM_DATA_GPIO_I2C_Info, &size, 0, 0, &off)) return d;
    int n = (size - 4) / 27;
    for (int i = 0; i < n; i++) {
        uint32_t g = off + 4 + i * 27u;
        uint16_t clk_mask_reg = atom_u16(&k->atom, g);
        uint8_t access = atom_u8(&k->atom, g + 16);
        if (i == 4 && clk_mask_reg == 0x1FDA && access == 0x94) access = 0x14;   /* some DCE3 boards have bad data here */
        if (access != id) continue;
        d.mask_clk_reg = clk_mask_reg * 4u;                d.en_clk_reg = atom_u16(&k->atom, g + 2) * 4u;
        d.y_clk_reg = atom_u16(&k->atom, g + 4) * 4u;      d.a_clk_reg = atom_u16(&k->atom, g + 6) * 4u;
        d.mask_data_reg = atom_u16(&k->atom, g + 8) * 4u;  d.en_data_reg = atom_u16(&k->atom, g + 10) * 4u;
        d.y_data_reg = atom_u16(&k->atom, g + 12) * 4u;    d.a_data_reg = atom_u16(&k->atom, g + 14) * 4u;
        d.mask_clk = 1u << atom_u8(&k->atom, g + 17); d.en_clk = 1u << atom_u8(&k->atom, g + 18);
        d.y_clk = 1u << atom_u8(&k->atom, g + 19);    d.a_clk = 1u << atom_u8(&k->atom, g + 20);
        d.mask_data = 1u << atom_u8(&k->atom, g + 21); d.en_data = 1u << atom_u8(&k->atom, g + 22);
        d.y_data = 1u << atom_u8(&k->atom, g + 23);    d.a_data = 1u << atom_u8(&k->atom, g + 24);
        d.hw_capable = access & 0x80;
        d.valid = d.mask_clk_reg != 0;
        break;
    }
    return d;
}
static bool encoder_supported(uint8_t id) {
    switch (id) {
    case ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1: case ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC2:
    case ENCODER_OBJECT_ID_INTERNAL_UNIPHY: case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1: case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
    case ENCODER_OBJECT_ID_INTERNAL_KLDSCP_LVTMA: return true;
    }
    return false;
}
static bool is_dig(uint8_t id) { return id != ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1 && id != ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC2; }
static void parse_objects(struct radeon_kms *k) {
    struct atom_ctx *a = &k->atom;
    uint16_t size, off; uint8_t frev, crev;
    if (!atom_data_header(a, ATOM_DATA_Object_Header, &size, &frev, &crev, &off) || crev < 2) { kprintf("radeon: no BIOS object table\n"); return; }
    uint16_t dev_support = atom_u16(a, off + 4);
    uint32_t conn_tbl = off + atom_u16(a, off + 6), enc_tbl = off + atom_u16(a, off + 10), path_tbl = off + atom_u16(a, off + 14);
    int npaths = atom_u8(a, path_tbl);
    uint32_t p = path_tbl + 4;
    for (int i = 0; i < npaths && k->noutputs < (int)ARRAY_LEN(k->outputs); i++) {
        uint16_t tag = atom_u16(a, p), psize = atom_u16(a, p + 2), conn = atom_u16(a, p + 4);
        if (psize < 8) break;
        if ((dev_support & tag) && !(tag & (ATOM_DEVICE_TV_SUPPORT | ATOM_DEVICE_CV_SUPPORT))) {
            struct output o = { .device = tag, .conn_obj = conn, .conn_type = conn & 0xFF };
            for (int j = 0; j < (psize - 8) / 2; j++) {
                uint16_t obj = atom_u16(a, p + 8 + 2 * j);
                int type = (obj & 0x7000) >> 12;
                if (type == GRAPH_OBJECT_TYPE_ROUTER) o.router = true;
                if (type != GRAPH_OBJECT_TYPE_ENCODER) continue;
                bool listed = false;   /* the path's encoder must be in the encoder table */
                for (int e = 0; e < atom_u8(a, enc_tbl); e++) if (atom_u16(a, enc_tbl + 4 + e * 8) == obj) listed = true;
                if (!listed) continue;
                if (encoder_supported(obj & 0xFF)) { o.enc_obj = obj; o.enc_id = obj & 0xFF; o.linkb = ((obj & 0x0700) >> 8) == 2; }
                else o.external = true;
            }
            /* the connector's records name its DDC line */
            for (int c = 0; c < atom_u8(a, conn_tbl); c++) {
                uint32_t obj = conn_tbl + 4 + c * 8u;
                if (atom_u16(a, obj) != conn) continue;
                uint32_t rec = off + atom_u16(a, obj + 4);
                for (int guard = 0; guard < 32; guard++) {
                    uint8_t type = atom_u8(a, rec), rsize = atom_u8(a, rec + 1);
                    if (!rsize || !type || type > ATOM_MAX_OBJECT_RECORD_NUMBER) break;
                    if (type == ATOM_I2C_RECORD_TYPE) o.ddc = lookup_i2c(k, atom_u8(a, rec + 2));
                    rec += rsize;
                }
                break;
            }
            if (o.enc_id) k->outputs[k->noutputs++] = o;
        }
        p += psize;
    }
}

/* ---- PLL (radeon_display.c) ---- */
static unsigned gcd_u(unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a; }
#define DIV_ROUND_CLOSEST(x, d) (((x) + (d) / 2) / (d))
#define DIV_ROUND_UP(x, d) (((x) + (d) - 1) / (d))
static void reduce_ratio(unsigned *nom, unsigned *den, unsigned nom_min, unsigned den_min) {
    unsigned tmp = gcd_u(*nom, *den);
    *nom /= tmp; *den /= tmp;
    if (*nom < nom_min) { tmp = DIV_ROUND_UP(nom_min, *nom); *nom *= tmp; *den *= tmp; }
    if (*den < den_min) { tmp = DIV_ROUND_UP(den_min, *den); *nom *= tmp; *den *= tmp; }
}
static void get_fb_ref_div(unsigned nom, unsigned den, unsigned post_div, unsigned fb_div_max, unsigned ref_div_max, unsigned *fb_div, unsigned *ref_div) {
    ref_div_max = MIN(100 / post_div, ref_div_max); if (ref_div_max < 1) ref_div_max = 1;
    *ref_div = MIN(MAX(den / post_div, 1u), ref_div_max);
    *fb_div = DIV_ROUND_CLOSEST(nom * *ref_div * post_div, den);
    if (*fb_div > fb_div_max) { *ref_div = (*ref_div * fb_div_max) / *fb_div; *fb_div = fb_div_max; }
}
/* dot_clock = ref_freq * fb_div / (ref_div * post_div); freq in kHz, results in 10 kHz */
static void compute_pll(const struct pll *pll, uint32_t freq, uint32_t *dot, uint32_t *fb_p, uint32_t *frac_p, uint32_t *ref_p, uint32_t *post_p) {
    bool frac = pll->flags & PLL_USE_FRAC_FB_DIV;
    unsigned target = frac ? freq : freq / 10;
    unsigned fb_div_min = pll->min_feedback_div, fb_div_max = pll->max_feedback_div;
    if (frac) { fb_div_min *= 10; fb_div_max *= 10; }
    unsigned ref_div_min = (pll->flags & PLL_USE_REF_DIV) ? pll->reference_div : pll->min_ref_div;
    unsigned ref_div_max = (frac && (pll->flags & PLL_USE_REF_DIV)) ? pll->reference_div : (pll->flags & PLL_PREFER_MINM_OVER_MAXP) ? MIN(pll->max_ref_div, 7u) : pll->max_ref_div;
    unsigned post_div_min, post_div_max;
    if (pll->flags & PLL_USE_POST_DIV) post_div_min = post_div_max = pll->post_div;
    else {
        unsigned vco_min = pll->pll_out_min, vco_max = pll->pll_out_max;
        if (frac) { vco_min *= 10; vco_max *= 10; }
        post_div_min = vco_min / target;
        if (target * post_div_min < vco_min) post_div_min++;
        if (post_div_min < pll->min_post_div) post_div_min = pll->min_post_div;
        post_div_max = vco_max / target;
        if (target * post_div_max > vco_max) post_div_max--;
        if (post_div_max > pll->max_post_div) post_div_max = pll->max_post_div;
    }
    unsigned nom = target, den = pll->reference_freq;
    reduce_ratio(&nom, &den, fb_div_min, post_div_min);
    unsigned post_best = (pll->flags & PLL_PREFER_MINM_OVER_MAXP) ? post_div_min : post_div_max, diff_best = ~0u, fb_div, ref_div;
    for (unsigned post = post_div_min; post <= post_div_max; post++) {
        get_fb_ref_div(nom, den, post, fb_div_max, ref_div_max, &fb_div, &ref_div);
        int d = (int)(target - (pll->reference_freq * fb_div) / (ref_div * post));
        unsigned diff = (unsigned)(d < 0 ? -d : d);
        if (diff < diff_best || (diff == diff_best && !(pll->flags & PLL_PREFER_MINM_OVER_MAXP))) { post_best = post; diff_best = diff; }
    }
    unsigned post = post_best;
    get_fb_ref_div(nom, den, post, fb_div_max, ref_div_max, &fb_div, &ref_div);
    reduce_ratio(&fb_div, &ref_div, fb_div_min, ref_div_min);
    if (frac && (fb_div % 10)) {   /* avoid high jitter with small fractional dividers */
        fb_div_min = MAX(fb_div_min, (9 - (fb_div % 10)) * 20 + 50);
        if (fb_div < fb_div_min) { unsigned tmp = DIV_ROUND_UP(fb_div_min, fb_div); fb_div *= tmp; ref_div *= tmp; }
    }
    if (frac) { *fb_p = fb_div / 10; *frac_p = fb_div % 10; } else { *fb_p = fb_div; *frac_p = 0; }
    *dot = (pll->reference_freq * *fb_p * 10 + pll->reference_freq * *frac_p) / (ref_div * post * 10);
    *ref_p = ref_div; *post_p = post;
}
static void read_pll_limits(struct radeon_kms *k) {
    struct atom_ctx *a = &k->atom;
    struct pll *p = &k->pll;
    uint16_t off; uint8_t frev = 0, crev = 0;
    memset(p, 0, sizeof *p);
    k->max_pixel_clock = 400000;
    if (atom_data_header(a, ATOM_DATA_FirmwareInfo, 0, &frev, &crev, &off)) {
        p->reference_freq = atom_u16(a, off + 0x52);
        p->pll_out_min = (frev < 2 && crev < 2) ? atom_u16(a, off + 0x4E) : atom_u32(a, off + 0x38);
        p->pll_out_max = atom_u32(a, off + 0x20);
        p->pll_in_min = atom_u16(a, off + 0x4A);
        p->pll_in_max = atom_u16(a, off + 0x4C);
        uint32_t maxpix = atom_u16(a, off + 0x48);
        if (maxpix) k->max_pixel_clock = maxpix * 10;
    }
    if (!p->reference_freq) p->reference_freq = 2700;
    if (!p->pll_out_min) p->pll_out_min = 64800;
    if (!p->pll_out_max) p->pll_out_max = 120000;
    p->min_post_div = 2; p->max_post_div = 0x7F;
    p->min_ref_div = 2; p->max_ref_div = 0x3FF;
    p->min_feedback_div = 4; p->max_feedback_div = 0x7FF;
}

/* ---- CRTC (atombios_crtc.c) ---- */
static void crtc_lock(struct radeon_kms *k, struct radeon *r, int lock) { struct crtc_enable a = { (uint8_t)r->crtc, (uint8_t)lock }; run(k, ATOM_CMD_UpdateCRTC_DoubleBufferRegisters, &a, sizeof a, "UpdateCRTC_DoubleBufferRegisters"); }
static void crtc_enable(struct radeon_kms *k, struct radeon *r, int on) { struct crtc_enable a = { (uint8_t)r->crtc, (uint8_t)on }; run(k, ATOM_CMD_EnableCRTC, &a, sizeof a, "EnableCRTC"); }
static void crtc_memreq(struct radeon_kms *k, struct radeon *r, int on) { struct crtc_enable a = { (uint8_t)r->crtc, (uint8_t)on }; run(k, ATOM_CMD_EnableCRTCMemReq, &a, sizeof a, "EnableCRTCMemReq"); }
static void crtc_blank(struct radeon_kms *k, struct radeon *r, int on) { struct crtc_blank a = { (uint8_t)r->crtc, (uint8_t)on }; run(k, ATOM_CMD_BlankCRTC, &a, sizeof a, "BlankCRTC"); }
static void crtc_dpms(struct radeon_kms *k, struct radeon *r, bool on) {
    if (on) { crtc_enable(k, r, ATOM_ENABLE); crtc_memreq(k, r, ATOM_ENABLE); crtc_blank(k, r, ATOM_DISABLE); }
    else { crtc_blank(k, r, ATOM_ENABLE); crtc_memreq(k, r, ATOM_DISABLE); crtc_enable(k, r, ATOM_DISABLE); }
}
static int encoder_mode(const struct output *o) { return is_dig(o->enc_id) ? ATOM_ENCODER_MODE_DVI : ATOM_ENCODER_MODE_CRT; }
static bool dual_link(const struct output *o, uint32_t clock) {
    bool dl_connector = o->conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I || o->conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D || o->conn_type == CONNECTOR_OBJECT_ID_HDMI_TYPE_B;
    return is_dig(o->enc_id) && dl_connector && clock > 165000;
}
/* AdjustDisplayPll: the BIOS may move the clock or dictate dividers for the encoder */
static uint32_t adjust_pll(struct radeon_kms *k, struct radeon *r, uint32_t clock) {
    struct output *o = k->out;
    struct pll *p = &k->pll;
    p->flags = 0;
    if (r->dce32 && clock > 165000) p->flags |= PLL_USE_FRAC_FB_DIV;
    if (r->is_rs880) p->flags |= PLL_USE_FRAC_FB_DIV | PLL_PREFER_MINM_OVER_MAXP;   /* no spread spectrum is used */
    uint8_t frev, crev;
    if (!atom_cmd_header(&k->atom, ATOM_CMD_AdjustDisplayPll, &frev, &crev) || frev != 1) return clock;
    if (crev == 1 || crev == 2) {
        struct adjust_pll_v1 a = { (uint16_t)(clock / 10), o->enc_id, (uint8_t)encoder_mode(o) };
        if (run(k, ATOM_CMD_AdjustDisplayPll, &a, sizeof a, "AdjustDisplayPll")) return clock;
        return (uint32_t)a.pixel_clock * 10;
    }
    if (crev == 3) {
        union { struct adjust_pll_v3_in in; struct adjust_pll_v3_out out; } a;
        memset(&a, 0, sizeof a);
        a.in.pixel_clock = (uint16_t)(clock / 10); a.in.transmitter_id = o->enc_id; a.in.encode_mode = (uint8_t)encoder_mode(o);
        if (o->device & ATOM_DEVICE_DFP_SUPPORT) {
            a.in.disp_pll_config |= DISPPLL_CONFIG_COHERENT_MODE;
            if (dual_link(o, clock)) a.in.disp_pll_config |= DISPPLL_CONFIG_DUAL_LINK;
        }
        if (run(k, ATOM_CMD_AdjustDisplayPll, &a, sizeof a, "AdjustDisplayPll")) return clock;
        uint32_t adjusted = a.out.disp_pll_freq * 10;
        if (a.out.ref_div) { p->flags |= PLL_USE_FRAC_FB_DIV | PLL_USE_REF_DIV; p->reference_div = a.out.ref_div; }
        if (a.out.post_div) { p->flags |= PLL_USE_FRAC_FB_DIV | PLL_USE_POST_DIV; p->post_div = a.out.post_div; }
        return adjusted ? adjusted : clock;
    }
    return clock;
}
static int set_pixel_clock(struct radeon_kms *k, struct radeon *r, uint32_t clock, uint32_t ref, uint32_t fb, uint32_t frac, uint32_t post) {
    uint8_t frev, crev;
    if (!atom_cmd_header(&k->atom, ATOM_CMD_SetPixelClock, &frev, &crev) || frev != 1) return -1;
    int pll_id = r->crtc;   /* DCE3: PPLLn is hard-wired to CRTCn */
    if (crev == 1 || crev == 2) {
        struct pixel_clock_v2 a = { (uint16_t)(clock / 10), (uint16_t)ref, (uint16_t)fb, (uint8_t)post, (uint8_t)frac, (uint8_t)pll_id, 1, (uint8_t)r->crtc, 0 };
        return run(k, ATOM_CMD_SetPixelClock, &a, sizeof a, "SetPixelClock");
    }
    if (crev == 3) {
        struct pixel_clock_v3 a = { (uint16_t)(clock / 10), (uint16_t)ref, (uint16_t)fb, (uint8_t)post, (uint8_t)frac, (uint8_t)pll_id, k->out->enc_id, (uint8_t)encoder_mode(k->out),
                                    r->crtc ? PIXEL_CLOCK_MISC_CRTC_SEL_CRTC2 : 0 };
        return run(k, ATOM_CMD_SetPixelClock, &a, sizeof a, "SetPixelClock");
    }
    kprintf("radeon: SetPixelClock table version %d.%d is not supported\n", frev, crev);
    return -1;
}
static int set_crtc_timing(struct radeon_kms *k, struct radeon *r, const struct display_timing *t) {
    struct dtd_timing a = { 0 };
    a.h_size = t->hdisplay; a.h_blank = (uint16_t)(t->htotal - t->hdisplay);
    a.v_size = t->vdisplay; a.v_blank = (uint16_t)(t->vtotal - t->vdisplay);
    a.h_sync_offset = (uint16_t)(t->hsync_start - t->hdisplay); a.h_sync_width = (uint16_t)(t->hsync_end - t->hsync_start);
    a.v_sync_offset = (uint16_t)(t->vsync_start - t->vdisplay); a.v_sync_width = (uint16_t)(t->vsync_end - t->vsync_start);
    a.misc = (t->nvsync ? ATOM_VSYNC_POLARITY : 0) | (t->nhsync ? ATOM_HSYNC_POLARITY : 0);
    a.crtc = (uint8_t)r->crtc;
    return run(k, ATOM_CMD_SetCRTC_UsingDTDTiming, &a, sizeof a, "SetCRTC_UsingDTDTiming");
}
/* linear 32-bpp ARGB scanout surface (avivo_crtc_do_set_base) */
static void set_surface(struct radeon *r, uint64_t gpu_addr, int w, int h, int pitch_px) {
    radeon_wreg(r, r->crtc ? D2VGA_CONTROL : D1VGA_CONTROL, 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_FLIP_CONTROL), 0);   /* surface updates at vblank, not hblank */
    radeon_wreg(r, r->crtc ? D2GRPH_PRIMARY_SURFACE_ADDRESS_HIGH : D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH, (uint32_t)(gpu_addr >> 32));
    radeon_wreg(r, r->crtc ? D2GRPH_SECONDARY_SURFACE_ADDRESS_HIGH : D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH, (uint32_t)(gpu_addr >> 32));
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_PRIMARY_SURFACE_ADDRESS), (uint32_t)gpu_addr);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_SECONDARY_SURFACE_ADDRESS), (uint32_t)gpu_addr);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_CONTROL), GRPH_DEPTH_32BPP);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_SWAP_CONTROL), 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_LUT_SEL), radeon_rreg(r, radeon_crtc_reg(r, D1GRPH_LUT_SEL)) & ~LUT_10BIT_BYPASS_EN);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_SURFACE_OFFSET_X), 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_SURFACE_OFFSET_Y), 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_X_START), 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_Y_START), 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_X_END), (uint32_t)w);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_Y_END), (uint32_t)h);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_PITCH), (uint32_t)pitch_px);
    radeon_wreg(r, radeon_crtc_reg(r, D1GRPH_ENABLE), 1);
    radeon_wreg(r, radeon_crtc_reg(r, D1MODE_DESKTOP_HEIGHT), (uint32_t)h);
    radeon_wreg(r, radeon_crtc_reg(r, D1MODE_VIEWPORT_START), 0);
    radeon_wreg(r, radeon_crtc_reg(r, D1MODE_VIEWPORT_SIZE), (uint32_t)w << 16 | (uint32_t)((h + 1) & ~1));
    radeon_wreg(r, radeon_crtc_reg(r, D1MODE_MASTER_UPDATE_MODE), 3);   /* flips at the start of vblank */
}

/* CRTC timing and scanout registers, for undoing a failed mode set when no timing is known to restore */
static const uint16_t crtc_save_regs[] = { 0x6000, 0x6004, 0x6008, 0x600C, 0x6010, 0x6014, 0x6020, 0x6024, 0x6028, 0x602C, 0x6030, 0x6034,
    D1GRPH_CONTROL, D1GRPH_LUT_SEL, D1GRPH_SWAP_CONTROL, D1GRPH_PRIMARY_SURFACE_ADDRESS, D1GRPH_SECONDARY_SURFACE_ADDRESS, D1GRPH_PITCH,
    D1GRPH_SURFACE_OFFSET_X, D1GRPH_SURFACE_OFFSET_Y, D1GRPH_X_START, D1GRPH_Y_START, D1GRPH_X_END, D1GRPH_Y_END, D1GRPH_FLIP_CONTROL, D1GRPH_ENABLE,
    D1MODE_DATA_FORMAT, D1MODE_DESKTOP_HEIGHT, D1MODE_VIEWPORT_START, D1MODE_VIEWPORT_SIZE, D1MODE_MASTER_UPDATE_MODE };
#define NCRTC_SAVE (sizeof crtc_save_regs / sizeof crtc_save_regs[0])
struct crtc_save { uint32_t v[NCRTC_SAVE], hi[2]; };
static void crtc_save(struct radeon *r, struct crtc_save *s) {
    for (size_t i = 0; i < NCRTC_SAVE; i++) s->v[i] = radeon_rreg(r, radeon_crtc_reg(r, crtc_save_regs[i]));
    s->hi[0] = radeon_rreg(r, r->crtc ? D2GRPH_PRIMARY_SURFACE_ADDRESS_HIGH : D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH);
    s->hi[1] = radeon_rreg(r, r->crtc ? D2GRPH_SECONDARY_SURFACE_ADDRESS_HIGH : D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH);
}
static void crtc_restore(struct radeon_kms *k, struct radeon *r, const struct crtc_save *s) {
    crtc_lock(k, r, ATOM_ENABLE);
    radeon_wreg(r, r->crtc ? D2GRPH_PRIMARY_SURFACE_ADDRESS_HIGH : D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH, s->hi[0]);
    radeon_wreg(r, r->crtc ? D2GRPH_SECONDARY_SURFACE_ADDRESS_HIGH : D1GRPH_SECONDARY_SURFACE_ADDRESS_HIGH, s->hi[1]);
    for (size_t i = 0; i < NCRTC_SAVE; i++) radeon_wreg(r, radeon_crtc_reg(r, crtc_save_regs[i]), s->v[i]);
    crtc_dpms(k, r, true);
    crtc_lock(k, r, ATOM_DISABLE);
}

/* ---- encoders (atombios_encoders.c) ---- */
static void output_lock(struct radeon *r, bool lock) {
    uint32_t s6 = radeon_rreg(r, BIOS_6_SCRATCH);
    if (lock) { s6 |= ATOM_S6_CRITICAL_STATE; s6 &= ~ATOM_S6_ACC_MODE; }
    else { s6 &= ~ATOM_S6_CRITICAL_STATE; s6 |= ATOM_S6_ACC_MODE; }
    radeon_wreg(r, BIOS_6_SCRATCH, s6);
}
static int select_crtc_source(struct radeon_kms *k, struct radeon *r) {
    struct output *o = k->out;
    uint8_t frev, crev;
    if (!atom_cmd_header(&k->atom, ATOM_CMD_SelectCRTC_Source, &frev, &crev) || frev != 1) return -1;
    if (crev == 1) {
        /* v1 names a device: only the DACs have one (ATOM_DEVICE_CRT1/CRT2_INDEX) */
        if (is_dig(o->enc_id)) { kprintf("radeon: SelectCRTC_Source v1 cannot route a DIG encoder\n"); return -1; }
        uint8_t a[4] = { (uint8_t)r->crtc, o->enc_id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1 ? 0 : 4 };
        return run(k, ATOM_CMD_SelectCRTC_Source, a, sizeof a, "SelectCRTC_Source");
    }
    struct crtc_source_v2 a = { (uint8_t)r->crtc, 0, (uint8_t)encoder_mode(o), 0 };
    switch (o->enc_id) {
    case ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1: a.encoder_id = ASIC_INT_DAC1_ENCODER_ID; break;
    case ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC2: a.encoder_id = ASIC_INT_DAC2_ENCODER_ID; break;
    default: a.encoder_id = k->dig_encoder ? ASIC_INT_DIG2_ENCODER_ID : ASIC_INT_DIG1_ENCODER_ID; break;
    }
    return run(k, ATOM_CMD_SelectCRTC_Source, &a, sizeof a, "SelectCRTC_Source");
}
static int dig_encoder_setup(struct radeon_kms *k, int action, uint32_t clock) {
    struct output *o = k->out;
    int index = k->dig_encoder ? ATOM_CMD_DIG2EncoderControl : ATOM_CMD_DIG1EncoderControl;
    uint8_t frev, crev;
    if (!atom_cmd_header(&k->atom, index, &frev, &crev) || frev != 1) return -1;
    struct dig_encoder a = { (uint16_t)(clock / 10), 0, (uint8_t)action, ATOM_ENCODER_MODE_DVI, (uint8_t)(dual_link(o, clock) ? 8 : 4) };
    if (crev == 1) {
        switch (o->enc_id) {
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY: a.config = ATOM_ENCODER_CONFIG_V2_TRANSMITTER1; break;
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1: case ENCODER_OBJECT_ID_INTERNAL_KLDSCP_LVTMA: a.config = ATOM_ENCODER_CONFIG_V2_TRANSMITTER2; break;
        case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2: a.config = ATOM_ENCODER_CONFIG_V2_TRANSMITTER3; break;
        }
        if (o->linkb) a.config |= ATOM_ENCODER_CONFIG_LINKB;
    } else if (crev == 2 || crev == 3) {
        a.config = (uint8_t)(k->dig_encoder << 4);   /* acConfig.ucDigSel */
        a.bpc = PANEL_8BIT_PER_COLOR;
    } else { kprintf("radeon: DIG encoder table version %d.%d is not supported\n", frev, crev); return -1; }
    return run(k, index, &a, sizeof a, "DIGEncoderControl");
}
static int dig_transmitter_setup(struct radeon_kms *k, struct radeon *r, int action, uint32_t clock) {
    struct output *o = k->out;
    int index = o->enc_id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_LVTMA ? ATOM_CMD_LVTMATransmitterControl : ATOM_CMD_UNIPHYTransmitterControl;
    uint8_t frev, crev;
    if (!atom_cmd_header(&k->atom, index, &frev, &crev) || frev != 1) return -1;
    bool dl = action != ATOM_TRANSMITTER_ACTION_INIT && dual_link(o, clock);
    struct dig_transmitter a = { 0 };
    a.action = (uint8_t)action;
    if (action == ATOM_TRANSMITTER_ACTION_INIT) a.pixel_clock = o->conn_obj & 0xFF;   /* usInitInfo: connector object id */
    else a.pixel_clock = (uint16_t)((dl ? clock / 2 : clock) / 10);
    int sel = o->enc_id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY1 ? 1 : o->enc_id == ENCODER_OBJECT_ID_INTERNAL_UNIPHY2 ? 2 : 0;
    int dig = action == ATOM_TRANSMITTER_ACTION_INIT ? 0 : k->dig_encoder;
    if (crev == 1) {
        a.config = (dig ? ATOM_TRANSMITTER_CONFIG_DIG2_ENCODER : 0) | (o->linkb ? ATOM_TRANSMITTER_CONFIG_LINKB : 0) | ATOM_TRANSMITTER_CONFIG_COHERENT | (dl ? ATOM_TRANSMITTER_CONFIG_8LANE_LINK : 0);
    } else if (crev == 2) {
        /* fDualLinkConnector:1 fCoherentMode:1 ucLinkSel:1 ucEncoderSel:1 fDPConnector:1 reserved:1 ucTransmitterSel:2 */
        a.config = (uint8_t)((dl ? 1 : 0) | 1 << 1 | (o->linkb ? 1 : 0) << 2 | (dig & 1) << 3 | sel << 6);
    } else if (crev == 3) {
        /* ... ucEncoderSel:1 ucRefClkSource:2 (the PLL) ucTransmitterSel:2 */
        a.config = (uint8_t)((dl ? 1 : 0) | 1 << 1 | (o->linkb ? 1 : 0) << 2 | (dig & 1) << 3 | (r->crtc & 3) << 4 | sel << 6);
        a.lane_num = dl ? 8 : 4;
    } else { kprintf("radeon: transmitter table version %d.%d is not supported\n", frev, crev); return -1; }
    return run(k, index, &a, sizeof a, "TransmitterControl");
}
static int dac_setup(struct radeon_kms *k, int action, uint32_t clock) {
    int index = k->out->enc_id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1 ? ATOM_CMD_DAC1EncoderControl : ATOM_CMD_DAC2EncoderControl;
    struct dac_encoder a = { (uint16_t)(clock / 10), ATOM_DAC1_PS2, (uint8_t)action };
    return run(k, index, &a, sizeof a, "DACEncoderControl");
}
static int encoder_dpms(struct radeon_kms *k, struct radeon *r, bool on, uint32_t clock) {
    struct output *o = k->out;
    if (!is_dig(o->enc_id)) {
        struct output_control a = { on ? ATOM_ENABLE : ATOM_DISABLE };
        return run(k, o->enc_id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_DAC1 ? ATOM_CMD_DAC1OutputControl : ATOM_CMD_DAC2OutputControl, &a, sizeof a, "DACOutputControl");
    }
    if (on) {
        int e = dig_encoder_setup(k, ATOM_ENABLE, clock);
        e |= dig_transmitter_setup(k, r, ATOM_TRANSMITTER_ACTION_SETUP, clock);
        e |= dig_transmitter_setup(k, r, ATOM_TRANSMITTER_ACTION_ENABLE, clock);
        return e;
    }
    int e = dig_transmitter_setup(k, r, ATOM_TRANSMITTER_ACTION_DISABLE, clock);
    e |= dig_encoder_setup(k, ATOM_DISABLE, clock);
    return e;
}
/* tell the BIOS which device is lit and on which CRTC */
static void update_scratch(struct radeon_kms *k, struct radeon *r) {
    uint32_t s3 = radeon_rreg(r, BIOS_3_SCRATCH);
    for (int i = 0; i < 12; i++) if (k->out->device & (1u << i)) {
        s3 |= 1u << i;
        s3 &= ~(1u << (16 + i));
        s3 |= (uint32_t)r->crtc << (16 + i);
    }
    radeon_wreg(r, BIOS_3_SCRATCH, s3);
}

/* the whole helper sequence: encoder prepare, CRTC prepare, CRTC mode set, encoder mode set, commits */
static int program_mode(struct radeon_kms *k, struct radeon *r, const struct display_timing *t, uint64_t surface_off, int pitch_px) {
    struct output *o = k->out;
    int err = 0;
    if (is_dig(o->enc_id) && !k->transmitter_inited) {
        dig_transmitter_setup(k, r, ATOM_TRANSMITTER_ACTION_INIT, t->clock_khz);
        k->transmitter_inited = true;
    }
    k->dig_encoder = r->dce32 ? r->crtc : (o->enc_id == ENCODER_OBJECT_ID_INTERNAL_KLDSCP_LVTMA ? 1 : 0);
    /* encoder prepare */
    output_lock(r, true);
    err |= select_crtc_source(k, r);
    /* CRTC prepare */
    crtc_lock(k, r, ATOM_ENABLE);
    crtc_dpms(k, r, false);
    /* CRTC mode set */
    uint32_t adjusted = adjust_pll(k, r, t->clock_khz), dot, fb, frac, ref, post;
    compute_pll(&k->pll, adjusted, &dot, &fb, &frac, &ref, &post);
    struct ss_on_ppll ss = { 0 };
    ss.ppll = (uint8_t)r->crtc;
    run(k, ATOM_CMD_EnableSpreadSpectrumOnPPLL, &ss, sizeof ss, "EnableSpreadSpectrumOnPPLL");
    err |= set_pixel_clock(k, r, t->clock_khz, ref, fb, frac, post);
    err |= set_crtc_timing(k, r, t);
    set_surface(r, r->fb_start_gpu + surface_off, t->hdisplay, t->vdisplay, pitch_px);
    struct crtc_overscan ov = { 0 }; ov.crtc = (uint8_t)r->crtc;
    run(k, ATOM_CMD_SetCRTC_OverScan, &ov, sizeof ov, "SetCRTC_OverScan");
    struct scaler_enable sc = { (uint8_t)r->crtc, ATOM_SCALER_DISABLE, 0, 0 };
    run(k, ATOM_CMD_EnableScaler, &sc, sizeof sc, "EnableScaler");
    /* encoder mode set */
    encoder_dpms(k, r, false, t->clock_khz);
    uint32_t s3 = radeon_rreg(r, BIOS_3_SCRATCH);
    radeon_wreg(r, BIOS_3_SCRATCH, 0);
    struct yuv_enable yuv = { ATOM_DISABLE, (uint8_t)r->crtc };
    run(k, ATOM_CMD_EnableYUV, &yuv, sizeof yuv, "EnableYUV");
    radeon_wreg(r, BIOS_3_SCRATCH, s3);
    if (!is_dig(o->enc_id)) err |= dac_setup(k, ATOM_ENABLE, t->clock_khz);
    radeon_wreg(r, radeon_crtc_reg(r, D1MODE_DATA_FORMAT), 0);   /* set_scaler clears this on some chips */
    /* commits */
    crtc_dpms(k, r, true);
    crtc_lock(k, r, ATOM_DISABLE);
    err |= encoder_dpms(k, r, true, t->clock_khz);
    output_lock(r, false);
    update_scratch(k, r);
    kprintf("radeon: %ux%u@%d, pixel clock %u kHz (adjusted %u), PLL ref %u fb %u.%u post %u -> %u kHz%s\n", t->hdisplay, t->vdisplay, timing_refresh_hz(t),
            t->clock_khz, adjusted, ref, fb, frac, post, dot * 10, err ? ", with errors" : "");
    return err ? -1 : 0;
}

/* the scanout position counter moves while the CRTC runs */
static bool crtc_running(struct radeon *r) {
    uint32_t a = radeon_rreg(r, radeon_crtc_reg(r, D1CRTC_STATUS_POSITION));
    for (int i = 0; i < 50; i++) {
        radeon_udelay(1000);
        if (radeon_rreg(r, radeon_crtc_reg(r, D1CRTC_STATUS_POSITION)) != a) return true;
    }
    return false;
}

/* ---- modes ---- */
static bool timing_ok(struct radeon_kms *k, struct radeon *r, const struct display_timing *t) {
    if (t->interlace || t->hdisplay < 640 || t->vdisplay < 480 || t->hdisplay > 4096 || t->vdisplay > 4096) return false;
    if (t->clock_khz > k->max_pixel_clock) return false;
    struct output *o = k->out;
    if (is_dig(o->enc_id)) {
        bool dl_connector = o->conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I || o->conn_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D || o->conn_type == CONNECTOR_OBJECT_ID_HDMI_TYPE_B;
        if (t->clock_khz > (dl_connector ? 330000u : 165000u)) return false;
    }
    if (k->edid.max_clock_khz && t->clock_khz > k->edid.max_clock_khz) return false;
    uint64_t fb = (uint64_t)ALIGN_UP(t->hdisplay, 64) * 4 * t->vdisplay;
    return ALIGN_UP(fb, 0x10000) * 2 + 0x10000 <= k->vram_limit;
}
/* the timing for w x h: the EDID's preferred one, else the one closest to 60 Hz */
static const struct display_timing *pick_timing(struct radeon_kms *k, struct radeon *r, int w, int h) {
    const struct display_timing *best = NULL; int best_score = 1 << 30;
    for (int i = 0; i < k->edid.ntimings; i++) {
        const struct display_timing *t = &k->edid.timings[i];
        if (t->hdisplay != w || t->vdisplay != h || !timing_ok(k, r, t)) continue;
        int hz = timing_refresh_hz(t), score = t->preferred ? -1 : (hz > 60 ? hz - 60 : (60 - hz) * 2);
        if (score < best_score) { best = t; best_score = score; }
    }
    return best;
}
static int kms_list_modes(struct gpu_device *g, struct display_mode *out, int max) {
    struct radeon *r = g->priv; struct radeon_kms *k = r->kms;
    int n = 0;
    for (int i = 0; i < k->edid.ntimings && n < max; i++) {
        const struct display_timing *t = &k->edid.timings[i];
        if (!timing_ok(k, r, t)) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) if (out[j].w == t->hdisplay && out[j].h == t->vdisplay) dup = true;
        if (!dup) { out[n].w = t->hdisplay; out[n].h = t->vdisplay; n++; }
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && (uint64_t)out[j].w * out[j].h < (uint64_t)out[j - 1].w * out[j - 1].h; j--) { struct display_mode t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

static bool kms_set_mode(struct gpu_device *g, int w, int h) {
    struct radeon *r = g->priv; struct radeon_kms *k = r->kms;
    const struct display_timing *t = pick_timing(k, r, w, h);
    if (!t) { kprintf("radeon: no usable timing for %dx%d\n", w, h); return false; }
    struct display_timing want = *t;
    /* lay VRAM out again: two scanout buffers and the cursor from offset 0 */
    int pitch_px = (int)ALIGN_UP(want.hdisplay, 64);
    uint64_t fb_size = (uint64_t)pitch_px * 4 * want.vdisplay, buf1 = ALIGN_UP(fb_size, 0x10000), cursor = ALIGN_UP(buf1 + fb_size, 0x1000);
    struct display_timing old = k->cur; bool old_valid = k->cur_valid;
    uint64_t old_front = r->buf_off[r->back ^ 1]; uint32_t old_pitch = r->pitch_px;
    struct crtc_save saved; crtc_save(r, &saved);
    int err = program_mode(k, r, &want, 0, pitch_px);
    if (err || !crtc_running(r)) {
        kprintf("radeon: mode set to %dx%d failed%s; restoring\n", w, h, err ? "" : " (CRTC not running)");
        if (old_valid) program_mode(k, r, &old, old_front, (int)old_pitch);
        else crtc_restore(k, r, &saved);
        return false;
    }
    memset(r->vram, 0, (size_t)(buf1 + fb_size));   /* only now: a failed attempt keeps the old picture */
    k->cur = want; k->cur_valid = true;
    r->buf_off[0] = 0; r->buf_off[1] = buf1; r->back = 1;
    r->cursor_off = cursor;
    r->width = want.hdisplay; r->height = want.vdisplay; r->pitch_px = (uint32_t)pitch_px;
    radeon_cursor_upload(r);
    g->fb_phys = r->gpu.vram_base; g->fb_width = want.hdisplay; g->fb_height = want.vdisplay; g->fb_pitch = pitch_px * 4;
    return true;
}

void radeon_kms_init(struct radeon *r) {
    struct radeon_kms *k = kzalloc(sizeof *k);
    if (!k) return;
    k->card = (struct atom_card){ r, cb_reg_read, cb_reg_write, cb_io_read, cb_io_write, cb_mc_read, cb_mc_write, cb_pll_read, cb_pll_write, cb_delay, cb_now };
    if (!atom_init(&k->atom, &k->card, r->rom, (uint32_t)r->rom_len)) { kprintf("radeon: AtomBIOS image not usable; mode setting off\n"); kfree(k); return; }
    parse_objects(k);
    read_pll_limits(k);
    /* the firmware's active devices, and which CRTC each is on, are in BIOS scratch register 3 */
    uint32_t s3 = radeon_rreg(r, BIOS_3_SCRATCH);
    for (int i = 0; i < k->noutputs; i++) {
        struct output *o = &k->outputs[i];
        int bit = __builtin_ctz(o->device);
        bool active = (s3 >> bit) & 1, on_crtc = (int)((s3 >> (16 + bit)) & 1) == r->crtc;
        kprintf("radeon: output %d: connector %02x, encoder %02x%s, device %04x%s%s%s\n", i, o->conn_type, o->enc_id, o->linkb ? " link B" : "",
                o->device, o->ddc.valid ? ", DDC" : "", active ? ", active" : "", active && on_crtc ? " on the scanout CRTC" : "");
        if (active && on_crtc && (!k->out || (o->device & ATOM_DEVICE_DFP_SUPPORT))) k->out = o;
    }
    /* scratch registers left empty (some GOP drivers): take the first output a monitor answers on */
    static uint8_t edid[256];
    size_t elen = 0;
    if (k->out) elen = ddc_read_edid(r, &k->out->ddc, edid);
    else for (int i = 0; i < k->noutputs && !elen; i++) {
        elen = ddc_read_edid(r, &k->outputs[i].ddc, edid);
        if (elen && ((edid[20] & 0x80) != 0) == is_dig(k->outputs[i].enc_id)) k->out = &k->outputs[i];
        else elen = 0;
    }
    if (!k->out) { kprintf("radeon: cannot tell which output is lit; mode setting off\n"); goto off; }
    struct output *o = k->out;
    if (o->external || o->router) { kprintf("radeon: output uses an external encoder or a router; mode setting off\n"); goto off; }
    if (o->device & ATOM_DEVICE_LCD_SUPPORT || o->conn_type == CONNECTOR_OBJECT_ID_LVDS || o->conn_type == CONNECTOR_OBJECT_ID_LVDS_eDP) { kprintf("radeon: LVDS panel; mode setting off\n"); goto off; }
    if (o->conn_type == CONNECTOR_OBJECT_ID_DISPLAYPORT || o->conn_type == CONNECTOR_OBJECT_ID_eDP) { kprintf("radeon: DisplayPort needs link training; mode setting off\n"); goto off; }
    if (o->conn_type == CONNECTOR_OBJECT_ID_PCIE_CONNECTOR) { kprintf("radeon: DDI slot output; mode setting off\n"); goto off; }
    if (!elen || !edid_parse(edid, elen, &k->edid)) { kprintf("radeon: no EDID on the active output; mode setting off\n"); goto off; }
    /* VRAM below what the firmware reserves for itself */
    k->vram_limit = r->aper_len;
    uint16_t uoff;
    if (atom_data_header(&k->atom, ATOM_DATA_VRAM_UsageByFirmware, 0, 0, 0, &uoff)) {
        uint64_t start = atom_u32(&k->atom, uoff + 4);
        if (start >= r->fb_start_gpu) start -= r->fb_start_gpu;
        if (start && start < k->vram_limit) k->vram_limit = start;
    }
    /* the boot mode, if the EDID lists it, is what a failed mode set falls back to */
    const struct display_timing *boot = pick_timing(k, r, (int)r->width, (int)r->height);
    if (boot) { k->cur = *boot; k->cur_valid = true; }
    kprintf("radeon: AtomBIOS '%s', %s monitor %s '%s', %d modes; PLL ref %u0 kHz, VCO %u0-%u0 kHz, max pixel clock %u kHz\n", r->gpu.bios_message[0] ? r->gpu.bios_message : k->atom.name,
            k->edid.digital ? "digital" : "analog", k->edid.vendor, k->edid.name, k->edid.ntimings, k->pll.reference_freq, k->pll.pll_out_min, k->pll.pll_out_max, k->max_pixel_clock);
    /* tell the BIOS a driver owns the display */
    radeon_wreg(r, BIOS_2_SCRATCH, radeon_rreg(r, BIOS_2_SCRATCH) & ~ATOM_S2_VRI_BRIGHT_ENABLE);
    radeon_wreg(r, BIOS_6_SCRATCH, radeon_rreg(r, BIOS_6_SCRATCH) | ATOM_S6_ACC_BLOCK_DISPLAY_SWITCH);
    r->kms = k;
    r->gpu.modeset = true;
    r->gpu.set_mode = kms_set_mode;
    r->gpu.list_modes = kms_list_modes;
    return;
off:
    atom_fini(&k->atom);
    kfree(k);
}
