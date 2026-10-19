/* Business 0xF0 link: the downlink packet channel, the control channel, and the
 * uplink replies.
 *
 * DOWNLINK (revision 2): the module registers its own RNLink callback table for
 * business 0xF0 with rnlink_if_register_callback, from the same launcher init
 * routine (LinkMonitor::registerLinkCallback) that registers the stock tables.
 * Each business-0xF0 AA55 frame payload is one packet [opt u8][stream u8][chunk]:
 * opt bits 0x01 LENS, 0x80 RESET, 0x40 END, 0x10 CONTROL; `stream` is the low
 * byte of the RESET frame's AA55 messageNumber (RNLink does not hand the number
 * to the callback, so the phone repeats it in every packet) and is echoed in
 * ACK/NACK/REPLY. A control packet is [0x10][stream][ctrl].
 *
 * The RX callback runs on the RNLink thread: it answers QUERY with IDENT (and a
 * DIAG) right there, then validates, deep-copies and enqueues every packet to
 * the LVGL thread through message_processor_send (module 5, or 3 if 5 is
 * taken; ids must be 1..15 and a taken id is refused without side effects).
 * All packet parsing, LVGL mutation and queued uplink run on the LVGL thread;
 * replies (<= 192 bytes) go out in order as soon as they are queued, after
 * the packet that produced them (revision 8; the G2 sends its inline too).
 * The UPLINK uses rnlink_if_send_payload(0xF0, ...).
 */
#include "cfw.h"

/* ---- CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the decoded message -- */
static uint16_t crc16_ccitt(const uint8_t *d, uint32_t n) {
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (unsigned b = 0; b < 8; b++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0));
    }
    return crc;
}

/* ---- zlib allocation shims (one persistent inflater per connection) -------- */
static void *cfw_zalloc(void *opaque, uint32_t items, uint32_t size) {
    (void)opaque;
    if (size && items > 0xffffffffu / size) return 0;
    return fw_memalign(8, items * size);
}
static void cfw_zfree(void *opaque, void *p) { (void)opaque; fw_free(p); }

static void inflate_reset(customCfwContext *ctx) {
    if (ctx->stream.inflater) {
        fw_inflate_end(ctx->stream.inflater);
        fw_free(ctx->stream.inflater);
        ctx->stream.inflater = 0;
    }
}

static int inflate_record(customCfwContext *ctx, const uint8_t *in, uint32_t n,
                          uint8_t *out, uint32_t *outlen) {
    if (!ctx->stream.inflater) {
        cfw_zstream *z = (cfw_zstream *)fw_memalign(8, (uint32_t)sizeof(*z));
        if (!z) return 0;
        cfw_zero(z, (uint32_t)sizeof(*z));
        z->zalloc = cfw_zalloc;
        z->zfree = cfw_zfree;
        if (fw_inflate_init(z) != 0) { fw_free(z); return 0; }
        ctx->stream.inflater = z;
    }
    /* every record ends in a complete SYNC_FLUSH: 00 00 FF FF */
    if (n < 4 || in[n - 4] || in[n - 3] || in[n - 2] != 0xff || in[n - 1] != 0xff) return 0;
    cfw_zstream *z = ctx->stream.inflater;
    z->next_in = in;
    z->avail_in = n;
    z->next_out = out;
    z->avail_out = CFW_MESSAGE_CAP;
    while (z->avail_in) {
        uint32_t ai = z->avail_in, ao = z->avail_out;
        if (fw_inflate(z) != 0 || !z->avail_out || (ai == z->avail_in && ao == z->avail_out)) return 0;
    }
    *outlen = CFW_MESSAGE_CAP - z->avail_out;
    z->next_in = 0;
    z->next_out = 0;
    return 1;
}

/* ---- uplink queue (drained in order after each packet and each page tick) ---- */
int cfw_uplink_push(customCfwContext *ctx, const uint8_t *data, uint8_t len) {
    if (len > UPLINK_MAX) return 0;
    uint8_t nxt = (uint8_t)((ctx->uplink_tail + 1) % UPLINK_QUEUE);
    if (nxt == ctx->uplink_head) return 0;  /* full: drop (phone retries) */
    ctx->uplink[ctx->uplink_tail].len = len;
    memcpy(ctx->uplink[ctx->uplink_tail].data, data, len);
    ctx->uplink_tail = nxt;
    return 1;
}

void cfw_uplink_flush(customCfwContext *ctx) {
    while (ctx->uplink_head != ctx->uplink_tail) {
        cfw_uplink *u = &ctx->uplink[ctx->uplink_head];
        if (fw_send(BIZ_GLASSLY, u->data, u->len, 0) != 0) return;   /* link busy: the next tick retries */
        ctx->uplink_head = (uint8_t)((ctx->uplink_head + 1) % UPLINK_QUEUE);
        ctx->last_uplink_tick = fw_tick();
    }
}

