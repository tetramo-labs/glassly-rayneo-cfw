/* The direct-framebuffer family (G2 image-handler modes 3, 6, 8, 9, 11, 15,
 * 19, 20 and 36; g2flash zlib_glue.c image_dispatch), drawn straight into
 * the lens canvases the display thread composes from (disp.c). Wire formats
 * are the G2's with the canvas at 540x220, so the phone's G2 encoders apply
 * with the IDENT size: mode-3 boxes are [left/4][top/2][w/4][h/2] bytes, RLE
 * runs are pixel nibbles top-down, left to right, crossing rows, decoded
 * here as L8 runs. These messages carry no epoch/request header and get an
 * ACK or a NACK only; a bundle (8) also relays the REPLY of its last cache
 * segment. A raster message takes the canvas over from the retained scene
 * (cfw_scene_takeover): the next SHOW/HIDE takes it back. The page lease
 * stands in for the G2's framebuffer lease: a closed page NACKs everything
 * (link.c) and closing zeroes the canvases, so the phone resends a keyframe
 * after reopening. A malformed message is NACKed; pixels it decoded before
 * failing stay on the canvas, as on the G2, so the phone resends a keyframe
 * after any NACK. */
#include "cfw.h"

int cfw_raster_mode(uint8_t mode) {
    uint8_t m = mode & 0x7fu;
    if (mode & MSG_LENSES_DIFFER) return m == MSG_RASTER_BOX || m == MSG_COPY;
    return m == MSG_RASTER_BOX || m == MSG_RASTER_FULL || m == MSG_BUNDLE || m == MSG_COPY || m == MSG_CLEANUP ||
           m == MSG_TEXT_BUILTIN || m == MSG_DRAW_IMAGE || m == MSG_DRAW_STRING || m == MSG_SHAPES;
}

/* ---- RLE -> L8 box -------------------------------------------------------- */
typedef struct { uint8_t *canvas; int left, top, w, h, row, col; uint32_t left_px; } rs_box;

static int rs_emit(rs_box *b, uint8_t v, uint32_t n) {
    if (n > b->left_px) return 0;
    b->left_px -= n;
    uint8_t g = (uint8_t)(v * 17u);
    while (n) {
        uint32_t run = (uint32_t)(b->w - b->col);
        if (run > n) run = n;
        memset(b->canvas + (uint32_t)(b->top + b->row) * CFW_W + (uint32_t)(b->left + b->col), g, run);
        b->col += (int)run; n -= run;
        if (b->col >= b->w) { b->col = 0; b->row++; }
    }
    return 1;
}

/* Decode `n` bytes of RLE into the box; 1 when it covered the box exactly. */
static int rs_decode(uint8_t *canvas, int left, int top, int w, int h, const uint8_t *p, uint32_t n) {
    rs_box b = { canvas, left, top, w, h, 0, 0, (uint32_t)w * (uint32_t)h };
    uint32_t i = 0;
    while (i < n) {
        uint8_t t = p[i++];
        uint8_t color = (uint8_t)(t & 0x0fu);
        uint32_t cnt = t >> 4;
        if (!cnt) {
            if (i >= n) return 0;
            cnt = p[i++];
            if (!cnt) {
                if (i + 2 > n) return 0;
                cnt = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8);
                i += 2;
                if (!cnt) return 0;
            }
        }
        if (!rs_emit(&b, color, cnt)) return 0;
    }
    return b.left_px == 0;
}

static int box_ok(int l, int t, int w, int h) {
    return w > 0 && h > 0 && l >= 0 && t >= 0 && l + w <= CFW_W && t + h <= CFW_H;
}

/* ---- one message for one lens ---------------------------------------------- */
/* `present` is set at the top level: a bundle applies its segments with it
 * clear and presents once. Returns 1 applied, 0 refused (NACK). */
