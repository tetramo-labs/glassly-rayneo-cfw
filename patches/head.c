/* Head events arrive on the launcher UI thread, even with our page closed.
 * Hook the dispatch, BEFORE the stock handler's re-pairing early return. */
#include "cfw.h"

void cfw_head_hook(void *handler, const TIONativeMessage *message) {
    customCfwContext *ctx = cfw_ctx_peek();
    uint32_t direction = message ? message->reserved : 0u;
    if (ctx && ctx->queried && message && message->id == 0x1du &&
        (direction == 1u || direction == 2u)) {
        ctx->head_events++;
        ctx->head_direction = (uint8_t)direction;
        /* Source 2 is the head detector, not the button (1) or wheel (0). */
        cfw_gesture(ctx, direction == 2u ? GEST_HEAD_UP : GEST_HEAD_DOWN, 2);
        /* Do not depend on the sleeping page timer or stock wake processing. */
        cfw_uplink_flush(ctx);
    }
    /* Preserve every stock check and side effect, including rebond and DND. */
    fw_handle_head(handler, message);
}

void cfw_head_angle(customCfwContext *ctx, uint8_t angle) {
    if (angle > 90u) angle = 90u;
    ctx->head_angle = angle;
    ctx->head_config_status = 2;
    void *monitor = fw_sysstate_monitor();
    if (!monitor) return;
    int32_t enabled = 0, previous = -1;
    /* Avoid rewriting persistent preferences on every reconnect. */
    fw_pref_read(0x2e, &previous);
    ctx->head_config_status = 3;
    if (previous != angle && fw_pref_write(0x2e, angle) != 0) return;
    fw_pref_read(0x2d, &enabled);
    ctx->head_config_status = 4;
    if (enabled != 1 && fw_pref_write(0x2d, 1) != 0) return;
    /* Apply even when unchanged: the sensor may have restarted. */
    fw_apply_wakeup(monitor);
    ctx->head_config_status = 1; /* stock apply returned; not a sensor ACK */
}
