/* Gesture capture on the Glassly page.
 *
 * An LVGL event callback on the page root turns stock input into the shared
 * gesture numbering and forwards it to the phone (business 0xF0, GESTURE). Only
 * fires while the page is open or the phone has sent QUERY this connection.
 *
 * UNVERIFIED: the crown/rotary path. On this panel the crown arrives on the
 * launcher's AppListView wheel hook (~600 raw units/detent); whether a
 * full-screen page receives it as an LVGL rotary event could not be confirmed
 * without a device. The key click/long-press path uses the documented
 * LV_EVENT_KEY codes 0x3a/0x3b. See docs/HOOKS.md.
 */
#include "cfw.h"

#define LV_EVENT_ROTARY 0x5eu      /* lv_event_code_t LV_EVENT_ROTARY (v9); unverified on this build */

void cfw_gesture(customCfwContext *ctx, uint8_t type, uint8_t source) {
    if (!ctx->page_open && !ctx->queried) return;
    uint8_t b[7];
    b[0] = RPL_GESTURE;
    b[1] = type;
    b[2] = source;
    uint32_t t = fw_tick();
    b[3] = (uint8_t)t; b[4] = (uint8_t)(t >> 8);
    b[5] = (uint8_t)(t >> 16); b[6] = (uint8_t)(t >> 24);
    cfw_uplink_push(ctx, b, sizeof(b));
}

static void cfw_event_cb(void *e) {
    customCfwContext *ctx = (customCfwContext *)fw_event_user(e);
    if (!ctx) return;
    uint32_t code = fw_event_code(e);
    if (code == LV_EVENT_KEY_CODE) {
        uint32_t key = fw_event_key(e);
        if (key == KEY_CLICK) {
            uint32_t now = fw_tick();
            if (ctx->last_click_tick && (now - ctx->last_click_tick) <= DOUBLE_TAP_MS) {
                cfw_gesture(ctx, GEST_DOUBLE, 0);
                ctx->last_click_tick = 0;
            } else {
                cfw_gesture(ctx, GEST_SINGLE, 0);
                ctx->last_click_tick = now;
            }
        } else if (key == KEY_LONG) {
            cfw_gesture(ctx, GEST_TAP_HOLD, 1);
            ctx->press_start_tick = fw_tick();   /* arm the 2 s local close */
        }
    } else if (code == LV_EVENT_ROTARY) {
        int32_t d = (int32_t)fw_event_key(e);    /* raw wheel delta */
        ctx->crown_accum += d;
        while (ctx->crown_accum >= CROWN_PER_DETENT) {
            cfw_gesture(ctx, GEST_SWIPE_UP, 0);
            ctx->crown_accum -= CROWN_PER_DETENT;
        }
        while (ctx->crown_accum <= -CROWN_PER_DETENT) {
            cfw_gesture(ctx, GEST_SWIPE_DOWN, 0);
            ctx->crown_accum += CROWN_PER_DETENT;
        }
    }
}

void cfw_input_attach(customCfwContext *ctx, void *root) {
    fw_add_event_cb(root, (void *)cfw_event_cb, 0 /* LV_EVENT_ALL */, ctx);
}
