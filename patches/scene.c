/* Retained object cache (modes RESET/PUT/SHOW/HIDE) and the 540x180 L8 render.
 *
 * Ported from the G2 Glassly CFW object cache, reduced to the RayNeo v1 subset:
 * images, strings, cached fonts, RECT/RECT_FILL and inline/cached text, no
 * animation, tween, vectors or SVG. The phone sends (id, version) references;
 * objects persist across SHOW/HIDE until a RESET or eviction. The single-bit
 * panel thresholds 4bpp gray >= 8 to on, written as L8 255. Inline/cached text
 * is drawn with positioned lv_labels (the firmware's default LVGL font); the
 * ResourcePool 20 px font overload ABI could not be verified here, so it is not
 * used (see docs/HOOKS.md).
 */
#include "cfw.h"
#include "rle.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }

/* ---- store allocator (bump; the whole store is reset on RESET) ------------- */
static uint32_t store_alloc(customCfwContext *ctx, uint32_t n) {
    uint32_t a = (ctx->store_used + 15u) & ~15u;
    if (a + n > CFW_STORE_BYTES) return 0xffffffffu;
    ctx->store_used = a + n;
    return a;
}

static cfw_asset *asset_find(customCfwContext *ctx, uint32_t id) {
    for (unsigned i = 0; i < CACHE_ASSETS; i++)
        if (ctx->assets[i].id == id && id) return &ctx->assets[i];
    return 0;
}
static cfw_asset *asset_slot(customCfwContext *ctx) {
    for (unsigned i = 0; i < CACHE_ASSETS; i++)
        if (!ctx->assets[i].id) return &ctx->assets[i];
    return 0;
}
static cfw_object *object_find(customCfwContext *ctx, uint32_t id) {
    for (unsigned i = 0; i < CACHE_OBJECTS; i++)
        if (ctx->objects[i].id == id && id) return &ctx->objects[i];
    return 0;
}
static cfw_object *object_slot(customCfwContext *ctx) {
    for (unsigned i = 0; i < CACHE_OBJECTS; i++)
        if (!ctx->objects[i].id) return &ctx->objects[i];
    return 0;
}

void cfw_scene_reset(customCfwContext *ctx, uint8_t cap) {
    (void)cap;
    for (unsigned i = 0; i < CACHE_ASSETS; i++) cfw_zero(&ctx->assets[i], sizeof(ctx->assets[i]));
    for (unsigned i = 0; i < CACHE_OBJECTS; i++) cfw_zero(&ctx->objects[i], sizeof(ctx->objects[i]));
    ctx->active_count = 0;
    ctx->store_used = 0;
    ctx->epoch_revision++;
}

/* ---- shape record -> object ------------------------------------------------ */
static int shape_into(customCfwContext *ctx, cfw_object *o, const uint8_t *rec, uint32_t len) {
    if (len < 4) return 0;
    uint8_t type = rec[0];
    o->type = type;
    o->options = rec[2];
    o->color = rec[2];              /* geometry: low nibble is gray; carried in options */
    o->text_off = 0;
    o->text_len = 0;
    o->asset_id = o->font_id = 0;
    if (type == SH_RECT || type == SH_RECT_FILL) {
        if (len < 20) return 0;
        o->color = rec[2];
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->w = rds16(rec + 8); o->h = rds16(rec + 10); o->r = rds16(rec + 12);
    } else if (type == SH_IMAGE) {
        if (len < 12) return 0;
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->asset_id = rd32(rec + 8);
    } else if (type == SH_TEXT_CACHED) {
        if (len < 16) return 0;
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->asset_id = rd32(rec + 8);
        o->font_id = rd32(rec + 12);
    } else if (type == SH_TEXT_INLINE) {
        if (len < 13) return 0;
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->w = rds16(rec + 8); o->h = rds16(rec + 10);
        uint8_t slen = rec[12];
        if ((uint32_t)13 + slen > len) return 0;
        uint32_t off = store_alloc(ctx, slen + 1u);
        if (off == 0xffffffffu) return 0;
        memcpy(ctx->store + off, rec + 13, slen);
        ctx->store[off + slen] = 0;
        o->text_off = off; o->text_len = slen;
    } else {
        return 0;               /* unsupported record type for v1 */
    }
    return 1;
}

