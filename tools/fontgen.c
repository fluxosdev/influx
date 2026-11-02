/*
 * fontgen - host-side tool that rasterises a TrueType font at a given pixel
 * size into the inFlux bitmap font format (.fnt) using stb_truetype.
 *
 *   fontgen <in.ttf> <size_px> <out.fnt>
 *
 * File layout (all little endian):
 *   struct fnt_header  { "IFNT", u16 size, i16 ascent, i16 descent, u16 line_height,
 *                        u32 glyph_count, u32 data_size }
 *   struct fnt_glyph[glyph_count] { u32 codepoint, i16 x0, i16 y0, u16 w, u16 h,
 *                                   u16 advance_q8, u16 pad, u32 data_offset }
 *   u8 data[data_size]   (8-bit coverage, row major, w*h bytes per glyph)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

#pragma pack(push, 1)
struct fnt_header { char magic[4]; uint16_t size; int16_t ascent, descent; uint16_t line_height; uint32_t glyph_count, data_size; };
struct fnt_glyph  { uint32_t cp; int16_t x0, y0; uint16_t w, h, advance_q8, pad; uint32_t off; };
#pragma pack(pop)

static const uint32_t extra_cps[] = {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2039, 0x203A,
    0x2190, 0x2191, 0x2192, 0x2193, 0x21E7, 0x2303, 0x2318, 0x2325, 0x232B, 0x23CE,
    0x2212, 0x221A, 0x2260, 0x2264, 0x2265, 0x03C0, 0x2713, 0x2717, 0x25B2, 0x25B6,
    0x25BC, 0x25C0, 0x25CF, 0x25CB, 0x2588, 0x2591, 0x2592, 0x2593, 0x2500, 0x2502,
    0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C, 0x2605,
    0x2606, 0x26A0, 0x2302, 0x20AC, 0x2B06, 0x2B07, 0x00D7, 0x00F7, 0x2630, 0x2715,
};

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s in.ttf size_px out.fnt\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *ttf = malloc(n);
    if (fread(ttf, 1, n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) { fprintf(stderr, "bad font\n"); return 1; }
    int px = atoi(argv[2]);
    float scale = stbtt_ScaleForPixelHeight(&info, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);

    size_t ncp = (126 - 32 + 1) + (255 - 160 + 1) + sizeof(extra_cps) / sizeof(extra_cps[0]);
    uint32_t *cps = malloc(ncp * sizeof(uint32_t));
    size_t k = 0;
    for (uint32_t c = 32; c <= 126; c++) cps[k++] = c;
    for (uint32_t c = 160; c <= 255; c++) cps[k++] = c;
    for (size_t i = 0; i < sizeof(extra_cps) / sizeof(extra_cps[0]); i++) cps[k++] = extra_cps[i];

    struct fnt_glyph *glyphs = calloc(ncp, sizeof(*glyphs));
    size_t cap = 1 << 20, used = 0;
    unsigned char *data = malloc(cap);

    for (size_t i = 0; i < ncp; i++) {
        int gi = stbtt_FindGlyphIndex(&info, (int)cps[i]);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&info, gi, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (gi == 0 && cps[i] != 32) w = h = 0; /* missing glyph -> draw nothing */
        if (w < 0) w = 0; if (h < 0) h = 0;
        if (used + (size_t)w * h > cap) { cap *= 2; data = realloc(data, cap); }
        if (w && h) stbtt_MakeGlyphBitmap(&info, data + used, w, h, w, scale, scale, gi);
        glyphs[i].cp = cps[i];
        glyphs[i].x0 = (int16_t)x0; glyphs[i].y0 = (int16_t)y0;
        glyphs[i].w = (uint16_t)w; glyphs[i].h = (uint16_t)h;
        glyphs[i].advance_q8 = (uint16_t)lroundf(adv * scale * 256.0f);
        glyphs[i].off = (uint32_t)used;
        used += (size_t)w * h;
    }

    struct fnt_header hdr;
    memcpy(hdr.magic, "IFNT", 4);
    hdr.size = (uint16_t)px;
    hdr.ascent = (int16_t)lroundf(asc * scale);
    hdr.descent = (int16_t)lroundf(-desc * scale);
    hdr.line_height = (uint16_t)lroundf((asc - desc + gap) * scale);
    hdr.glyph_count = (uint32_t)ncp;
    hdr.data_size = (uint32_t)used;

    FILE *o = fopen(argv[3], "wb");
    if (!o) { perror(argv[3]); return 1; }
    fwrite(&hdr, sizeof hdr, 1, o);
    fwrite(glyphs, sizeof(*glyphs), ncp, o);
    fwrite(data, 1, used, o);
    fclose(o);
    return 0;
}
