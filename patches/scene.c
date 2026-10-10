/* Retained object cache (modes RESET/PUT/SHOW/HIDE) and the per-lens L8 render.
 *
 * Ported from the G2 Glassly CFW object cache. Revision 5 keeps one scene per
 * lens (cfw_lens, G2 parity: the phone sends each lens its own SHOW once any
 * element has depth; while flat, one SHOW targets both). Assets (images,
 * fonts, strings) live in the shared store, objects and the active set are
 * per lens. Each lens renders into its own 540x220 L8 canvas with 16 gray
 * levels; disp.c converts the canvases to the LCDC's 4-bit eye buffers on the
 * display thread. Text is drawn from the builtin bitmap font (font_builtin.h,
 * TEXT(15) and TEXT_INLINE(17)) or a cached font asset (TEXT_CACHED(16)); no
 * LVGL object is involved anywhere here.
 */
#include "cfw.h"
#include "rle.h"
#include "font_builtin.h"

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
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
cfw_object *object_find(cfw_lens *L, uint32_t id) {
    for (unsigned i = 0; i < CACHE_OBJECTS; i++)
        if (L->objects[i].id == id && id) return &L->objects[i];
    return 0;
}
static cfw_object *object_slot(cfw_lens *L) {
    for (unsigned i = 0; i < CACHE_OBJECTS; i++)
        if (!L->objects[i].id) return &L->objects[i];
    return 0;
}

static void lens_reset(cfw_lens *L) {
    L->settle_pending = 0;
    L->last_show_valid = 0;
    L->bg = 0;
    for (unsigned i = 0; i < CACHE_OBJECTS; i++) cfw_zero(&L->objects[i], sizeof(L->objects[i]));
    L->active_count = 0;
}

/* RESET: the shared store and both lens scenes (a RESET targets a lens, but
 * the store is one; the phone resets both lenses together). */
void cfw_scene_reset(customCfwContext *ctx, uint8_t cap) {
    (void)cap;
    cfw_anim_stop(ctx);
    for (unsigned i = 0; i < CACHE_ASSETS; i++) cfw_zero(&ctx->assets[i], sizeof(ctx->assets[i]));
    lens_reset(&ctx->lens[0]);
    lens_reset(&ctx->lens[1]);
    ctx->store_used = 0;
    ctx->epoch_revision++;
}

/* ---- shape record -> object ------------------------------------------------ */
static int shape_into(customCfwContext *ctx, cfw_object *o, const uint8_t *rec, uint32_t len) {
    if (len < 4) return 0;
    uint8_t type = rec[0];
    o->type = type;
    o->options = rec[2];
    o->color = rec[2];              /* low nibble is the gray level */
    o->text_off = 0;
    o->text_len = 0;
    o->asset_id = o->font_id = 0;
    o->width = rec[3];
    for (unsigned i = 0; i < 8; i++) o->p[i] = 0;
    if (type >= SH_LINE && type <= SH_PIE) {
        if (len < 20) return 0;
        for (unsigned i = 0; i < 8; i++) o->p[i] = rds16(rec + 4 + 2 * i);
        o->x = o->p[0]; o->y = o->p[1]; o->w = o->p[2]; o->h = o->p[3]; o->r = o->p[4];
    } else if (type == SH_IMAGE || type == SH_TEXT) {
        if (len < 12) return 0;
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->asset_id = rd32(rec + 8);
        o->p[0] = o->x; o->p[1] = o->y;
    } else if (type == SH_TEXT_CACHED) {
        if (len < 16) return 0;
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->asset_id = rd32(rec + 8);
        o->font_id = rd32(rec + 12);
        o->p[0] = o->x; o->p[1] = o->y;
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
        o->p[0] = o->x; o->p[1] = o->y; o->p[2] = o->w; o->p[3] = o->h;
    } else {
        return 0;               /* PATH (18) and the rest: vector.c, later */
    }
    return 1;
}

/* ---- PUT entry ------------------------------------------------------------- */
/* Byte length of one PUT entry (asset or object) at e, or 0 when it cannot be
 * sized within `avail`. Geometry records (types 1..13) are 4 + 8 i16 = 20 B
 * (G2CfwScene.Slot pads params to 8); IMAGE/TEXT 12, TEXT_CACHED 16,
 * TEXT_INLINE 13 + len, PATH 12 + count. */
