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

/* standard CRC-32 (IEEE 802.3, reflected 0xEDB88320) as used by GPT */
static uint32_t ieee_table[256]; static bool ieee_ready;
uint32_t crc32_ieee(const void *data, size_t n) {
    if (!ieee_ready) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; ieee_table[i] = c; } ieee_ready = true; }
    const uint8_t *p = data; uint32_t c = 0xFFFFFFFFu;
    while (n--) c = ieee_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
