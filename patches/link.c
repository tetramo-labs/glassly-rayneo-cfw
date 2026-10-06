/* Business 0xF0 link: the downlink record channel, the control channel, and the
 * uplink replies.
 *
 * TRANSPORT DECISION (see docs/HOOKS.md and docs/PROTOCOL.md): this build
 * environment has no ARM disassembler, so a generic RNLink business demux could
 * not be verified for a safe entry hook. v1 therefore uses the spec §7 fallback:
 * the DOWNLINK is the named-file channel — the launcher's file-receive callback
 * (byte-verified hook) delivers a completed file "glassly-cfw.gcf" whose bytes
 * are length-delimited stream blocks [opt u8][len u16 LE][chunk]. The UPLINK uses
 * rnlink_if_send_payload(0xF0, ...). The phone spec (§3) uses raw 0xF0 frames in
 * both directions; the one deviation a phone must know is the downlink framing.
 *
 * The RX callback only validates, deep-copies and enqueues to the launcher UI
 * thread (message_processor_send). All record parsing, LVGL mutation and uplink
 * run on the UI thread, one reply at a time, >= 100 ms apart, <= 192 bytes.
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

/* ---- uplink queue (drained one-at-a-time, >=100 ms apart, <=192 B) --------- */
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
    if (ctx->uplink_head == ctx->uplink_tail) return;
    uint32_t now = fw_tick();
    if (ctx->last_uplink_tick && (now - ctx->last_uplink_tick) < UPLINK_MIN_GAP_MS) return;
    cfw_uplink *u = &ctx->uplink[ctx->uplink_head];
    if (fw_send(BIZ_GLASSLY, u->data, u->len, 0) == 0) {
        ctx->uplink_head = (uint8_t)((ctx->uplink_head + 1) % UPLINK_QUEUE);
        ctx->last_uplink_tick = now;
    }
}

/* ---- reply builders -------------------------------------------------------- */

static void send_ack(customCfwContext *ctx, uint8_t kind, uint16_t size, uint16_t crc) {
    uint8_t b[9];
    b[0] = kind;                       /* ACK or NACK */
    b[1] = ctx->stream.stream_id;
    cfw_put16(b + 2, ctx->stream.ordinal);
    b[4] = REC_LENS;
    cfw_put16(b + 5, size);
    cfw_put16(b + 7, crc);
    cfw_uplink_push(ctx, b, sizeof(b));
}

static void send_reply(customCfwContext *ctx, uint8_t mode, uint16_t request, uint8_t status, uint16_t epoch) {
    uint8_t b[15];
    b[0] = RPL_REPLY;
    b[1] = ctx->stream.stream_id;
    cfw_put16(b + 2, ctx->stream.ordinal);
    b[4] = REC_LENS;
    b[5] = mode;
    cfw_put16(b + 6, request);
    b[8] = status;
    cfw_put16(b + 9, epoch);
    cfw_put32(b + 11, ctx->epoch_revision);
    cfw_uplink_push(ctx, b, sizeof(b));
}

