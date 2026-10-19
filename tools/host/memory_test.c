#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cfw.h"
#define memcpy test_memcpy
#define memset test_memset
#define memmove test_memmove
#define memcmp test_memcmp
#include "mem.c"
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#include "pixels.h"
int main(void) {
    unsigned char src[512], got[512], want[512];
    for (unsigned i = 0; i < sizeof src; i++) src[i] = (unsigned char)(i * 37u);
    for (unsigned n = 0; n <= 256; n++) for (unsigned a = 0; a < 8; a++) for (unsigned b = 0; b < 8; b++) {
        memset(got, 0xaa, sizeof got); memset(want, 0xaa, sizeof want);
        test_memcpy(got + a, src + b, n); memcpy(want + a, src + b, n); assert(!memcmp(got, want, sizeof got));
        test_memset(got + a, 0x123, n); memset(want + a, 0x123, n); assert(!memcmp(got, want, sizeof got));
        memcpy(got, src, sizeof got); memcpy(want, src, sizeof want);
        test_memmove(got + a, got + b, n); memmove(want + a, want + b, n); assert(!memcmp(got, want, sizeof got));
    }
    uint32_t aligned[CFW_W / 4]; uint8_t *l8 = (uint8_t *)aligned;
    uint16_t packed[CFW_W / 2 + 2]; uint8_t *g4 = (uint8_t *)packed;
    // Test the LCDC's alternating 4-/2-byte row alignment and every L8 level.
    for (unsigned shift = 0; shift <= 2; shift += 2) for (unsigned seed = 0; seed < 256; seed++) {
        memset(g4, 0xaa, sizeof packed);
        for (unsigned x = 0; x < CFW_W; x++) l8[x] = (uint8_t)(seed + x * 37u);
        cfw_pack_l8_row(g4 + shift, l8);
        for (unsigned x = 0; x < CFW_W; x += 2) assert(g4[shift + x / 2] == (uint8_t)((l8[x] & 0xf0) | (l8[x+1] >> 4)));
        assert(g4[shift + CFW_W / 2] == 0xaa);
        if (shift) assert(g4[0] == 0xaa && g4[1] == 0xaa);
    }
    puts("memory/packing: all alignments, overlapping moves, tails and gray levels passed");
}