/* ---- PUT entry ------------------------------------------------------------- */
static int put_entry(customCfwContext *ctx, const uint8_t *e, uint32_t len) {
    if (len < 1) return ST_REFUSED;
    uint8_t kind = e[0];
    if (kind == PUT_ASSET) {
        if (len < 20) return ST_REFUSED;
        uint32_t id = rd32(e + 1), ver = rd32(e + 5);
        uint8_t akind = e[9];
        uint32_t total = rd32(e + 10), offset = rd32(e + 14);
        uint16_t dl = rd16(e + 18);
        if ((uint32_t)20 + dl > len || offset + dl > total || total > CFW_STORE_BYTES) return ST_REFUSED;
        cfw_asset *a = asset_find(ctx, id);
        if (!a || a->version != ver) {
            a = asset_find(ctx, id);
            if (a && a->version != ver) cfw_zero(a, sizeof(*a));  /* superseded */
            a = asset_slot(ctx);
            if (!a) return ST_CAPACITY;
            uint32_t base = store_alloc(ctx, total);
            if (base == 0xffffffffu) return ST_CAPACITY;
            a->id = id; a->version = ver; a->kind = akind;
            a->length = total; a->offset = base; a->filled = 0; a->state = 0;
        }
        memcpy(ctx->store + a->offset + offset, e + 20, dl);
        if (offset + dl > a->filled) a->filled = offset + dl;
        if (a->filled >= a->length) a->state |= 1;   /* complete */
        return ST_APPLIED;
    }
    if (kind == PUT_OBJECT) {
        if (len < 9) return ST_REFUSED;
        uint32_t id = rd32(e + 1), ver = rd32(e + 5);
        cfw_object *o = object_find(ctx, id);
        if (!o) o = object_slot(ctx);
        if (!o) return ST_CAPACITY;
        uint32_t pid = o->id ? o->id : id;
        cfw_zero(o, sizeof(*o));
        o->id = pid; o->version = ver;
        if (!shape_into(ctx, o, e + 9, len - 9)) { o->id = 0; return ST_REFUSED; }
        return ST_APPLIED;
    }
    return ST_REFUSED;
}

/* ---- message dispatch ------------------------------------------------------ */
int cfw_scene_message(customCfwContext *ctx, const uint8_t *msg, uint32_t len,
                      uint8_t *status, uint16_t *request) {
    if (len < 5) return 0;
    uint8_t mode = msg[0];
    uint16_t epoch = rd16(msg + 1);
    *request = rd16(msg + 3);
    const uint8_t *body = msg + 5;
    uint32_t blen = len - 5;
    *status = ST_APPLIED;

    if (mode == MSG_RESET) {
        uint8_t cap = blen >= 2 ? body[1] : 0;
        ctx->stream.epoch = epoch;
        cfw_scene_reset(ctx, cap);
        return 1;
    }
    if (epoch != ctx->stream.epoch) { *status = ST_STALE; return 1; }

    if (mode == MSG_PUT) {
        uint32_t off = 0;
        while (off < blen) {
            uint32_t remain = blen - off;
            uint8_t r = (uint8_t)put_entry(ctx, body + off, remain);
            if (r != ST_APPLIED) { *status = r; return 1; }
            /* one entry per PUT message on RayNeo (<= 8192 B); stop after it */
            break;
        }
        return 1;
    }
    if (mode == MSG_HIDE) {
        ctx->active_count = 0;
        cfw_scene_hide(ctx);
        return 1;
    }
    if (mode == MSG_SHOW) {
        if (blen < 3) { *status = ST_REFUSED; return 1; }
        uint8_t puts = body[2];
        uint32_t off = 3;
        /* inline PUT entries carried in the SHOW */
        for (uint8_t i = 0; i < puts && off < blen; i++) {
            /* each inline entry is a full PUT entry preceded by a u16 length */
            if (off + 2 > blen) { *status = ST_REFUSED; return 1; }
            uint16_t elen = rd16(body + off); off += 2;
            if (off + elen > blen) { *status = ST_REFUSED; return 1; }
            uint8_t r = (uint8_t)put_entry(ctx, body + off, elen);
            if (r != ST_APPLIED) { *status = r; return 1; }
            off += elen;
        }
        if (off + 2 > blen) { *status = ST_REFUSED; return 1; }
        uint16_t count = rd16(body + off); off += 2;
        if (count > CACHE_OBJECTS) { *status = ST_REFUSED; return 1; }
        ctx->active_count = 0;
        for (uint16_t i = 0; i < count; i++) {
            if (off + 8 > blen) { *status = ST_REFUSED; return 1; }
            uint32_t id = rd32(body + off);
            uint32_t ver = rd32(body + off + 4);
            off += 8;
            cfw_object *o = object_find(ctx, id);
            if (!o || o->version != ver) { *status = ST_MISSING; return 1; }
            ctx->active_list[ctx->active_count++] = (uint16_t)(o - ctx->objects);
        }
        cfw_scene_render(ctx);
        return 1;
    }
    return 0;
}