/* ---- record handler -------------------------------------------------------- */
static void cfw_record(customCfwContext *ctx, const uint8_t *wire, uint32_t total) {
    uint8_t flags = wire[0];
    uint16_t len = (uint16_t)(wire[1] | (wire[2] << 8));
    uint16_t crc = (uint16_t)(wire[3] | (wire[4] << 8));
    const uint8_t *body = wire + 5;
    (void)total;

    if (!(flags & REC_LENS)) { ctx->last_nack = CFW_NACK_FLAGS; ctx->nack_count++; send_ack(ctx, RPL_NACK, 0, crc); ctx->stream.ordinal++; return; }
    if (flags & REC_RESET_CTX) inflate_reset(ctx);

    uint8_t *decoded = 0;
    const uint8_t *data;
    uint32_t dlen;
    if (flags & REC_COMPRESSED) {
        decoded = (uint8_t *)fw_memalign(8, CFW_MESSAGE_CAP);
        if (!decoded || !inflate_record(ctx, body, len, decoded, &dlen)) {
            if (decoded) fw_free(decoded);
            ctx->last_nack = CFW_NACK_INFLATE; ctx->nack_count++;
            send_ack(ctx, RPL_NACK, 0, crc); ctx->stream.ordinal++; return;
        }
        data = decoded;
    } else {
        data = body; dlen = len;
    }

    if (crc16_ccitt(data, dlen) != crc) {
        if (decoded) fw_free(decoded);
        ctx->last_nack = CFW_NACK_CRC; ctx->nack_count++;
        send_ack(ctx, RPL_NACK, (uint16_t)dlen, crc); ctx->stream.ordinal++; return;
    }

    /* cache messages require the page to be open (spec §3.2) */
    if (!ctx->page_open) {
        if (decoded) fw_free(decoded);
        send_ack(ctx, RPL_NACK, (uint16_t)dlen, crc); ctx->stream.ordinal++; return;
    }

    uint8_t status = ST_APPLIED;
    uint16_t request = 0;
    int handled = dlen >= 5 ? cfw_scene_message(ctx, data, dlen, &status, &request) : 0;
    if (handled) {
        send_reply(ctx, data[0], request, status, (uint16_t)(data[1] | (data[2] << 8)));
        send_ack(ctx, RPL_ACK, (uint16_t)dlen, crc);
    } else {
        send_ack(ctx, RPL_NACK, (uint16_t)dlen, crc);
    }
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
void cfw_control(customCfwContext *ctx, uint8_t ctrl) {
    uint8_t reason;
    switch (ctrl) {
    case CTRL_QUERY:
        ctx->queried = 1;
        cfw_send_ident(ctx);
        break;
    case CTRL_PAGE_OPEN:
        if (cfw_page_open(ctx, &reason)) cfw_send_page_state(ctx, 1, PAGE_OK);
        else cfw_send_page_state(ctx, 0, reason);
        break;
    case CTRL_PAGE_CLOSE:
        cfw_page_close(ctx, 0);
        break;
    case CTRL_KEEPALIVE:
        if (ctx->page_open) {
            ctx->lease_deadline = fw_tick() + PAGE_LEASE_MS;
            cfw_send_page_state(ctx, 1, PAGE_OK);
        }
        break;
    default: break;
    }
}

/* ---- one stream block (opt, chunk) ----------------------------------------- */
static void cfw_packet(customCfwContext *ctx, uint8_t opt, const uint8_t *chunk, uint32_t len) {
    if (opt & PKT_CONTROL) {
        if (len >= 1) cfw_control(ctx, chunk[0]);
        return;
    }
    if (opt & PKT_RESET) { ctx->stream.used = 0; ctx->stream.in_stream = 1; ctx->stream.ordinal = 0; }
    if (len) stream_feed(ctx, chunk, len);
    if (opt & PKT_END) { ctx->stream.used = 0; ctx->stream.in_stream = 0; }
}

/* ======================= RX thread: file-receive ============================ */
static int name_is_ours(const char *n) {
    static const char want[] = "glassly-cfw.gcf";
    for (unsigned i = 0; i < sizeof(want); i++) if (n[i] != want[i]) return 0;
    return 1;
}

/* UI-thread handler for the deep-copied downlink. ABI (message_processor_send /
 * register_c_handler) is UNVERIFIED on this build: the handler is assumed to
 * receive the TIONativeMessage pointer. See docs/HOOKS.md. */
static void cfw_ui_handler(void *arg) {
    const TIONativeMessage *m = (const TIONativeMessage *)arg;
    if (!m || m->id != TIO_MSG_ID || !m->data) return;
    customCfwContext *ctx = cfw_ctx_peek();
    if (ctx) {
        const uint8_t *d = m->data;
        uint32_t n = m->bytes, off = 0;
        while (off + 3u <= n) {
            uint8_t opt = d[off];
            uint16_t blen = (uint16_t)(d[off + 1] | (d[off + 2] << 8));
            off += 3;
            if (off + blen > n) break;
            cfw_packet(ctx, opt, d + off, blen);
            off += blen;
        }
    }
    fw_free((void *)m->data);   /* release the RX-thread deep copy */
}

void cfw_file_rx(const TIONativeFile *f) {
    if (!f || !f->complete || !name_is_ours(f->filename)) return;
    customCfwContext *ctx = cfw_ctx();
    if (!ctx) return;
    if (!ctx->ui_registered) {
        fw_msg_register(UI_MODULE, (void *)cfw_ui_handler, 0);
        ctx->ui_registered = 1;
    }
    uint32_t n = f->declared;
    if (!n || n > (CFW_REC_BUF + 1024u)) return;
    uint8_t *copy = (uint8_t *)fw_memalign(4, n);
    if (!copy) return;
    memcpy(copy, f->data, n);
    TIONativeMessage msg;
    cfw_zero(&msg, (uint32_t)sizeof(msg));
    msg.id = TIO_MSG_ID;
    msg.data = copy;
    msg.bytes = n;
    msg.mode = 0;
    if (fw_msg_send(UI_MODULE, &msg) != 0) fw_free(copy);
}

/* ======================= launcher file-sink shim ============================
 * Replaces the 10-byte inline sequence that left the launcher's extra
 * file-receive slot NULL; points it at cfw_file_rx while preserving the stock
 * callback at [0]. Naked so the displaced `movs r4,#0` (which following stock
 * code relies on) is reproduced exactly. Byte-verified by build.py. */
__attribute__((naked, used)) void cfw_register_shim(void) {
    __asm volatile(
        "movs r4, #0\n"
        "ldr r0, =0x19750d2c\n"
        "ldr r3, =cfw_file_rx\n"
        "str r3, [r0, #0x18]\n"
        "ldr r3, =0x106d97f1\n"
        "str r3, [r0]\n"
        "bx lr\n"
        ".ltorg\n"
    );
}