static uint32_t put_entry_len(const uint8_t *e, uint32_t avail) {
    if (avail < 1) return 0;
    if (e[0] == PUT_ASSET) {
        if (avail < 20) return 0;
        return 20u + rd16(e + 18);
    }
    if (e[0] != PUT_OBJECT || avail < 10) return 0;
    uint8_t type = e[9];
    uint32_t rlen;
    if (type >= 1 && type <= 13) rlen = 20;
    else if (type == SH_IMAGE || type == SH_TEXT) rlen = 12;
    else if (type == SH_TEXT_CACHED) rlen = 16;
    else if (type == SH_TEXT_INLINE) { if (avail < 9 + 13) return 0; rlen = 13u + e[9 + 12]; }
    else if (type == 18) { if (avail < 9 + 12) return 0; rlen = 12u + rd16(e + 9 + 10); }
    else return 0;
    return 9u + rlen;
}

static int put_entry(customCfwContext *ctx, cfw_lens *L, const uint8_t *e, uint32_t len) {
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
        cfw_object *o = object_find(L, id);
        if (!o) o = object_slot(L);
        if (!o) return ST_CAPACITY;
        uint32_t pid = o->id ? o->id : id;
        uint8_t was_active = o->active;
        cfw_zero(o, sizeof(*o));
        o->id = pid; o->version = ver;
        o->active = was_active;
        o->flags = OBJ_VISIBLE;
        if (!shape_into(ctx, o, e + 9, len - 9)) { o->id = 0; return ST_REFUSED; }
        return ST_APPLIED;
    }
    return ST_REFUSED;
}

/* ---- message dispatch ------------------------------------------------------ */
/* One lens's view of a message. `first` is set for the first targeted lens of
 * a record, so shared state (store, epoch) is touched once per record. */
int cfw_scene_message(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint32_t len,
                      uint8_t *status, uint16_t *request, int first) {
    if (len < 5) return 0;
    uint8_t mode = msg[0];
    uint16_t epoch = rd16(msg + 1);
    *request = rd16(msg + 3);
    const uint8_t *body = msg + 5;
    uint32_t blen = len - 5;
    *status = ST_APPLIED;

    if (mode == MSG_RESET) {
        uint8_t cap = blen >= 2 ? body[1] : 0;
        if (first) {
            ctx->stream.epoch = epoch;
            cfw_scene_reset(ctx, cap);
            cfw_scene_hide(ctx, &ctx->lens[0]);
            cfw_scene_hide(ctx, &ctx->lens[1]);
        }
        return 1;
    }
    if (epoch != ctx->stream.epoch) { *status = ST_STALE; return 1; }

    /* a retried SHOW/HIDE never runs its motion twice */
    if ((mode == MSG_SHOW || mode == MSG_HIDE) && L->last_show_valid && L->last_show_request == *request) return 1;

    if (mode == MSG_PUT) {
        /* one entry per PUT message on RayNeo (<= 8192 B); an asset PUT applied
         * for the second lens finds the asset already stored and is a no-op */
        if (blen) { uint8_t r = (uint8_t)put_entry(ctx, L, body, blen); if (r != ST_APPLIED) *status = r; }
        return 1;
    }
    if (mode == MSG_HIDE) {
        uint8_t flags = blen >= 1 ? body[0] : 0;
        cfw_anim_freeze_all(L);
        for (uint32_t i = 0; i < L->active_count; i++) L->objects[L->active_list[i]].active = 0;
        L->active_count = 0;
        cfw_scene_hide(ctx, L);
        L->last_show_request = *request;
        L->last_show_valid = 1;
        if (flags & SHOW_FLAG_TAG) { cfw_anim_settled(ctx, L); L->settle_tag = *request; L->settle_pending = 1; }
        cfw_anim_settled(ctx, L);
        cfw_anim_arm(ctx);
        return 1;
    }
    if (mode == MSG_SHOW) {
        if (blen < 3) { *status = ST_REFUSED; return 1; }
        uint8_t flags = body[0], bg = (uint8_t)(body[1] & 0x0f), puts = body[2];
        uint32_t off = 3;
        /* inline PUT entries carried in the SHOW, back to back with no length
         * prefix (G2CfwScene.showPayload); each entry's size follows from its
         * kind and record type */
        for (uint8_t i = 0; i < puts && off < blen; i++) {
            uint32_t elen = put_entry_len(body + off, blen - off);
            if (!elen || off + elen > blen) { *status = ST_REFUSED; return 1; }
            uint8_t r = (uint8_t)put_entry(ctx, L, body + off, elen);
            if (r != ST_APPLIED) { *status = r; return 1; }
            off += elen;
        }
        if (off + 2 > blen) { *status = ST_REFUSED; return 1; }
        uint16_t count = rd16(body + off); off += 2;
        if (count > CACHE_OBJECTS) { *status = ST_REFUSED; return 1; }
        if ((flags & SHOW_FLAG_KEEP) && count) { *status = ST_REFUSED; return 1; }
        /* resolve the references first; a missing one leaves the scene unchanged */
        uint16_t next[CACHE_OBJECTS];
        for (uint16_t i = 0; i < count; i++) {
            if (off + 8 > blen) { *status = ST_REFUSED; return 1; }
            uint32_t id = rd32(body + off);
            uint32_t ver = rd32(body + off + 4);
            off += 8;
            cfw_object *o = object_find(L, id);
            if (!o || o->version != ver) { *status = ST_MISSING; return 1; }
            next[i] = (uint16_t)(o - L->objects);
        }
        /* ops: syntax, then targets against the incoming active set */
        uint32_t ops = off;
        for (uint32_t pos = ops; pos < blen;) {
            uint32_t n = cfw_op_len(body + pos, blen - pos);
            if (!n) { *status = ST_REFUSED; return 1; }
            pos += n;
        }
        if (!(flags & SHOW_FLAG_KEEP)) {
            for (uint32_t i = 0; i < L->active_count; i++) L->objects[L->active_list[i]].active = 0;
            for (uint16_t i = 0; i < count; i++) L->objects[next[i]].active = 1;
        }
        for (uint32_t pos = ops; pos < blen; pos += cfw_op_len(body + pos, blen - pos)) {
            uint8_t reason;
            if (cfw_op_check(L, body + pos, &reason) != ST_APPLIED) {
                /* undo the active marks: the displayed scene is unchanged */
                if (!(flags & SHOW_FLAG_KEEP)) {
                    for (uint16_t i = 0; i < count; i++) L->objects[next[i]].active = 0;
                    for (uint32_t i = 0; i < L->active_count; i++) L->objects[L->active_list[i]].active = 1;
                }
                *status = ST_REFUSED;
                return 1;
            }
        }
        if (!(flags & SHOW_FLAG_KEEP)) {
            /* departing objects freeze; the new list is in paint order */
            for (uint32_t i = 0; i < L->active_count; i++) {
                cfw_object *o = &L->objects[L->active_list[i]];
                if (!o->active) o->frames = 0, o->frame = 0;
            }
            for (uint16_t i = 0; i < count; i++) L->active_list[i] = next[i];
            L->active_count = count;
        }
        if (flags & SHOW_FLAG_FREEZE) cfw_anim_freeze_all(L);
        for (uint32_t pos = ops; pos < blen; pos += cfw_op_len(body + pos, blen - pos)) cfw_op_apply(L, body + pos);
        L->bg = bg;
        L->last_show_request = *request;
        L->last_show_valid = 1;
        if (flags & SHOW_FLAG_TAG) {
            cfw_anim_settled(ctx, L);       /* a superseded tag reports now */
            L->settle_tag = *request;
            L->settle_pending = 1;
        }
        cfw_scene_render(ctx, L);
        cfw_anim_arm(ctx);
        return 1;
    }
    return 0;
}

