/*
 * imgconv - host-side tool that converts a PNG into the inFlux raw image
 * format (.img) using stb_image.
 *
 *   imgconv <in.png> <out.img>
 *
 * Layout: "IIMG", u32 width, u32 height, then width*height u32 pixels as
 * 0xAARRGGBB with straight (non-premultiplied) alpha.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb/stb_image.h"

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s in.png out.img\n", argv[0]); return 1; }
    int w, h, n;
    unsigned char *px = stbi_load(argv[1], &w, &h, &n, 4);
    if (!px) { fprintf(stderr, "%s: %s\n", argv[1], stbi_failure_reason()); return 1; }
    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 1; }
    uint32_t hdr[3] = { 0x474D4949u /* "IIMG" */, (uint32_t)w, (uint32_t)h };
    fwrite(hdr, 4, 3, o);
    uint32_t *out = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        unsigned char r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], a = px[i * 4 + 3];
        out[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    fwrite(out, 4, (size_t)w * h, o);
    fclose(o);
    stbi_image_free(px);
    free(out);
    return 0;
}