/* ---- reply builders -------------------------------------------------------- */

static void send_ack(customCfwContext *ctx, uint8_t lens, uint8_t kind, uint16_t size, uint16_t crc) {
    uint8_t b[9];
    b[0] = kind;                       /* ACK or NACK */
    b[1] = ctx->stream.stream_id;
    cfw_put16(b + 2, ctx->stream.ordinal);
    b[4] = lens;
    cfw_put16(b + 5, size);
    cfw_put16(b + 7, crc);
    cfw_uplink_push(ctx, b, sizeof(b));
}

static void send_reply(customCfwContext *ctx, uint8_t lens, uint8_t mode, uint16_t request, uint8_t status, uint16_t epoch,
                       const uint8_t *extra, uint8_t extra_len) {
    uint8_t b[15 + REPLY_EXTRA_MAX];
    b[0] = RPL_REPLY;
    b[1] = ctx->stream.stream_id;
    cfw_put16(b + 2, ctx->stream.ordinal);
    b[4] = lens;
    b[5] = mode;
    cfw_put16(b + 6, request);
    b[8] = status;
    cfw_put16(b + 9, epoch);
    cfw_put32(b + 11, ctx->lens[lens == LENS_RIGHT ? 1 : 0].revision);   /* the lens's journal revision */
    if (extra_len > REPLY_EXTRA_MAX) extra_len = REPLY_EXTRA_MAX;
    for (uint8_t i = 0; i < extra_len; i++) b[15 + i] = extra[i];
    cfw_uplink_push(ctx, b, (uint8_t)(15 + extra_len));
}

/* ---- record handler -------------------------------------------------------- */
static void cfw_record(customCfwContext *ctx, const uint8_t *wire, uint32_t total) {
    uint8_t flags = wire[0];
    uint16_t len = (uint16_t)(wire[1] | (wire[2] << 8));
    uint16_t crc = (uint16_t)(wire[3] | (wire[4] << 8));
    const uint8_t *body = wire + 5;
    (void)total;

    /* every targeted lens answers (the phone counts an ACK per lens); a NACK
     * before dispatch goes to each of them too */
    uint8_t targets = flags & REC_LENS_MASK;
    if (!targets) { ctx->last_nack = CFW_NACK_FLAGS; ctx->nack_count++; send_ack(ctx, LENS_LEFT, RPL_NACK, 0, crc); ctx->stream.ordinal++; return; }
#define NACK_ALL(size) do { for (uint8_t lb = LENS_LEFT; lb <= LENS_RIGHT; lb <<= 1) if (targets & lb) send_ack(ctx, lb, RPL_NACK, (uint16_t)(size), crc); } while (0)
    if (flags & REC_RESET_CTX) inflate_reset(ctx);

    uint8_t *decoded = 0;
    const uint8_t *data;
    uint32_t dlen;
    if (flags & REC_COMPRESSED) {
        decoded = (uint8_t *)fw_memalign(8, CFW_MESSAGE_CAP);
        if (!decoded || !inflate_record(ctx, body, len, decoded, &dlen)) {
            if (decoded) fw_free(decoded);
            ctx->last_nack = CFW_NACK_INFLATE; ctx->nack_count++;
            NACK_ALL(0); ctx->stream.ordinal++; return;
        }
        data = decoded;
    } else {
        data = body; dlen = len;
    }

    if (crc16_ccitt(data, dlen) != crc) {
        if (decoded) fw_free(decoded);
        ctx->last_nack = CFW_NACK_CRC; ctx->nack_count++;
        NACK_ALL(dlen); ctx->stream.ordinal++; return;
    }

    /* cache messages require the page to be open (spec §3.2) */
    if (!ctx->page_open) {
        if (decoded) fw_free(decoded);
        NACK_ALL(dlen); ctx->stream.ordinal++; return;
    }

    int first = 1;
    for (uint8_t lb = LENS_LEFT; lb <= LENS_RIGHT; lb <<= 1) {
        if (!(targets & lb)) continue;
        cfw_lens *L = &ctx->lens[lb == LENS_LEFT ? 0 : 1];
        uint8_t status = ST_APPLIED;
        uint16_t request = 0;
        uint8_t extra[REPLY_EXTRA_MAX];
        uint8_t extra_len = 0;
        int handled;
        uint8_t reply_mode = data[0];
        if (cfw_raster_mode(data[0])) {
            /* the direct-framebuffer family: ACK/NACK only, plus the REPLY of a
             * bundle's last cache segment (reply_mode 0 = none), as the G2 */
            reply_mode = 0;
            handled = cfw_raster_message(ctx, L, data, dlen, first, &reply_mode, &status, &request, extra, &extra_len);
        } else {
            handled = dlen >= 5 ? cfw_scene_message(ctx, L, data, dlen, &status, &request, first, extra, &extra_len) : 0;
        }
        if (handled) {
            if (reply_mode) send_reply(ctx, lb, reply_mode, request, status, ctx->stream.epoch, extra, extra_len);   /* the current epoch, as the G2 */
            send_ack(ctx, lb, RPL_ACK, (uint16_t)dlen, crc);
        } else {
            send_ack(ctx, lb, RPL_NACK, (uint16_t)dlen, crc);
        }
        first = 0;
    }
#undef NACK_ALL
    if (decoded) fw_free(decoded);
    ctx->stream.ordinal++;
}

