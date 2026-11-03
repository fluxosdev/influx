#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* a display timing; horizontal values in pixels, vertical in lines */
struct display_timing {
    uint32_t clock_khz;
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    bool nhsync, nvsync, interlace, preferred;
};

struct edid_info {
    char name[14];                     /* monitor name descriptor */
    char vendor[4];                    /* PNP id, e.g. "DEL" */
    bool digital;                      /* digital input (DVI/HDMI/DP) */
    uint32_t max_clock_khz;            /* range limits descriptor, 0 if none */
    int ntimings;
    struct display_timing timings[48];
};

bool edid_block_valid(const uint8_t *block);   /* header (block 0) and checksum */
/* parse a base block plus any CEA extensions that follow it */
bool edid_parse(const uint8_t *edid, size_t len, struct edid_info *out);
/* VESA CVT 1.1 timing (integer arithmetic); reduced blanking for flat panels */
void timing_cvt(int w, int h, int refresh, bool reduced, struct display_timing *t);
int timing_refresh_hz(const struct display_timing *t);   /* rounded */
