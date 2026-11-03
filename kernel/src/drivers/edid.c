/*
 * EDID 1.x parsing (base block and CEA-861 extensions) and VESA CVT timing
 * generation. Everything is integer arithmetic: the kernel is built without
 * floating point.
 */
#include "edid.h"
#include "kernel.h"
#include "string.h"

bool edid_block_valid(const uint8_t *b) {
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum += b[i];
    return sum == 0;
}
static bool header_ok(const uint8_t *b) {
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    return !memcmp(b, hdr, 8);
}

int timing_refresh_hz(const struct display_timing *t) {
    uint64_t pixels = (uint64_t)t->htotal * t->vtotal;
    if (!pixels) return 0;
    uint64_t hz = ((uint64_t)t->clock_khz * 1000 * (t->interlace ? 2 : 1) + pixels / 2) / pixels;
    return (int)hz;
}

/* VESA Display Monitor Timings that EDID's established-timing bits refer to */
struct dmt { uint16_t w, h, hz; struct display_timing t; };
#define T(clk, hd, hss, hse, ht, vd, vss, vse, vt, nh, nv) { clk, hd, hss, hse, ht, vd, vss, vse, vt, nh, nv, false, false }
static const struct dmt established[17] = {
    /* byte 0x23, bit 7 down */
    { 720, 400, 70, T(28320, 720, 738, 846, 900, 400, 412, 414, 449, true, false) },
    { 720, 400, 88, T(35500, 720, 738, 846, 900, 400, 421, 423, 449, true, false) },
    { 640, 480, 60, T(25175, 640, 656, 752, 800, 480, 490, 492, 525, true, true) },
    { 640, 480, 67, T(30240, 640, 704, 768, 864, 480, 483, 486, 525, true, true) },
    { 640, 480, 72, T(31500, 640, 664, 704, 832, 480, 489, 492, 520, true, true) },
    { 640, 480, 75, T(31500, 640, 656, 720, 840, 480, 481, 484, 500, true, true) },
    { 800, 600, 56, T(36000, 800, 824, 896, 1024, 600, 601, 603, 625, false, false) },
    { 800, 600, 60, T(40000, 800, 840, 968, 1056, 600, 601, 605, 628, false, false) },
    /* byte 0x24, bit 7 down */
    { 800, 600, 72, T(50000, 800, 856, 976, 1040, 600, 637, 643, 666, false, false) },
    { 800, 600, 75, T(49500, 800, 816, 896, 1056, 600, 601, 604, 625, false, false) },
    { 832, 624, 75, T(57284, 832, 864, 928, 1152, 624, 625, 628, 667, true, true) },
    { 0, 0, 0, T(0, 0, 0, 0, 0, 0, 0, 0, 0, false, false) },   /* 1024x768 87 Hz interlaced: not offered */
    { 1024, 768, 60, T(65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, true, true) },
    { 1024, 768, 70, T(75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, true, true) },
    { 1024, 768, 75, T(78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, false, false) },
    { 1280, 1024, 75, T(135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, false, false) },
    /* byte 0x25, bit 7 */
    { 1152, 870, 75, T(100000, 1152, 1216, 1344, 1456, 870, 873, 876, 915, true, true) },
};
/* common DMT modes standard-timing descriptors usually name; anything else is generated with CVT */
static const struct dmt dmt_modes[] = {
    { 1152, 864, 75, T(108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, false, false) },
    { 1280, 720, 60, T(74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, false, false) },
    { 1280, 800, 60, T(83500, 1280, 1352, 1480, 1680, 800, 803, 809, 831, true, false) },
    { 1280, 960, 60, T(108000, 1280, 1376, 1488, 1800, 960, 961, 964, 1000, false, false) },
    { 1280, 1024, 60, T(108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, false, false) },
    { 1360, 768, 60, T(85500, 1360, 1424, 1536, 1792, 768, 771, 777, 795, false, false) },
    { 1366, 768, 60, T(85500, 1366, 1436, 1579, 1792, 768, 771, 774, 798, false, false) },
    { 1400, 1050, 60, T(121750, 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, true, false) },
    { 1440, 900, 60, T(106500, 1440, 1520, 1672, 1904, 900, 903, 909, 934, true, false) },
    { 1600, 900, 60, T(108000, 1600, 1624, 1704, 1800, 900, 901, 904, 1000, false, false) },
    { 1600, 1200, 60, T(162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, false, false) },
    { 1680, 1050, 60, T(146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, true, false) },
    { 1920, 1080, 60, T(148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, false, false) },
    { 1920, 1200, 60, T(154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, false, true) },
};
#undef T

