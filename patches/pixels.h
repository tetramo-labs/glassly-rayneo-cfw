#ifndef CFW_PIXELS_H
#define CFW_PIXELS_H
#include "cfw.h"
/* LCDC rows are 2-byte aligned (stride 302); L8 rows are 4-byte aligned
 * (stride 540). Four L8 pixels become one uint16_t, high nibble first.
 * Little-endian, as both the AP and its framebuffer are. */
typedef uint32_t cfw_pixel_word __attribute__((__may_alias__));
typedef uint16_t cfw_pixel_pair __attribute__((__may_alias__));
static void cfw_pack_l8_row(uint8_t *dst, const uint8_t *src) {
    const cfw_pixel_word *s = (const cfw_pixel_word *)(const void *)src;
    cfw_pixel_pair *d = (cfw_pixel_pair *)(void *)dst;
    for (uint32_t i = 0; i < CFW_W / 4u; i++) {
        uint32_t v = s[i];
        d[i] = (uint16_t)((v & 0xf0u) | ((v >> 12) & 0x0fu) | ((v >> 8) & 0xf000u) | ((v >> 20) & 0x0f00u));
    }
}
#endif