/* ---- record accumulator ---------------------------------------------------- */
static void stream_feed(customCfwContext *ctx, const uint8_t *p, uint32_t n) {
    cfw_stream *s = &ctx->stream;
    if (!s->buffer) {
        s->buffer = (uint8_t *)fw_memalign(8, CFW_REC_BUF);
        if (!s->buffer) return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (s->used < CFW_REC_BUF) s->buffer[s->used++] = p[i];
        if (s->used >= 5) {
            uint16_t rlen = (uint16_t)(s->buffer[1] | (s->buffer[2] << 8));
            uint32_t rtotal = 5u + rlen;
            if (rtotal > CFW_REC_BUF) { s->used = 0; continue; }  /* malformed: drop */
            if (s->used >= rtotal) {
                cfw_record(ctx, s->buffer, rtotal);
                uint32_t rest = s->used - rtotal;
                memmove(s->buffer, s->buffer + rtotal, rest);
                s->used = rest;
            }
        }
    }
}

/* ---- control channel ------------------------------------------------------- */
void cfw_control(customCfwContext *ctx, uint8_t ctrl, uint8_t arg) {
    uint8_t reason;
    switch (ctrl) {
    case CTRL_QUERY:
        /* IDENT already went out from the RX callback; the queued DIAG proves the hop. */
        ctx->queried = 1;
        cfw_send_diag(ctx);
        break;
    case CTRL_PAGE_OPEN:
        ctx->page_by_takeover = 0;             /* the phone's page, takeover allowed again */
        if (cfw_page_open(ctx, &reason)) cfw_send_page_state(ctx, 1, PAGE_OK);
        else cfw_send_page_state(ctx, 0, reason);
        break;
    case CTRL_PAGE_CLOSE:
        cfw_page_close(ctx, 0);
        break;
    case CTRL_KEEPALIVE:
        if (ctx->page_open) {
            ctx->page_by_takeover = 0;
            ctx->lease_deadline = fw_tick() + PAGE_LEASE_MS;
            cfw_send_page_state(ctx, 1, PAGE_OK);
        }
        break;
    case CTRL_HEAD_ANGLE:
        cfw_head_angle(arg);
        break;
    case CTRL_COMPASS:
        cfw_compass_control(ctx, arg);
        break;
    case CTRL_DISPLAY_PROBE:
        cfw_display_probe(ctx, arg);
        break;
    default: break;
    }
}

/* ---- one stream packet (opt, stream, chunk) -------------------------------- */
static void cfw_packet(customCfwContext *ctx, uint8_t opt, uint8_t stream, const uint8_t *chunk, uint32_t len) {
    if (opt & PKT_CONTROL) {
        if (len >= 1 && (chunk[0] != CTRL_HEAD_ANGLE || len >= 2))
            cfw_control(ctx, chunk[0], len >= 2 ? chunk[1] : 0u);
        return;
    }
    if (opt & PKT_RESET) {
        ctx->stream.used = 0; ctx->stream.in_stream = 1; ctx->stream.ordinal = 0;
        ctx->stream.stream_id = stream;
    }
    uint8_t hold = ctx->disp_hold;
    ctx->disp_hold = 1; /* a packet may finish both eyes' SHOW records */
    if (len) stream_feed(ctx, chunk, len);
    ctx->disp_hold = hold;
    if (!hold && ctx->disp_dirty) cfw_page_present(ctx);
    if (opt & PKT_END) { ctx->stream.used = 0; ctx->stream.in_stream = 0; }
}

