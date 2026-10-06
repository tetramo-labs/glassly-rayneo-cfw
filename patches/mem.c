/* Freestanding mem* so the module is self-contained: clang emits implicit calls
 * to these for struct copies/zeroing, and we never pull in a libc. They are the
 * external symbols the single-TU blob needs; build.py resolves the intra-module
 * branches to them. */
#include "cfw.h"

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n) {
    uint8_t *dp = (uint8_t *)d;
    while (n--) *dp++ = (uint8_t)c;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = (uint8_t *)d;
    const uint8_t *sp = (const uint8_t *)s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *ap = (const uint8_t *)a, *bp = (const uint8_t *)b;
    while (n--) { if (*ap != *bp) return (int)*ap - (int)*bp; ap++; bp++; }
    return 0;
}