static int cvt_vsync_lines(int w, int h) {
    if (h * 4 == w * 3) return 4;
    if (h * 16 == w * 9) return 5;
    if (h * 16 == w * 10) return 6;
    if (h * 5 == w * 4 || h * 15 == w * 9) return 7;
    return 10;
}
void timing_cvt(int w, int h, int refresh, bool reduced, struct display_timing *t) {
    memset(t, 0, sizeof *t);
    int hd = w / 8 * 8, vsync = cvt_vsync_lines(w, h);
    int64_t frame_ns = 1000000000LL / refresh;
    if (reduced) {
        /* CVT-RB: fixed 160-pixel blanking, at least 460 us of vertical blanking */
        int64_t hp_ns = (frame_ns - 460000) / h;
        int vbi = (int)(460000 / hp_ns) + 1;
        if (vbi < 3 + vsync + 6) vbi = 3 + vsync + 6;
        int total = hd + 160, vtotal = h + vbi;
        uint64_t clk = (uint64_t)refresh * vtotal * total / 1000;          /* kHz */
        t->clock_khz = (uint32_t)(clk / 250 * 250);
        t->hdisplay = hd; t->hsync_start = hd + 48; t->hsync_end = hd + 48 + 32; t->htotal = total;
        t->vdisplay = h; t->vsync_start = h + 3; t->vsync_end = h + 3 + vsync; t->vtotal = vtotal;
        t->nhsync = false; t->nvsync = true;
        return;
    }
    /* CVT with normal blanking: 550 us minimum vsync+back porch, C' = 30, M' = 300 */
    int64_t hp_ns = (frame_ns - 550000) / (h + 3);
    int vsbp = (int)(550000 / hp_ns) + 1;
    if (vsbp < vsync + 6) vsbp = vsync + 6;
    int vtotal = h + vsbp + 3;
    int64_t duty_m = 30000 - 300 * hp_ns / 1000;             /* ideal blanking duty cycle in 1/1000 % */
    if (duty_m < 20000) duty_m = 20000;
    int hblank = (int)((int64_t)hd * duty_m / (100000 - duty_m) / 16 * 16);
    int total = hd + hblank;
    uint64_t clk = (uint64_t)total * 1000000 / (uint64_t)hp_ns;   /* kHz */
    t->clock_khz = (uint32_t)(clk / 250 * 250);
    int hsync = total * 8 / 100 / 8 * 8, back = hblank / 2;
    t->hdisplay = hd; t->hsync_start = hd + (hblank - hsync - back); t->hsync_end = t->hsync_start + hsync; t->htotal = total;
    t->vdisplay = h; t->vsync_start = h + 3; t->vsync_end = h + 3 + vsync; t->vtotal = vtotal;
    t->nhsync = true; t->nvsync = false;
}

