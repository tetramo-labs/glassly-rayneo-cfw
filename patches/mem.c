/* Freestanding memory operations. Canvas clears/copies touch 118,800 bytes
 * per eye; use aligned words on Cortex-M33 instead of byte-at-a-time loops.
 * may_alias makes accesses to arbitrary object representations well-defined.
 * The build uses -fno-builtin, so these loops cannot recurse into libc. */
#include "cfw.h"
typedef uint32_t cfw_mem_word __attribute__((__may_alias__));

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    if (((uintptr_t)dp & 3u) == ((uintptr_t)sp & 3u)) {
        while (n && ((uintptr_t)dp & 3u)) { *dp++ = *sp++; n--; }
        cfw_mem_word *dw = (cfw_mem_word *)(void *)dp;
        const cfw_mem_word *sw = (const cfw_mem_word *)(const void *)sp;
        while (n >= 16) { dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3]; dw += 4; sw += 4; n -= 16; }
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        dp = (uint8_t *)dw; sp = (const uint8_t *)sw;
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n) {
    uint8_t *dp = d; uint8_t byte = (uint8_t)c;
    while (n && ((uintptr_t)dp & 3u)) { *dp++ = byte; n--; }
    uint32_t word = (uint32_t)byte * 0x01010101u;
    cfw_mem_word *dw = (cfw_mem_word *)(void *)dp;
    while (n >= 16) { dw[0] = word; dw[1] = word; dw[2] = word; dw[3] = word; dw += 4; n -= 16; }
    while (n >= 4) { *dw++ = word; n -= 4; }
    dp = (uint8_t *)dw;
    while (n--) *dp++ = byte;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    if ((uintptr_t)dp <= (uintptr_t)sp) return memcpy(d, s, n);
    dp += n; sp += n;
    if (((uintptr_t)dp & 3u) == ((uintptr_t)sp & 3u)) {
        while (n && ((uintptr_t)dp & 3u)) { *--dp = *--sp; n--; }
        cfw_mem_word *dw = (cfw_mem_word *)(void *)dp;
        const cfw_mem_word *sw = (const cfw_mem_word *)(const void *)sp;
        while (n >= 4) { *--dw = *--sw; n -= 4; }
        dp = (uint8_t *)dw; sp = (const uint8_t *)sw;
    }
    while (n--) *--dp = *--sp;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *ap = a, *bp = b;
    while (n--) { if (*ap != *bp) return (int)*ap - (int)*bp; ap++; bp++; }
    return 0;
}
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
/* Struct snapshots may lower to these aligned-copy AEABI entry points. */
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
