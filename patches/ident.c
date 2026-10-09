/* IDENT, DIAG and PAGE-state uplink replies.
 *
 * IDENT answers a QUERY and must be sendable without the page open, without
 * the launcher being idle, and without the LVGL hop: the phone classifies as
 * stock after 2 s of silence. Since revision 3 it is sent synchronously from
 * the RNLink RX callback (rnlink_if_send_payload only selects a channel and
 * hands the bytes to protocol_data_transfer; the stock RX path itself sends
 * from that context). DIAG is a debugging reply the phone only logs.
 */
#include "cfw.h"

static uint32_t cfw_ident_fill(uint8_t *b, uint8_t flags) {
    static const char name[] = REV_STRING;
    uint8_t *p = b;
    *p++ = RPL_IDENT;
    *p++ = REV_MAJOR;                 /* ver */
    cfw_put16(p, CFW_W); p += 2;
    cfw_put16(p, CFW_H); p += 2;
    cfw_put16(p, MAX_CHUNK); p += 2;
    cfw_put16(p, STORE_KIB); p += 2;
    cfw_put16(p, REV_MAJOR); p += 2;      /* revision */
    *p++ = flags;
    for (unsigned i = 0; i < sizeof(name) - 1; i++) *p++ = (uint8_t)name[i];
    return (uint32_t)(p - b);
}

/* RX thread: IDENT straight to the link. `ctx` may be NULL (no context cell). */
void cfw_ident_direct(const customCfwContext *ctx) {
    uint8_t b[64];
    uint8_t flags = (uint8_t)IDENT_FLAGS | (ctx ? (uint8_t)IDENT_FLAG_CONTEXT : 0u);
    uint32_t n = cfw_ident_fill(b, flags);
    fw_send(BIZ_GLASSLY, b, n, 0);
}

/* DIAG [15][source][entry u32][ctx u32][ui_module u8][reg_rc i8][send_rc i8][timer u32][tick u32]
 * source 1 = RX thread (sent directly), 2 = LVGL thread (queued). */
static uint32_t cfw_diag_fill(uint8_t *b, uint8_t source, const customCfwContext *ctx) {
    uint8_t *p = b;
    *p++ = RPL_DIAG;
    *p++ = source;
    cfw_put32(p, (uint32_t)(uintptr_t)cfw_rn_entry()); p += 4;
    cfw_put32(p, (uint32_t)(uintptr_t)ctx); p += 4;
    *p++ = ctx ? ctx->ui_module : 0u;
    *p++ = ctx ? (uint8_t)ctx->ui_reg_rc : 0u;
    *p++ = ctx ? (uint8_t)ctx->ui_send_rc : 0u;
    cfw_put32(p, ctx ? (uint32_t)(uintptr_t)ctx->timer : 0u); p += 4;
    cfw_put32(p, fw_tick()); p += 4;
    return (uint32_t)(p - b);
}

void cfw_diag_direct(const customCfwContext *ctx) {
    uint8_t b[24];
    uint32_t n = cfw_diag_fill(b, DIAG_SRC_RX, ctx);
    fw_send(BIZ_GLASSLY, b, n, 0);
}

void cfw_send_diag(customCfwContext *ctx) {
    uint8_t b[24];
    uint32_t n = cfw_diag_fill(b, DIAG_SRC_UI, ctx);
    cfw_uplink_push(ctx, b, (uint8_t)n);
}

void cfw_send_page_state(customCfwContext *ctx, uint8_t state, uint8_t reason) {
    uint8_t b[3] = {RPL_PAGE, state, reason};
    cfw_uplink_push(ctx, b, sizeof(b));
}