/* ======================= LVGL thread: packet pump =========================== */
static void cfw_ui_handler(void *arg, void *user) {
    (void)user;
    const TIONativeMessage *m = (const TIONativeMessage *)arg;
    if (!m || !m->data) return;
    customCfwContext *ctx = cfw_ctx_peek();
    if (m->id == TIO_WAKE_ID) {
        if (ctx) { ctx->wake_posted = 0; cfw_page_tick(ctx); }
        return;
    }
    if (m->id != TIO_MSG_ID) return;
    if (ctx && m->bytes >= 2) {
        cfw_page_ensure_timer(ctx);
        cfw_packet(ctx, m->data[0], m->data[1], m->data + 2, m->bytes - 2);
        cfw_uplink_flush(ctx);
    }
    /* m->data (the RX-thread copy) is freed by message_processor_handle's
     * release_message_data once this returns: mode 0 hands ownership to the
     * queue. Freeing it here too was revision 3's double free (the glasses
     * reset ~10 s after every QUERY). */
}

/* ======================= RNLink thread: business 0xF0 ======================= */
static int cfw_ui_register(customCfwContext *ctx) {
    if (ctx->ui_module) return 1;
    int rc = fw_msg_register(UI_MODULE_FIRST, (void *)cfw_ui_handler, 0);
    if (rc == 0) ctx->ui_module = UI_MODULE_FIRST;
    else {
        rc = fw_msg_register(UI_MODULE_SECOND, (void *)cfw_ui_handler, 0);
        if (rc == 0) ctx->ui_module = UI_MODULE_SECOND;
    }
    ctx->ui_reg_rc = (int8_t)rc;
    return ctx->ui_module != 0;
}

static void cfw_rx_data(uint32_t bus, const uint8_t *data, uint32_t len) {
    if (bus != BIZ_GLASSLY || !data || len < 2 || len > RAW_MAX_BYTES) return;
    customCfwContext *ctx = cfw_ctx();
    if ((data[0] & PKT_CONTROL) && len >= 3 && data[2] == CTRL_QUERY) {
        /* Answer here, independent of the context and the LVGL hop. */
        cfw_ident_direct(ctx);
        cfw_diag_direct(ctx);
    }
    if (!ctx || !cfw_ui_register(ctx)) return;
    uint8_t *copy = (uint8_t *)fw_memalign(4, len);
    if (!copy) return;
    memcpy(copy, data, len);
    TIONativeMessage msg;
    cfw_zero(&msg, (uint32_t)sizeof(msg));
    msg.id = TIO_MSG_ID;
    msg.data = copy;
    msg.bytes = len;
    msg.mode = 0;
    int rc = fw_msg_send(ctx->ui_module, &msg);
    ctx->ui_send_rc = (int8_t)rc;
    if (rc != 0) fw_free(copy);   /* not queued: still ours */
}

/* Any thread: have the UI thread run the page tick as soon as it can. One
 * wake in flight at a time; the queue frees the one-byte payload (mode 0). */
void cfw_post_wake(customCfwContext *ctx) {
    if (!ctx->ui_module || ctx->wake_posted) return;
    uint8_t *tag = (uint8_t *)fw_memalign(4, 4);
    if (!tag) return;
    tag[0] = 1;
    TIONativeMessage msg;
    cfw_zero(&msg, (uint32_t)sizeof(msg));
    msg.id = TIO_WAKE_ID;
    msg.data = tag;
    msg.bytes = 1;
    msg.mode = 0;
    ctx->wake_posted = 1;
    if (fw_msg_send(ctx->ui_module, &msg) != 0) { ctx->wake_posted = 0; fw_free(tag); }
}

/* Registered by the boot shim; RNLink copies the 32 bytes. Only slot 0 is used.
 * Its address identifies the module's registry entry (ctx.c). */
const cfw_rn_callbacks cfw_rn_cbs = { { (void *)cfw_rx_data, 0, 0, 0, 0, 0, 0, 0 } };

__attribute__((used)) void cfw_boot_register(void) {
    if (cfw_rn_entry()) return;   /* registerLinkCallback ran again: keep the one entry */
    fw_rnlink_register(&cfw_rn_cbs, 0, BIZ_GLASSLY);
}

/* ======================= launcher init shim =================================
 * Replaces the 10-byte inline sequence in LinkMonitor::registerLinkCallback
 * that clears the launcher table's slot 0x18 and stores its stock callback:
 * registers the 0xF0 table first, then reproduces the displaced instructions
 * (r0 and r4 are live afterwards). Byte-verified by build.py. */
__attribute__((naked, used)) void cfw_register_shim(void) {
    __asm volatile(
        "push {r4, lr}\n"
        "bl cfw_boot_register\n"
        "pop {r4, lr}\n"
        "movs r4, #0\n"
        "ldr r0, =0x19750d2c\n"
        "ldr r3, =0x106d97f1\n"
        "str r4, [r0, #0x18]\n"
        "str r3, [r0]\n"
        "bx lr\n"
        ".ltorg\n"
    );
}