static int raster_apply(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint32_t len, int first, int present,
                        uint8_t *reply_mode, uint8_t *status, uint16_t *request, uint8_t *extra, uint8_t *extra_len) {
    if (!len) return 0;
    uint8_t mode = (uint8_t)(msg[0] & 0x7fu);
    int stereo = (msg[0] & MSG_LENSES_DIFFER) != 0;
    int right = L->bit == LENS_RIGHT;
    if (mode != MSG_BUNDLE && mode != MSG_CLEANUP && !L->canvas) return 0;

    switch (mode) {
    case MSG_RASTER_FULL: {
        if (len < 3) return 0;
        cfw_scene_takeover(ctx, L);
        if (!rs_decode(L->canvas, 0, 0, CFW_W, CFW_H, msg + 1, len - 1)) return 0;
        break;
    }
    case MSG_RASTER_BOX: {
        /* [3][l/4][t/2][w/4][h/2][fid u16][rle] or [0x83][L box][R box][fid][rle]
         * (the boxes share a size); the fid is diagnostic on the G2, ignored */
        uint32_t hdr = stereo ? 11u : 7u;
        if (len < hdr + 1u) return 0;
        if (stereo && (msg[3] != msg[7] || msg[4] != msg[8])) return 0;
        const uint8_t *b = msg + (stereo && right ? 5 : 1);
        int l = b[0] * 4, t = b[1] * 2, w = b[2] * 4, h = b[3] * 2;
        if (!box_ok(l, t, w, h)) return 0;
        cfw_scene_takeover(ctx, L);
        if (!rs_decode(L->canvas, l, t, w, h, msg + hdr, len - hdr)) return 0;
        break;
    }
    case MSG_COPY: {
        /* [9][sL][sT][sW][sH][dL][dT][dW][dH] u16 each; 0x89 carries a set per lens */
        if (len != (stereo ? 33u : 17u)) return 0;
        const uint8_t *q = msg + 1 + (stereo && right ? 16 : 0);
        int sl = rd16(q), st = rd16(q + 2), sw = rd16(q + 4), sh = rd16(q + 6);
        int dl = rd16(q + 8), dt = rd16(q + 10), dw = rd16(q + 12), dh = rd16(q + 14);
        if (sw != dw || sh != dh || !box_ok(sl, st, sw, sh) || !box_ok(dl, dt, dw, dh)) return 0;
        cfw_scene_takeover(ctx, L);
        if (dt <= st) {
            for (int r = 0; r < sh; r++)
                memmove(L->canvas + (uint32_t)(dt + r) * CFW_W + (uint32_t)dl, L->canvas + (uint32_t)(st + r) * CFW_W + (uint32_t)sl, (uint32_t)sw);
        } else {
            for (int r = sh - 1; r >= 0; r--)
                memmove(L->canvas + (uint32_t)(dt + r) * CFW_W + (uint32_t)dl, L->canvas + (uint32_t)(st + r) * CFW_W + (uint32_t)sl, (uint32_t)sw);
        }
        break;
    }
    case MSG_TEXT_BUILTIN: {
        /* [15][x i16][y i16][opt][len][bytes]: len must cover the rest exactly */
        if (len < 7 || msg[6] != len - 7u) return 0;
        for (uint32_t i = 7; i < len; i++) if (!msg[i]) return 0;
        cfw_scene_takeover(ctx, L);
        if (!cfw_scene_draw_string(ctx, L, 0, (int16_t)rd16(msg + 1), (int16_t)rd16(msg + 3), msg[5], msg + 7, len - 7)) return 0;
        break;
    }
    case MSG_DRAW_IMAGE: {
        if (len != 10) return 0;
        cfw_scene_takeover(ctx, L);
        if (!cfw_scene_draw_image_asset(ctx, L, rd32(msg + 1), (int16_t)rd16(msg + 5), (int16_t)rd16(msg + 7), msg[9])) return 0;
        break;
    }
    case MSG_DRAW_STRING: {
        if (len < 11 || len != 11u + msg[10]) return 0;
        for (uint32_t i = 11; i < len; i++) if (!msg[i] || msg[i] > 127) return 0;
        cfw_scene_takeover(ctx, L);
        if (!cfw_scene_draw_string(ctx, L, rd32(msg + 1), (int16_t)rd16(msg + 5), (int16_t)rd16(msg + 7), msg[9], msg + 11, msg[10])) return 0;
        break;
    }
    case MSG_SHAPES: {
        /* [36][count][records]: every record sized and its assets resolved
         * before any pixel is drawn; the list must end with the message */
        if (len < 2) return 0;
        uint32_t count = msg[1], pos = 2;
        for (uint32_t i = 0; i < count; i++) {
            int rlen = cfw_scene_record_len(msg + pos, len - pos);
            if (!rlen || !cfw_scene_record_check(ctx, msg + pos, (uint32_t)rlen)) return 0;
            pos += (uint32_t)rlen;
        }
        if (pos != len) return 0;
        cfw_scene_takeover(ctx, L);
        pos = 2;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t rlen = (uint32_t)cfw_scene_record_len(msg + pos, len - pos);
            cfw_scene_record_draw(ctx, L, msg + pos, rlen);
            pos += rlen;
        }
        break;
    }
    case MSG_CLEANUP: {
        /* release the raster and blank this lens; the first lens also drops
         * the object cache under a new epoch, so the phone's next cache
         * message is STALE and it starts over with a RESET (G2 mode 11 frees
         * the cache and the shadow) */
        if (first) {
            cfw_scene_reset(ctx, 0);
            ctx->stream.epoch ^= 0x8000u;
            ctx->cache_ready = 0;
        }
        L->raster = 0;
        cfw_scene_hide(ctx, L);
        return 1;
    }
    case MSG_BUNDLE: {
        /* [8][count]{[len u16][segment]}: top level only; segments are the
         * raster modes and the cache messages; applied in order, presented
         * once; a failing segment NACKs the bundle (earlier ones stay applied) */
        if (!present || len < 2) return 0;
        uint32_t count = msg[1], pos = 2;
        ctx->disp_hold = 1;
        int ok = 1;
        for (uint32_t i = 0; ok && i < count; i++) {
            if (pos + 2 > len) { ok = 0; break; }
            uint32_t seglen = rd16(msg + pos);
            pos += 2;
            if (!seglen || pos + seglen > len) { ok = 0; break; }
            const uint8_t *seg = msg + pos;
            uint8_t sm = (uint8_t)(seg[0] & 0x7fu);
            if (cfw_raster_mode(seg[0]) && sm != MSG_BUNDLE && sm != MSG_CLEANUP) {
                ok = raster_apply(ctx, L, seg, seglen, first, 0, reply_mode, status, request, extra, extra_len);
            } else if (sm >= MSG_PUT && sm <= MSG_RESET && seglen >= 5) {
                ok = cfw_scene_message(ctx, L, seg, seglen, status, request, first, extra, extra_len);
                if (ok) *reply_mode = seg[0];
            } else ok = 0;
            pos += seglen;
        }
        ctx->disp_hold = 0;
        cfw_page_present(ctx);
        return ok;
    }
    default:
        return 0;
    }
    if (present) cfw_page_present(ctx);
    return 1;
}

int cfw_raster_message(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint32_t len, int first,
                       uint8_t *reply_mode, uint8_t *status, uint16_t *request, uint8_t *extra, uint8_t *extra_len) {
    *reply_mode = 0;
    *status = ST_APPLIED;
    *request = 0;
    *extra_len = 0;
    return raster_apply(ctx, L, msg, len, first, 1, reply_mode, status, request, extra, extra_len);
}