/* ---- rendering into the lens canvas (ctx->canvas_buf) ---------------------- */
static void px_gray(customCfwContext *ctx, int x, int y, uint8_t v) {
    if (x < 0 || y < 0 || x >= CFW_W || y >= CFW_H || !ctx->canvas_buf) return;
    ctx->canvas_buf[(uint32_t)y * CFW_W + (uint32_t)x] = v;
}

/* Blit raw 4bpp rows (rowbytes per row, low nibble = left pixel), scaled by
 * `gray` (0..15): a nibble of 0 is transparent, the rest write gray*nibble/15. */
static void blit_rows(customCfwContext *ctx, const uint8_t *rows, uint32_t iw, uint32_t ih, uint32_t rowbytes,
                      int x, int y, uint8_t gray) {
    for (uint32_t j = 0; j < ih; j++) {
        for (uint32_t i = 0; i < iw; i++) {
            uint8_t b = rows[j * rowbytes + (i >> 1)];
            uint8_t g = (i & 1) ? (b >> 4) : (b & 0x0f);
            if (!g) continue;
            uint32_t v = ((uint32_t)g * gray + 7u) / 15u;
            px_gray(ctx, x + (int)i, y + (int)j, (uint8_t)(v * 17u));
        }
    }
}

/* Blit one cached image `[w u8][h u8][4bpp RLE]` (len bytes) at (x, y):
 * decoded into a scratch region at the store tail (not persisted). Returns
 * the image width, 0 when it did not decode. */
static uint32_t blit_image(customCfwContext *ctx, const uint8_t *img, uint32_t len, int x, int y, uint8_t gray) {
    if (len < 2) return 0;
    uint32_t iw = img[0], ih = img[1];
    if (!iw || !ih) return 0;
    uint32_t rowbytes = (iw + 1u) / 2u;
    uint32_t need = rowbytes * ih;
    uint32_t scratch = (ctx->store_used + 15u) & ~15u;
    if (scratch + need > CFW_STORE_BYTES) return 0;
    uint8_t *dst = ctx->store + scratch;
    cfw_zero(dst, need);
    rle_state rs;
    rle_init(&rs, dst, rowbytes, rowbytes, ih);
    rle_feed(&rs, img + 2, len - 2);
    if (rs.err) return 0;
    blit_rows(ctx, dst, iw, ih, rowbytes, x, y, gray);
    return iw;
}

