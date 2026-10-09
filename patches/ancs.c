/* ANCS relay: forward the iOS notifications the stock firmware already reads
 * over the Apple Notification Center Service to the phone, as business-0xF0
 * records. The RayNeo iO counterpart of the G2 Glassly CFW ancs_relay.c.
 *
 * WHY. iOS never lets a third-party app read other apps' notifications, but it
 * hands them to a paired BLE accessory over ANCS, and the stock firmware's
 * ANCSAdapter subscribes, fetches every attribute and serialises each
 * notification as JSON (NotificationConverter::ancsRawNotificationToJson:
 * notificationUID, appId, appName, title, subtitle, content, location,
 * carrier, timestamp, eventId, eventFlags, categoryID, categoryCount). That
 * JSON only travels in-glasses, through the message processor to
 * NotificationService (on-glass popup, whitelist). This taps it on the way.
 *
 * STOCK SEAM (1.0.4.12). NotificationMessageHandler::handleMessage calls
 * handleAncsNotificationReceived(this, msg) once, at 0x10702e7e; `msg` is a
 * TIONativeMessage whose data/bytes are the JSON text. build.py retargets
 * that one `bl` to cfw_ancs_hook, which queues the JSON and tail-calls the
 * stock handler, so stock behaviour is unchanged.
 *
 * THREADING. handleMessage runs from message_processor_handle, the LVGL
 * timer: the same thread as the CFW page tick that drains the uplink, so the
 * queue needs no locking. Records go out >= 100 ms apart like every reply.
 *
 * CONTRACT (glasses -> phone, business 0xF0):
 *   ANCS [0x17][seq u8][total u16 LE][offset u16 LE][chunk]
 * The JSON of one notification, split into <= 185-byte chunks that share a
 * seq; the phone concatenates by (seq, offset) and parses the JSON once
 * offset + chunk == total. A notification that does not fit the free uplink
 * slots is dropped whole (ancs_drops in the context). Nothing is relayed
 * before the phone has sent QUERY on this connection; there is no enable/
 * lease: the phone decides what to show, and the stock on-glass popup is
 * untouched. eventId 2 is a removal, eventFlags bit 2 a pre-existing
 * notification replayed at subscription time. */
#include "cfw.h"

#define ANCS_HDR   6u
#define ANCS_CHUNK (UPLINK_MAX - ANCS_HDR)
#define ANCS_MAX_JSON 4096u

static uint32_t cfw_uplink_free(const customCfwContext *ctx) {
    return (uint32_t)((ctx->uplink_head + UPLINK_QUEUE - ctx->uplink_tail - 1u) % UPLINK_QUEUE);
}

static void cfw_ancs_relay(customCfwContext *ctx, const uint8_t *json, uint32_t n) {
    uint32_t chunks = (n + ANCS_CHUNK - 1u) / ANCS_CHUNK;
    if (n == 0 || n > ANCS_MAX_JSON || chunks > cfw_uplink_free(ctx)) {
        if (ctx->ancs_drops != 255u) ctx->ancs_drops++;
        return;
    }
    uint8_t seq = ++ctx->ancs_seq;
    uint8_t b[UPLINK_MAX];
    for (uint32_t off = 0; off < n; off += ANCS_CHUNK) {
        uint32_t len = n - off < ANCS_CHUNK ? n - off : ANCS_CHUNK;
        b[0] = RPL_ANCS;
        b[1] = seq;
        cfw_put16(b + 2, (uint16_t)n);
        cfw_put16(b + 4, (uint16_t)off);
        memcpy(b + ANCS_HDR, json + off, len);
        cfw_uplink_push(ctx, b, (uint8_t)(ANCS_HDR + len));
    }
}

/* Retargeted from NotificationMessageHandler::handleMessage (build.py). */
__attribute__((used)) void cfw_ancs_hook(void *self, const TIONativeMessage *m) {
    customCfwContext *ctx = cfw_ctx_peek();
    if (ctx && ctx->queried && m && m->data && m->bytes) {
        uint32_t n = m->bytes;
        while (n && m->data[n - 1] == 0) n--;   /* the converter's NUL, if counted */
        cfw_ancs_relay(ctx, m->data, n);
    }
    FW(void (*)(void *, const TIONativeMessage *), FWA_ancs_notification_handler)(self, m);
}