static void add(struct edid_info *e, const struct display_timing *t) {
    if (!t->clock_khz || !t->hdisplay || !t->vdisplay || t->interlace) return;
    for (int i = 0; i < e->ntimings; i++) {
        struct display_timing *o = &e->timings[i];
        if (o->hdisplay == t->hdisplay && o->vdisplay == t->vdisplay && timing_refresh_hz(o) == timing_refresh_hz(t)) return;
    }
    if (e->ntimings < (int)ARRAY_LEN(e->timings)) e->timings[e->ntimings++] = *t;
}
static void add_dtd(struct edid_info *e, const uint8_t *d, bool preferred) {
    struct display_timing t = { 0 };
    t.clock_khz = (uint32_t)(d[0] | d[1] << 8) * 10;
    if (!t.clock_khz) return;
    int ha = d[2] | (d[4] & 0xF0) << 4, hb = d[3] | (d[4] & 0x0F) << 8;
    int va = d[5] | (d[7] & 0xF0) << 4, vb = d[6] | (d[7] & 0x0F) << 8;
    int hso = d[8] | (d[11] & 0xC0) << 2, hsw = d[9] | (d[11] & 0x30) << 4;
    int vso = (d[10] >> 4) | (d[11] & 0x0C) << 2, vsw = (d[10] & 0x0F) | (d[11] & 0x03) << 4;
    t.hdisplay = ha; t.hsync_start = ha + hso; t.hsync_end = ha + hso + hsw; t.htotal = ha + hb;
    t.vdisplay = va; t.vsync_start = va + vso; t.vsync_end = va + vso + vsw; t.vtotal = va + vb;
    t.interlace = d[17] & 0x80;
    if ((d[17] & 0x18) == 0x18) { t.nhsync = !(d[17] & 0x02); t.nvsync = !(d[17] & 0x04); }   /* digital separate sync */
    else { t.nhsync = t.nvsync = true; }
    t.preferred = preferred;
    if (t.hsync_end > t.htotal || t.vsync_end > t.vtotal) return;
    add(e, &t);
}
static void add_size(struct edid_info *e, int w, int h, int hz) {
    for (size_t i = 0; i < ARRAY_LEN(dmt_modes); i++) if (dmt_modes[i].w == w && dmt_modes[i].h == h && dmt_modes[i].hz == hz) { add(e, &dmt_modes[i].t); return; }
    for (size_t i = 0; i < ARRAY_LEN(established); i++) if (established[i].w == w && established[i].h == h && established[i].hz == hz) { add(e, &established[i].t); return; }
    struct display_timing t;
    timing_cvt(w, h, hz, e->digital, &t);   /* flat panels take reduced blanking */
    add(e, &t);
}

/* a 2-byte standard timing; aspect code 0 means 1:1 before EDID 1.3 and 16:10 after */
static void add_std(struct edid_info *e, const uint8_t *b, const uint8_t *s) {
    if ((s[0] == 1 && s[1] == 1) || s[0] == 0) return;
    int w = (s[0] + 31) * 8, ar = s[1] >> 6, hz = (s[1] & 0x3F) + 60;
    int h = ar == 0 ? (b[18] == 1 && b[19] < 3 ? w : w * 10 / 16) : ar == 1 ? w * 3 / 4 : ar == 2 ? w * 4 / 5 : w * 9 / 16;
    add_size(e, w, h, hz);
}

bool edid_parse(const uint8_t *b, size_t len, struct edid_info *e) {
    memset(e, 0, sizeof *e);
    if (len < 128 || !header_ok(b) || !edid_block_valid(b)) return false;
    uint16_t mfg = (uint16_t)(b[8] << 8 | b[9]);
    e->vendor[0] = (char)('@' + ((mfg >> 10) & 31)); e->vendor[1] = (char)('@' + ((mfg >> 5) & 31)); e->vendor[2] = (char)('@' + (mfg & 31));
    e->digital = b[20] & 0x80;
    /* detailed timings first: the first one is the preferred mode */
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = b + 54 + i * 18;
        if (d[0] || d[1]) { add_dtd(e, d, i == 0); continue; }
        if (d[3] == 0xFC) {   /* monitor name */
            int n = 0;
            while (n < 13 && d[5 + n] != '\n' && d[5 + n]) { e->name[n] = (char)d[5 + n]; n++; }
            while (n > 0 && e->name[n - 1] == ' ') n--;
            e->name[n] = 0;
        } else if (d[3] == 0xFD) e->max_clock_khz = (uint32_t)d[9] * 10000;   /* range limits */
        else if (d[3] == 0xFA) for (int k = 0; k < 6; k++) add_std(e, b, d + 5 + k * 2);   /* more standard timings */
    }
    /* CEA extensions carry the TV/HDMI detailed timings */
    int ext = b[126];
    for (int x = 1; x <= ext && (size_t)(x + 1) * 128 <= len; x++) {
        const uint8_t *c = b + x * 128;
        if (c[0] != 0x02 || !edid_block_valid(c)) continue;
        int dtd = c[2];
        if (dtd < 4) continue;
        for (int off = dtd; off + 18 <= 127; off += 18) if (c[off] || c[off + 1]) add_dtd(e, c + off, false);
    }
    /* established timings */
    uint32_t est = b[35] << 16 | b[36] << 8 | b[37];
    for (int i = 0; i < 17; i++) if (est & (1u << (23 - i)) && established[i].w) add(e, &established[i].t);
    /* standard timings */
    for (int i = 0; i < 8; i++) add_std(e, b, b + 38 + i * 2);
    return true;
}