static void draw_image(customCfwContext *ctx, cfw_object *o) {
    cfw_asset *a = asset_find(ctx, o->asset_id);
    if (!a || !(a->state & 1) || a->length < 2) return;
    blit_image(ctx, ctx->store + a->offset, a->length, o->p[0], o->p[1], 15);
}

/* A font table `[96 x u32 offsets][glyph images]` (G2 texture_cache.h): draw
 * `n` bytes of `str` glyph by glyph. Bytes 1..31 are kerning (x += byte - 11);
 * 32..127 index the table. Cached-font glyphs are RLE images, the builtin
 * font's are raw rows (font_builtin.h). 1 when drawn, 0 on a bad table. */
#define FONT_TABLE_CHARS 96u
#define FONT_TABLE_BYTES (FONT_TABLE_CHARS * 4u)
static int draw_font_text(customCfwContext *ctx, const uint8_t *font, uint32_t flen, int raw,
                          const uint8_t *str, uint32_t n, int x, int y, uint8_t gray) {
    if (flen < FONT_TABLE_BYTES) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ch = str[i];
        if (ch == 0) break;
        if (ch <= 31u) { x += (int)ch - 11; continue; }
        if (ch > 127u) ch = '?';
        uint32_t off = rd32(font + (ch - 32u) * 4u);
        if (off == 0 || off + 2u > flen) return 0;
        uint32_t w = font[off], h = font[off + 1];
        if (raw) {
            uint32_t rowbytes = (w + 1u) / 2u;
            if (off + 2u + rowbytes * h > flen) return 0;
            blit_rows(ctx, font + off + 2, w, h, rowbytes, x, y, gray);
        } else {
            w = blit_image(ctx, font + off, flen - off, x, y, gray);
            if (!w) return 0;
        }
        x += (int)w;
    }
    return 1;
}

static void draw_text(customCfwContext *ctx, cfw_object *o) {
    uint8_t gray = (uint8_t)(o->color & 0x0f);
    if (!gray) gray = 15;
    if (o->type == SH_TEXT_INLINE) {
        if (o->text_len)
            draw_font_text(ctx, font_builtin, FONT_BUILTIN_BYTES, 1, ctx->store + o->text_off, o->text_len, o->p[0], o->p[1], gray);
        return;
    }
    cfw_asset *s = asset_find(ctx, o->asset_id);
    if (!s || !(s->state & 1)) return;
    if (o->type == SH_TEXT_CACHED && o->font_id) {
        cfw_asset *f = asset_find(ctx, o->font_id);
        if (f && (f->state & 1) &&
            draw_font_text(ctx, ctx->store + f->offset, f->length, 0, ctx->store + s->offset, s->length, o->p[0], o->p[1], gray))
            return;
    }
    draw_font_text(ctx, font_builtin, FONT_BUILTIN_BYTES, 1, ctx->store + s->offset, s->length, o->p[0], o->p[1], gray);
}

void cfw_scene_hide(customCfwContext *ctx, cfw_lens *L) {
    if (L->canvas) cfw_zero(L->canvas, (uint32_t)CFW_W * CFW_H);
    cfw_page_present(ctx);
}

void cfw_scene_render(customCfwContext *ctx, cfw_lens *L) {
    if (!L->canvas) return;
    ctx->canvas_buf = L->canvas;
    memset(L->canvas, (uint8_t)(L->bg * 17u), (uint32_t)CFW_W * CFW_H);
    for (uint16_t k = 0; k < L->active_count; k++) {
        cfw_object *o = &L->objects[L->active_list[k]];
        if (!(o->flags & OBJ_VISIBLE)) continue;
        uint8_t gray = (uint8_t)(o->color & 0x0f);
        switch (o->type) {
        case SH_LINE: case SH_RECT: case SH_RECT_FILL: case SH_CIRCLE: case SH_CIRCLE_FILL:
        case SH_TRI: case SH_TRI_FILL: case SH_QUAD: case SH_QUAD_FILL:
        case SH_BEZIER2: case SH_BEZIER3: case SH_ARC: case SH_PIE:
            ctx->draw_gray = (uint8_t)(gray * 17u);
            cfw_geom_draw(ctx, o->type, o->width, o->p);
            break;
        case SH_IMAGE:
            draw_image(ctx, o);
            break;
        case SH_TEXT:
        case SH_TEXT_INLINE:
        case SH_TEXT_CACHED:
            draw_text(ctx, o);
            break;
        default: break;
        }
    }
    ctx->canvas_buf = 0;
    cfw_page_present(ctx);
}