/* ---- rendering into the L8 canvas ----------------------------------------- */
static void px_on(customCfwContext *ctx, int x, int y) {
    if (x < 0 || y < 0 || x >= CFW_W || y >= CFW_H || !ctx->canvas_buf) return;
    ctx->canvas_buf[(uint32_t)y * CFW_W + (uint32_t)x] = CFW_PANEL_ON;
}
static void fill_rect(customCfwContext *ctx, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px_on(ctx, x + i, y + j);
}
static void stroke_rect(customCfwContext *ctx, int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) { px_on(ctx, x + i, y); px_on(ctx, x + i, y + h - 1); }
    for (int j = 0; j < h; j++) { px_on(ctx, x, y + j); px_on(ctx, x + w - 1, y + j); }
}

static void draw_image(customCfwContext *ctx, cfw_object *o) {
    cfw_asset *a = asset_find(ctx, o->asset_id);
    if (!a || !(a->state & 1) || a->length < 2) return;
    const uint8_t *img = ctx->store + a->offset;
    uint32_t iw = img[0], ih = img[1];
    if (!iw || !ih || iw > 255 || ih > 255) return;
    uint32_t rowbytes = (iw + 1u) / 2u;
    uint32_t need = rowbytes * ih;
    /* decode 4bpp RLE into a scratch region at the store tail (not persisted) */
    uint32_t scratch = (ctx->store_used + 15u) & ~15u;
    if (scratch + need > CFW_STORE_BYTES) return;
    uint8_t *dst = ctx->store + scratch;
    cfw_zero(dst, need);
    rle_state rs;
    rle_init(&rs, dst, rowbytes, rowbytes, ih);
    rle_feed(&rs, img + 2, a->length - 2);
    if (rs.err) return;
    for (uint32_t j = 0; j < ih; j++) {
        for (uint32_t i = 0; i < iw; i++) {
            uint8_t b = dst[j * rowbytes + (i >> 1)];
            uint8_t g = (i & 1) ? (b >> 4) : (b & 0x0f);
            if (g >= CFW_GRAY_ON) px_on(ctx, o->x + (int)i, o->y + (int)j);
        }
    }
}

/* Inline/cached text: one lv_label per active text object, positioned absolutely.
 * Labels are recreated each render (bounded by CACHE_LABELS). */
static void labels_clear(customCfwContext *ctx) {
    for (unsigned i = 0; i < ctx->label_count; i++)
        if (ctx->labels[i]) { fw_obj_delete(ctx->labels[i]); ctx->labels[i] = 0; }
    ctx->label_count = 0;
}

static void draw_text(customCfwContext *ctx, cfw_object *o) {
    if (ctx->label_count >= CACHE_LABELS || !ctx->page_root) return;
    const char *s = 0;
    char tmp[CACHE_LABELS ? 129 : 129];
    if (o->type == SH_TEXT_INLINE && o->text_len) {
        s = (const char *)(ctx->store + o->text_off);
    } else if (o->type == SH_TEXT_CACHED) {
        cfw_asset *a = asset_find(ctx, o->asset_id);
        if (!a || !(a->state & 1)) return;
        uint32_t n = a->length < 128 ? a->length : 128;
        memcpy(tmp, ctx->store + a->offset, n);
        tmp[n] = 0;
        s = tmp;
    }
    if (!s) return;
    void *label = fw_label_create(ctx->page_root);
    if (!label) return;
    fw_label_set_text(label, s);
    fw_obj_set_pos(label, o->x, o->y);
    ctx->labels[ctx->label_count++] = label;
}

void cfw_scene_hide(customCfwContext *ctx) {
    if (ctx->canvas_buf) cfw_zero(ctx->canvas_buf, (uint32_t)CFW_W * CFW_H);
    labels_clear(ctx);
    if (ctx->canvas) fw_obj_invalidate(ctx->canvas);
}

void cfw_scene_render(customCfwContext *ctx) {
    if (!ctx->canvas_buf) return;
    cfw_zero(ctx->canvas_buf, (uint32_t)CFW_W * CFW_H);
    labels_clear(ctx);
    for (uint16_t k = 0; k < ctx->active_count; k++) {
        cfw_object *o = &ctx->objects[ctx->active_list[k]];
        uint8_t gray = (uint8_t)(o->color & 0x0f);
        switch (o->type) {
        case SH_RECT_FILL:
            if (gray >= CFW_GRAY_ON) fill_rect(ctx, o->x, o->y, o->w, o->h);
            break;
        case SH_RECT:
            if (gray >= CFW_GRAY_ON) stroke_rect(ctx, o->x, o->y, o->w, o->h);
            break;
        case SH_IMAGE:
            draw_image(ctx, o);
            break;
        case SH_TEXT_INLINE:
        case SH_TEXT_CACHED:
            draw_text(ctx, o);
            break;
        default: break;
        }
    }
    if (ctx->canvas) fw_obj_invalidate(ctx->canvas);
}
