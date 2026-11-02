#include "kernel.h"

static uint32_t table[256];
static bool ready;

static void init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    ready = true;
}

/* raw CRC-32C update: no pre/post inversion (callers supply the seed) */
uint32_t crc32c(uint32_t seed, const void *data, size_t n) {
    if (!ready) init();
    const uint8_t *p = data; uint32_t c = seed;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}
