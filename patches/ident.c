/* IDENT and PAGE-state uplink replies.
 *
 * IDENT answers a QUERY and must be sendable without the page open and without
 * the launcher being idle (the phone classifies as stock after 2 s of silence).
 */
#include "cfw.h"

void cfw_send_ident(customCfwContext *ctx) {
    static const char name[] = REV_STRING;
    uint8_t b[64];
    uint8_t *p = b;
    *p++ = RPL_IDENT;
    *p++ = REV_MAJOR;                 /* ver */
    cfw_put16(p, CFW_W); p += 2;
    cfw_put16(p, CFW_H); p += 2;
    cfw_put16(p, MAX_CHUNK); p += 2;
    cfw_put16(p, STORE_KIB); p += 2;
    cfw_put16(p, REV_MAJOR); p += 2;      /* revision */
    *p++ = (uint8_t)IDENT_FLAGS;      /* bit0 zlib yes, bit1 raw-0xF0 downlink no */
    for (unsigned i = 0; i < sizeof(name) - 1; i++) *p++ = (uint8_t)name[i];
    cfw_uplink_push(ctx, b, (uint8_t)(p - b));
}

void cfw_send_page_state(customCfwContext *ctx, uint8_t state, uint8_t reason) {
    uint8_t b[3] = {RPL_PAGE, state, reason};
    cfw_uplink_push(ctx, b, sizeof(b));
}
