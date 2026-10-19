/* Real head events arrive on the launcher UI thread, including while the
 * Glassly page is closed. Do not replace the sensor's single stock callback. */
#include "cfw.h"

void cfw_head_hook(void *monitor, uint32_t direction) {
    /* Preserve wake/DND behavior and the stock launcher notification. */
    fw_notify_head(monitor, direction);
    customCfwContext *ctx = cfw_ctx_peek();
    if (!ctx || !ctx->queried) return;
    if (direction != 1u && direction != 2u) return;
    /* Source 2 is the head detector, not the button (1) or wheel (0). */
    cfw_gesture(ctx, direction == 2u ? GEST_HEAD_UP : GEST_HEAD_DOWN, 2);
    /* A closed page / sleeping screen must not depend on the page timer. */
    cfw_uplink_flush(ctx);
}

void cfw_head_angle(uint8_t angle) {
    void *monitor = fw_sysstate_monitor();
    if (!monitor) return;
    int32_t enabled = 0, previous = -1;
    if (angle > 90u) angle = 90u;
    /* Avoid rewriting persistent preferences on every reconnect. */
    fw_pref_read(0x2e, &previous);
    if (previous != angle && fw_pref_write(0x2e, angle) != 0) return;
    fw_pref_read(0x2d, &enabled);
    if (enabled != 1 && fw_pref_write(0x2d, 1) != 0) return;
    /* Apply even when unchanged: the sensor may have restarted. */
    fw_apply_wakeup(monitor);
}
