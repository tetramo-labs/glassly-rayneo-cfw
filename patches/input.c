/* Gesture capture on the Glassly page: the crown through the launcher's raw
 * wheel listener, the button through the button driver's interceptor chain
 * (both below LVGL),
 * forwarded to the phone as GESTURE replies (business 0xF0). Only while the
 * page is open or the phone has sent QUERY this connection.
 */
#include "cfw.h"

void cfw_gesture(customCfwContext *ctx, uint8_t type, uint8_t source) {
    if (!ctx->page_open && !ctx->queried) return;
    /* gestures never take the last two uplink slots: replies come first */
    if ((uint8_t)((ctx->uplink_head + UPLINK_QUEUE - ctx->uplink_tail - 1u) % UPLINK_QUEUE) < 3u) return;
    uint8_t b[7];
    b[0] = RPL_GESTURE;
    b[1] = type;
    b[2] = source;
    uint32_t t = fw_tick();
    b[3] = (uint8_t)t; b[4] = (uint8_t)(t >> 8);
    b[5] = (uint8_t)(t >> 16); b[6] = (uint8_t)(t >> 24);
    cfw_uplink_push(ctx, b, sizeof(b));
}

/* ---- button: the RayNeo button driver's interceptor chain ---------------- */
/* rayneo_intercept_btn_event(event) runs on the button poll thread and calls
 * every registered interceptor cb(event) in list order until one returns
 * nonzero; an intercepted event is neither published to the launcher by its
 * key_monitor nor queued for the LVGL buttons indev. Stock order: the display
 * manager (wake on FIRST_PRESS, user activity), key_monitor, sm_monitor
 * (pairing on HOLD_5S). Ours is moved right behind the display manager's
 * node, so a press still wakes the glasses while the launcher never sees the
 * clicks the page owns. Holds of 3 s and more stay the system's. */
#define BTN_CFW_NAME "glassly"

static void keyq_push(customCfwContext *ctx, int event) {
    uint8_t w = ctx->keyq_w, n = (uint8_t)((w + 1u) % KEYQ_SIZE);
    if (n != ctx->keyq_r) { ctx->keyq[w] = (uint8_t)event; ctx->keyq_w = n; }
}

static int cfw_btn_interceptor(int event) {
    customCfwContext *ctx = cfw_ctx_peek();
    if (!ctx || !ctx->page_open) return 0;
    switch (event) {
    case BTN_EV_FIRST_PRESS:
    case BTN_EV_RELEASED:
        keyq_push(ctx, event);
        return 0;                       /* press state flows on (indev, activity) */
    case BTN_EV_CLICK:
    case BTN_EV_DOUBLE_CLICK:
    case BTN_EV_HOLD_1S:
        keyq_push(ctx, event);
        return 1;                       /* the page owns these */
    default:
        return 0;
    }
}

/* UI thread, first page open: register once and keep it (there is no
 * unregister API); the callback checks page_open itself. */
void cfw_input_btn_listen(customCfwContext *ctx) {
    if (ctx->btn_hooked) return;
    ctx->btn_hooked = 1;
    fw_btn_register(BTN_CFW_NAME, (void *)cfw_btn_interceptor);   /* appends at the tail */
    cfw_btn_node *h = *(cfw_btn_node *volatile *)BTN_LIST_HEAD;
    if (!h || !h->next) return;
    cfw_btn_node *p = h, *t = h->next;
    while (t->next) { p = t; t = t->next; }
    if (t->cb != cfw_btn_interceptor) return;                     /* registration refused (logged by the driver) */
    if (p == h) return;                                           /* already second */
    /* detach the tail first so the list stays valid for a concurrent walk */
    p->next = 0;
    t->next = h->next;
    h->next = t;
}

/* UI thread (page timer): queued button events -> gestures. `source` carries
 * the driver's event code so the logs show what the glasses reported. */
void cfw_input_poll(customCfwContext *ctx) {
    while (ctx->keyq_r != ctx->keyq_w) {
        uint8_t event = ctx->keyq[ctx->keyq_r];
        ctx->keyq_r = (uint8_t)((ctx->keyq_r + 1u) % KEYQ_SIZE);
        switch (event) {
        case BTN_EV_FIRST_PRESS:
            ctx->press_start_tick = fw_tick();        /* the tick closes the page after LONG_CLOSE_MS held */
            break;
        case BTN_EV_RELEASED:
            ctx->press_start_tick = 0;
            break;
        case BTN_EV_CLICK:        cfw_gesture(ctx, GEST_SINGLE, event); break;
        case BTN_EV_DOUBLE_CLICK: cfw_gesture(ctx, GEST_DOUBLE, event); break;
        case BTN_EV_HOLD_1S:      cfw_gesture(ctx, GEST_TAP_HOLD, event); break;
        default: break;
        }
    }
}

/* Raw wheel listener (InputEventMonitor). The listener is called as
 * fn(delta, user): AppListView's lambda swaps the two before calling
 * handleRawWheelInput(this, delta) (verified 2026-10-09; the first build had
 * them reversed and turned the context pointer into ~700k detents). Delta is
 * in raw units, CROWN_PER_DETENT per detent, one swipe gesture per detent,
 * at most WHEEL_MAX_PER_CALL per callback so a burst cannot starve replies. */
#define WHEEL_MAX_DELTA     (CROWN_PER_DETENT * 50)
#define WHEEL_MAX_PER_CALL  4
static void cfw_wheel_cb(int32_t delta, void *user) {
    customCfwContext *ctx = cfw_ctx_peek();
    (void)user;
    if (!ctx || !ctx->page_open || !delta) return;
    if (delta > WHEEL_MAX_DELTA || delta < -WHEEL_MAX_DELTA) return;   /* not a crown delta */
    ctx->crown_accum += delta;
    int sent = 0;
    while (ctx->crown_accum >= CROWN_PER_DETENT && sent < WHEEL_MAX_PER_CALL) {
        cfw_gesture(ctx, GEST_SWIPE_UP, 0);
        ctx->crown_accum -= CROWN_PER_DETENT;
        sent++;
    }
    while (ctx->crown_accum <= -CROWN_PER_DETENT && sent < WHEEL_MAX_PER_CALL) {
        cfw_gesture(ctx, GEST_SWIPE_DOWN, 0);
        ctx->crown_accum += CROWN_PER_DETENT;
        sent++;
    }
    if (ctx->crown_accum > CROWN_PER_DETENT * 2) ctx->crown_accum = CROWN_PER_DETENT * 2;
    if (ctx->crown_accum < -CROWN_PER_DETENT * 2) ctx->crown_accum = -CROWN_PER_DETENT * 2;
}

void cfw_input_wheel_listen(customCfwContext *ctx, int on) {
    void *monitor = fw_input_monitor();
    if (!monitor) return;
    if (on && !ctx->wheel_listening) {
        if (fw_wheel_register(monitor, (void *)cfw_wheel_cb, ctx)) ctx->wheel_listening = 1;
    } else if (!on && ctx->wheel_listening) {
        fw_wheel_unregister(monitor, (void *)cfw_wheel_cb, ctx);
        ctx->wheel_listening = 0;
    }
}
