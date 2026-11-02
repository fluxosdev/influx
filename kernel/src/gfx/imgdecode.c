/* PNG, JPEG, GIF (first frame) and BMP decoding for the web browser, via stb_image.
 * Built with SSE enabled like the OpenGL renderer (see the Makefile). */
#include "gfx.h"
#include "mm.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
static int stbi_abs(int v) { return v < 0 ? -v : v; }
#define abs stbi_abs

#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz) kmalloc(sz)
#define STBI_REALLOC(p, sz) krealloc(p, sz)
#define STBI_FREE(p) kfree(p)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../../third_party/stb/stb_image.h"

#define MAX_DIM 4096

struct canvas *image_decode(const void *data, size_t size) {
    if (!data || size < 8 || size > 0x7FFFFFFF) return NULL;
    int w, h, comp;
    if (!stbi_info_from_memory(data, (int)size, &w, &h, &comp)) return NULL;
    if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) return NULL;
    unsigned char *px = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
    if (!px) return NULL;
    struct canvas *c = canvas_create(w, h);
    for (int i = 0; i < w * h; i++) {
        const unsigned char *p = px + i * 4;
        c->px[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    stbi_image_free(px);
    return c;
}
