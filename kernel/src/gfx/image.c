#include "gfx.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

struct canvas *image_from_memory(const void *data, size_t size) {
    const uint32_t *h = data;
    if (size < 12 || h[0] != 0x474D4949u) return NULL;
    uint32_t w = h[1], hh = h[2];
    if (size < 12 + (size_t)w * hh * 4 || w > 8192 || hh > 8192) return NULL;
    struct canvas *c = canvas_create(w, hh);
    memcpy(c->px, h + 3, (size_t)w * hh * 4);
    return c;
}
struct canvas *image_load(const char *path) {
    void *data; size_t size;
    if (vfs_read_file(path, &data, &size) < 0) return NULL;
    struct canvas *c = image_from_memory(data, size);
    kfree(data);
    if (!c) kprintf("image: bad image %s\n", path);
    return c;
}
