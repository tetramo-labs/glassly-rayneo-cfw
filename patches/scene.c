/* Retained object cache (modes RESET/PUT/SHOW/HIDE) and the per-lens L8 render.
 *
 * Ported from the G2 Glassly CFW object cache. Revision 5 keeps one scene per
 * lens (cfw_lens, G2 parity: the phone sends each lens its own SHOW once any
 * element has depth; while flat, one SHOW targets both). Assets (images,
 * fonts, strings) live in the shared store, objects and the active set are
 * per lens. Each lens renders into its own 540x220 L8 canvas with 16 gray
 * levels; disp.c converts the canvases to the LCDC's 4-bit eye buffers on the
 * display thread. Text is drawn from the builtin bitmap font (font_builtin.h:
 * SF Pro 20 px, proportional, kerned, Latin-1; TEXT(15) and TEXT_INLINE(17))
 * or a cached font asset (TEXT_CACHED(16)); no LVGL object is involved
 * anywhere here.
 */
#include "cfw.h"
#include "rle.h"
#include "font_builtin.h"

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }

#include "cache_txn.c"

/* ---- store allocator (granule bitmap, first fit; assets and inline strings
 * are freed when evicted, as in the G2 scene.c) ----------------------------- */
static uint32_t store_granules(uint32_t len) { return (len + STORE_GRANULE - 1u) / STORE_GRANULE; }
static int store_bit(const customCfwContext *ctx, uint32_t g) { return (ctx->store_map[g >> 3] >> (g & 7u)) & 1u; }
static void store_mark(customCfwContext *ctx, uint32_t g0, uint32_t n, int set) {
    for (uint32_t g = g0; g < g0 + n; g++) {
        if (set) ctx->store_map[g >> 3] |= (uint8_t)(1u << (g & 7u));
        else ctx->store_map[g >> 3] &= (uint8_t)~(1u << (g & 7u));
    }
}
static uint32_t store_alloc(customCfwContext *ctx, uint32_t n) {
    uint32_t need = store_granules(n ? n : 1u);
    uint32_t run = 0;
    for (uint32_t g = 0; g < ctx->store_granules; g++) {
        run = store_bit(ctx, g) ? 0 : run + 1;
        if (run == need) {
            uint32_t g0 = g + 1u - need;
            if (!txn_save_store(ctx, g0, need)) return 0xffffffffu;
            store_mark(ctx, g0, need, 1);
            ctx->store_used += need * STORE_GRANULE;
            return g0 * STORE_GRANULE;
        }
    }
    return 0xffffffffu;
}
static void store_free(customCfwContext *ctx, uint32_t offset, uint32_t len) {
    if (!len || offset >= ctx->store_bytes) return;
    uint32_t g0 = offset / STORE_GRANULE, n = store_granules(len);
    if (g0 + n > ctx->store_granules) n = ctx->store_granules - g0;
    store_mark(ctx, g0, n, 0);
    ctx->store_used -= n * STORE_GRANULE;
}
static void store_clear(customCfwContext *ctx) {
    cfw_zero(ctx->store_map, sizeof(ctx->store_map));
    ctx->store_used = 0;
}

/* Renderers keep the complete version while its replacement uploads. */
static cfw_asset *asset_find(customCfwContext *ctx, uint32_t id) {
    cfw_asset *pending = 0;
    for (unsigned i = 0; i < CACHE_ASSETS; i++) {
        cfw_asset *a = &ctx->assets[i];
        if (!id || a->id != id) continue;
        if (a->state & 1) return a;
        pending = a;
    }
    return pending;
}
static cfw_asset *asset_version(customCfwContext *ctx, uint32_t id, uint32_t version) {
    for (unsigned i = 0; i < CACHE_ASSETS; i++)
        if (ctx->assets[i].id == id && ctx->assets[i].version == version) return &ctx->assets[i];
    return 0;
}
cfw_object *object_find(cfw_lens *L, uint32_t id) {
    for (unsigned i = 0; i < CACHE_OBJECTS; i++)
        if (L->objects[i].id == id && id) return &L->objects[i];
    return 0;
}

/* ---- journal (G2 scene.c): one ring per lens of the definitions,
 * completions, evictions and drops the phone's mirror of that lens must
 * learn about. Assets are shared, so an asset event lands in both rings. ---- */
static void journal_note_lens(cfw_lens *L, uint8_t kind, uint32_t id, uint32_t version) {
    uint32_t slot = (L->journal_head + L->journal_count) % CACHE_JOURNAL;
    if (L->journal_count == CACHE_JOURNAL) {
        cfw_journal_entry *old = &L->journal[L->journal_head];
        if (old->revision > L->journal_lost_rev) L->journal_lost_rev = old->revision;
        L->journal_head = (uint8_t)((L->journal_head + 1u) % CACHE_JOURNAL);
        slot = (L->journal_head + L->journal_count - 1u) % CACHE_JOURNAL;
    } else {
        L->journal_count++;
    }
    cfw_journal_entry *e = &L->journal[slot];
    e->id = id;
    e->version = version | ((uint32_t)kind << 31);   /* bit 31 = asset; versions are < 2^31 */
    e->revision = L->revision + 1u;                   /* recorded under the revision being built */
    L->mutated = 1;
}
/* L = the lens of an object event; NULL = both lenses (an asset event). */
static void journal_note(customCfwContext *ctx, cfw_lens *L, uint8_t kind, uint32_t id, uint32_t version) {
    if (L) { journal_note_lens(L, kind, id, version); return; }
    journal_note_lens(&ctx->lens[0], kind, id, version);
    journal_note_lens(&ctx->lens[1], kind, id, version);
}
static cfw_lens *lens_of(customCfwContext *ctx, const cfw_object *o) {
    return (o >= ctx->lens[1].objects && o < ctx->lens[1].objects + CACHE_OBJECTS) ? &ctx->lens[1] : &ctx->lens[0];
}
/* After a message: every lens with notes moves to the next revision. */
static void journal_commit(customCfwContext *ctx) {
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        if (L->mutated) { L->revision++; L->mutated = 0; }
    }
}

/* ---- eviction (G2 scene.c): unreferenced assets first, then the least
 * recently used object that is neither active nor pinned ------------------- */
static void object_free(customCfwContext *ctx, cfw_object *o) {
    if (!txn_save_object(ctx, o)) return;
    if (o->id) journal_note(ctx, lens_of(ctx, o), JOURNAL_OBJECT, o->id, 0);
    if (o->text_len) store_free(ctx, o->text_off, (uint32_t)o->text_len + 1u);
    if (o->path_bytes) store_free(ctx, o->path_off, o->path_bytes);
    cfw_zero(o, sizeof(*o));
}
static void asset_free(customCfwContext *ctx, cfw_asset *a) {
    if (a->id && (a->state & 1)) journal_note(ctx, 0, JOURNAL_ASSET, a->id, 0);
    store_free(ctx, a->offset, a->length);
    cfw_zero(a, sizeof(*a));
}
static int asset_referenced(const customCfwContext *ctx, uint32_t id) {
    for (unsigned e = 0; e < 2; e++)
        for (unsigned i = 0; i < CACHE_OBJECTS; i++) {
            const cfw_object *o = &ctx->lens[e].objects[i];
            if (o->id && (o->asset_id == id || o->font_id == id)) return 1;
        }
    return 0;
}
static int evict_asset(customCfwContext *ctx) {
    int best = -1;
    for (unsigned i = 0; i < CACHE_ASSETS; i++) {
        const cfw_asset *a = &ctx->assets[i];
        if (!a->id || a->pinned || asset_referenced(ctx, a->id)) continue;
        if (best < 0 || a->last_use < ctx->assets[best].last_use) best = (int)i;
    }
    if (best < 0) return 0;
    asset_free(ctx, &ctx->assets[best]);
    return 1;
}
/* Evict the LRU evictable object of lens L, or of either lens when L is NULL. */
static int evict_object(customCfwContext *ctx, cfw_lens *L) {
    cfw_object *best = 0;
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *lens = &ctx->lens[e];
        if (L && lens != L) continue;
        for (unsigned i = 0; i < CACHE_OBJECTS; i++) {
            cfw_object *o = &lens->objects[i];
            if (!o->id || o->active || o->pinned) continue;
            if (!best || o->last_use < best->last_use) best = o;
        }
    }
    if (!best) return 0;
    object_free(ctx, best);
    if (ctx->transaction && ctx->transaction->failed) return 0;
    ctx->evicted++;
    return 1;
}
static cfw_object *object_slot(customCfwContext *ctx, cfw_lens *L) {
    for (;;) {
        for (unsigned i = 0; i < CACHE_OBJECTS; i++)
            if (!L->objects[i].id) return &L->objects[i];
        if (!evict_object(ctx, L)) return 0;
    }
}
static cfw_asset *asset_slot(customCfwContext *ctx) {
    for (;;) {
        cfw_asset *a = 0;
        for (unsigned i = 0; i < CACHE_ASSETS && !a; i++)
            if (!ctx->assets[i].id) a = &ctx->assets[i];
        if (a) return a;
        if (!evict_asset(ctx) && !evict_object(ctx, 0)) return 0;
    }
}
/* Store space, evicting unreferenced assets then LRU objects until it fits. */
static uint32_t store_reserve(customCfwContext *ctx, uint32_t n) {
    for (;;) {
        uint32_t off = store_alloc(ctx, n);
        if (off != 0xffffffffu) return off;
        if (ctx->transaction && ctx->transaction->failed) return 0xffffffffu;
        if (!evict_asset(ctx) && !evict_object(ctx, 0)) return 0xffffffffu;
    }
}

static void lens_reset(cfw_lens *L) {
    L->canvas_valid = 0;
    L->settle_pending = 0;
    L->last_show_valid = 0;
    L->bg = 0;
    for (unsigned i = 0; i < CACHE_OBJECTS; i++) cfw_zero(&L->objects[i], sizeof(L->objects[i]));
    L->active_count = 0;
    L->revision = 0;
    L->journal_lost_rev = 0;
    L->journal_head = L->journal_count = 0;
    L->mutated = 0;
}

/* RESET: the shared store and both lens scenes (a RESET targets a lens, but
 * the store is one; the phone resets both lenses together). */
void cfw_scene_reset(customCfwContext *ctx, uint8_t cap) {
    (void)cap;
    cfw_anim_stop(ctx);
    for (unsigned i = 0; i < CACHE_ASSETS; i++) cfw_zero(&ctx->assets[i], sizeof(ctx->assets[i]));
    lens_reset(&ctx->lens[0]);
    lens_reset(&ctx->lens[1]);
    store_clear(ctx);
    ctx->use_clock = 0;
    ctx->cache_ready = 1;
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
        uint32_t off = store_reserve(ctx, slen + 1u);
        if (off == 0xffffffffu) return -1;         /* store full even after eviction */
        memcpy(ctx->store + off, rec + 13, slen);
        ctx->store[off + slen] = 0;
        o->text_off = off; o->text_len = slen;
        o->p[0] = o->x; o->p[1] = o->y; o->p[2] = o->w; o->p[3] = o->h;
    } else if (type == SH_PATH) {
        /* [18][flags][gray][rule][x][y][scale Q8][len][commands]: compile the
         * commands into Q4 edges kept with the object in the store (vector.c) */
        if (len < 12) return 0;
        uint32_t clen = rd16(rec + 10);
        uint16_t scale = rd16(rec + 8);
        if (rec[3] > 1 || !clen || clen > PATH_MAX_BYTES || scale > 2048 || 12u + clen > len) return 0;
        cfw_vector_work *w = cfw_vec_work(ctx);
        if (!w) return -1;
        int n = cfw_vec_compile(w, rec + 12, clen);
        if (n < 0) { ctx->refuse_reason = REFUSE_ASSET; return 0; }
        uint32_t bytes = (uint32_t)sizeof(cfw_path) + (uint32_t)n * (uint32_t)sizeof(cfw_edge);
        uint32_t off = store_reserve(ctx, bytes);
        if (off == 0xffffffffu) return -1;
        cfw_path *path = (cfw_path *)(void *)(ctx->store + off);
        path->count = (uint16_t)n;
        path->rule = rec[3];
        path->pad = 0;
        for (int i = 0; i < n; i++) path->edges[i] = w->edges[i];
        o->path_off = off; o->path_bytes = (uint16_t)bytes;
        o->x = rds16(rec + 4); o->y = rds16(rec + 6);
        o->p[0] = o->x; o->p[1] = o->y; o->p[2] = (int16_t)scale; o->p[3] = 0;
    } else {
        return 0;
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

/* Scan one RLE image without allocating/decompressing it. A font contains
 * several such streams, so return the byte count through its last pixel. */
static uint32_t asset_image_bytes(const uint8_t *p, uint32_t n) {
    if (n < 3 || !p[0] || !p[1]) return 0;
    uint32_t left = (uint32_t)p[0] * p[1], pos = 2;
    while (left) {
        if (pos >= n) return 0;
        uint32_t run = p[pos++] >> 4;
        if (!run) {
            if (pos >= n) return 0;
            run = p[pos++];
            if (!run) { if (n - pos < 2) return 0; run = rd16(p + pos); pos += 2; }
        }
        if (!run || run > left) return 0;
        left -= run;
    }
    return pos;
}
static int asset_content_valid(const uint8_t *p, uint32_t n, uint8_t kind) {
    if (kind == ASSET_IMAGE) return asset_image_bytes(p, n) == n;
    if (kind == ASSET_FONT) {
        if (n < 96u * 4u) return 0;
        for (uint32_t i = 0; i < 96u; i++) {
            uint32_t off = rd32(p + i * 4u);
            if (off && (off < 384u || off >= n || !asset_image_bytes(p + off, n - off))) return 0;
        }
        return 1;
    }
    return kind == ASSET_STRING;
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
        if (!id || !ver || ver >= 0x80000000u || akind > ASSET_STRING ||
            (uint32_t)20 + dl != len || !total || !dl || total > ctx->store_bytes ||
            offset > total || dl > total - offset) return ST_REFUSED;
        cfw_asset *a = asset_version(ctx, id, ver);
        if (a) {
            if (a->length != total || a->kind != akind) return ST_REFUSED;
            /* A repeated chunk (the other lens or a retry) is a byte-identical
             * no-op. It must never rewrite a complete asset or skip a hole. */
            if (offset < a->filled) {
                if (dl > a->filled - offset || memcmp(ctx->store + a->offset + offset, e + 20, dl)) return ST_REFUSED;
                a->pinned = 1;
                return ST_APPLIED;
            }
            if (offset != a->filled || (a->state & 1)) return ST_REFUSED;
        } else {
            if (offset) return ST_REFUSED;
            /* Preserve the displayed version, including under store pressure.
             * An abandoned incomplete version may be replaced at offset zero. */
            for (unsigned i = 0; i < CACHE_ASSETS; i++) {
                cfw_asset *old = &ctx->assets[i];
                if (old->id != id) continue;
                if (old->kind != akind) return ST_REFUSED;
                if (old->state & 1) old->pinned = 1;
                else asset_free(ctx, old);
            }
            a = asset_slot(ctx);
            if (!a) return ST_CAPACITY;
            uint32_t base = store_reserve(ctx, total);
            if (base == 0xffffffffu) return ST_CAPACITY;
            a->id = id; a->version = ver; a->kind = akind;
            a->length = total; a->offset = base; a->filled = 0; a->state = 0;
        }
        a->pinned = 1;
        a->last_use = ++ctx->use_clock;
        memcpy(ctx->store + a->offset + offset, e + 20, dl);
        a->filled += dl;
        if (a->filled == a->length) {
            if (!asset_content_valid(ctx->store + a->offset, a->length, a->kind)) { ctx->refuse_reason = REFUSE_ASSET; return ST_REFUSED; }
            a->state |= 1;
            /* Publish only once the new version is whole. Objects resolve by
             * id, so both eyes switch together at this commit. */
            for (unsigned i = 0; i < CACHE_ASSETS; i++) {
                cfw_asset *old = &ctx->assets[i];
                if (old != a && old->id == id) asset_free(ctx, old);
            }
            journal_note(ctx, 0, JOURNAL_ASSET, a->id, a->version);
        }
        return ST_APPLIED;
    }
    if (kind == PUT_OBJECT) {
        if (len < 9 || put_entry_len(e, len) != len) return ST_REFUSED;
        uint32_t id = rd32(e + 1), ver = rd32(e + 5);
        if (!id || !ver || ver >= 0x80000000u) return ST_REFUSED;
        cfw_object *o = object_find(L, id);
        if (!o) o = object_slot(ctx, L);
        if (!o || !txn_save_object(ctx, o)) return ST_CAPACITY;
        uint32_t pid = o->id ? o->id : id;
        uint8_t was_active = o->active, was_pinned = o->pinned;
        if (o->text_len) store_free(ctx, o->text_off, (uint32_t)o->text_len + 1u);
        if (o->path_bytes) store_free(ctx, o->path_off, o->path_bytes);
        cfw_zero(o, sizeof(*o));              /* a new version also resets its rotation */
        o->id = pid; o->version = ver;
        o->active = was_active;
        o->pinned = 1;                        /* the store reserve below must not evict it */
        o->flags = OBJ_VISIBLE;
        o->last_use = ++ctx->use_clock;
        int shaped = shape_into(ctx, o, e + 9, len - 9);
        o->pinned = ctx->transaction ? 1 : was_pinned;
        if (shaped <= 0) {
            /* the previous version is gone with it: the mirror must learn that */
            if (o->id && o->version != ver) journal_note(ctx, L, JOURNAL_OBJECT, o->id, 0);
            else if (o->id) journal_note(ctx, L, JOURNAL_OBJECT, o->id, 0);
            o->id = 0;
            return shaped < 0 ? ST_CAPACITY : ST_REFUSED;
        }
        journal_note(ctx, L, JOURNAL_OBJECT, o->id, o->version);
        return ST_APPLIED;
    }
    return ST_REFUSED;
}

/* ---- STATE (mode 40) ------------------------------------------------------- */
/* [40][epoch][revision u32][page u16]: UNCHANGED when the phone's mirror is at
 * this lens's revision, DELTA with the journal entries after it when they fit
 * one reply, else SNAPSHOT page `page` of every resident object and complete
 * asset. Queries never touch recency. The reply's request word is the page. */
static void scene_state(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint8_t *status, uint8_t *extra, uint8_t *extra_len) {
    uint16_t epoch = rd16(msg + 1);
    uint32_t revision = rd32(msg + 3);
    uint16_t page = rd16(msg + 7);
    if (epoch != 0 && epoch != ctx->stream.epoch) { *status = ST_STALE; return; }
    if (!ctx->cache_ready) { *status = ST_RESET; return; }
    if (page == 0 && epoch != 0) {
        if (revision == L->revision) { *status = ST_UNCHANGED; return; }
        if (revision < L->revision && L->journal_lost_rev <= revision) {
            uint32_t n = 0;
            for (uint32_t i = 0; i < L->journal_count; i++)
                if (L->journal[(L->journal_head + i) % CACHE_JOURNAL].revision > revision) n++;
            if (1u + 9u * n <= REPLY_EXTRA_MAX) {
                extra[0] = (uint8_t)n;
                uint8_t *q = extra + 1;
                for (uint32_t i = 0; i < L->journal_count; i++) {
                    const cfw_journal_entry *e = &L->journal[(L->journal_head + i) % CACHE_JOURNAL];
                    if (e->revision <= revision) continue;
                    q[0] = (uint8_t)(e->version >> 31);
                    cfw_put32(q + 1, e->id);
                    cfw_put32(q + 5, e->version & 0x7fffffffu);
                    q += 9;
                }
                *extra_len = (uint8_t)(1u + 9u * n);
                *status = ST_DELTA;
                return;
            }
        }
    }
    /* snapshot page: this lens's objects by index, then complete assets */
    uint32_t per = (REPLY_EXTRA_MAX - 5u) / 13u;
    uint32_t total = 0, skip = (uint32_t)page * per, n = 0;
    uint8_t *q = extra + 5;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t limit = pass ? CACHE_ASSETS : CACHE_OBJECTS;
        for (uint32_t i = 0; i < limit; i++) {
            uint32_t id, version, last;
            if (pass == 0) {
                const cfw_object *o = &L->objects[i];
                if (o->id == 0) continue;
                id = o->id; version = o->version; last = o->last_use;
            } else {
                const cfw_asset *a = &ctx->assets[i];
                if (a->id == 0 || !(a->state & 1)) continue;
                id = a->id; version = a->version; last = a->last_use;
            }
            if (total >= skip && n < per) {
                q[0] = (uint8_t)pass;
                cfw_put32(q + 1, id); cfw_put32(q + 5, version); cfw_put32(q + 9, last);
                q += 13; n++;
            }
            total++;
        }
    }
    cfw_put16(extra, (uint16_t)total);
    cfw_put16(extra + 2, page);
    extra[4] = (uint8_t)n;
    *extra_len = (uint8_t)(5u + 13u * n);
    *status = ST_SNAPSHOT;
}

/* ---- message dispatch ------------------------------------------------------ */
static int scene_message_apply(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint32_t len,
                               uint8_t *status, uint16_t *request, int first, uint8_t *extra, uint8_t *extra_len);

/* One lens's view of a message. `first` is set for the first targeted lens of
 * a record, so shared state (store, epoch) is touched once per record. The
 * journal revisions advance once the message has been applied, so the
 * REPLY (link.c) carries the revision that includes it. */
int cfw_scene_message(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint32_t len,
                      uint8_t *status, uint16_t *request, int first, uint8_t *extra, uint8_t *extra_len) {
    ctx->refuse_reason = 0;
    if (len < 5) return 0;
    int show = msg[0] == MSG_SHOW;
    int duplicate = show && L->last_show_valid && L->last_show_request == rd16(msg + 3);
    int transactional = msg[0] == MSG_PUT || (show && len >= 8 && msg[7] && !duplicate);
    *extra_len = 0;
    *request = rd16(msg + 3);
    if (transactional && !txn_begin(ctx)) { *status = ST_CAPACITY; return 1; }
    int handled = scene_message_apply(ctx, L, msg, len, status, request, first, extra, extra_len);
    int applied = handled && *status == ST_APPLIED;
    if (ctx->transaction) {
        txn_end(ctx, applied);
        if (!applied && *status == ST_CAPACITY && *extra_len) extra[0] = 0; /* evictions rolled back */
    }
    if (applied && msg[0] != MSG_STATE && !duplicate) {
        L->scene_serial++;
        /* Shared asset changes invalidate the other eye's cached text/images. */
        for (unsigned e = 0; e < 2; e++) if (ctx->lens[e].mutated) ctx->lens[e].scene_serial++;
    }
    if (handled && *status == ST_REFUSED && *extra_len == 0 && ctx->refuse_reason) { extra[0] = ctx->refuse_reason; *extra_len = 1; }
    journal_commit(ctx);
    if (applied && show && !duplicate) {
        if (msg[5] & SHOW_FLAG_TAG) {
            cfw_anim_settled(ctx, L);
            L->settle_tag = *request; L->settle_pending = 1;
        }
        cfw_scene_render(ctx, L);
        cfw_anim_arm(ctx);
    }
    return handled;
}

static int scene_message_apply(customCfwContext *ctx, cfw_lens *L, const uint8_t *msg, uint32_t len,
                               uint8_t *status, uint16_t *request, int first, uint8_t *extra, uint8_t *extra_len) {
    if (len < 5) return 0;
    uint8_t mode = msg[0];
    uint16_t epoch = rd16(msg + 1);
    *request = rd16(msg + 3);
    const uint8_t *body = msg + 5;
    uint32_t blen = len - 5;
    *status = ST_APPLIED;
    *extra_len = 0;
    ctx->evicted = 0;

    if (mode == MSG_STATE) {
        if (len < 9) return 0;
        *request = rd16(msg + 7);                /* the page, echoed as the G2 does */
        scene_state(ctx, L, msg, status, extra, extra_len);
        return 1;
    }
    if (mode == MSG_RESET) {
        uint8_t sub = blen >= 1 ? body[0] : 0;
        if (sub == CTL_RESET) {
            uint8_t cap = blen >= 2 ? body[1] : 0;
            if (first) {
                ctx->stream.epoch = epoch;
                cfw_scene_reset(ctx, cap);
                cfw_scene_hide(ctx, &ctx->lens[0]);
                cfw_scene_hide(ctx, &ctx->lens[1]);
            }
            /* [evicted][store KiB u16]: the phone sizes its asset mirror from this */
            extra[0] = 0; cfw_put16(extra + 1, (uint16_t)(ctx->store_bytes / 1024u)); *extra_len = 3;
            return 1;
        }
        if (epoch != ctx->stream.epoch) { *status = ST_STALE; return 1; }
        if (sub == CTL_DROP) {
            if (blen < 2) return 0;
            uint32_t n = body[1];
            if (blen != 2u + 4u * n) return 0;
            for (uint32_t i = 0; i < n; i++) {
                cfw_object *o = object_find(L, rd32(body + 2 + 4 * i));
                if (o && o->active) { *status = ST_REFUSED; ctx->refuse_reason = REFUSE_ACTIVE; return 1; }
            }
            for (uint32_t i = 0; i < n; i++) {
                cfw_object *o = object_find(L, rd32(body + 2 + 4 * i));
                if (o) object_free(ctx, o);
            }
            return 1;
        }
        if (sub == CTL_PERIOD) {
            if (blen != 2 || body[1] < 10 || body[1] > 250) return 0;
            ctx->anim_period_ms = body[1];
            return 1;
        }
        if (sub == CTL_TUNING) {
            if (blen != 2) return 0;
            ctx->tuning = body[1];
            return 1;
        }
        return 0;
    }
    if (epoch != ctx->stream.epoch) { *status = ST_STALE; return 1; }

    /* a retried SHOW/HIDE never runs its motion twice */
    if ((mode == MSG_SHOW || mode == MSG_HIDE) && L->last_show_valid && L->last_show_request == *request) return 1;

    if (mode == MSG_PUT) {
        /* one entry per PUT message on RayNeo (<= 8192 B); an asset PUT applied
         * for the second lens finds the asset already stored and is a no-op */
        if (!blen) { *status = ST_REFUSED; return 1; }
        if (blen) { uint8_t r = (uint8_t)put_entry(ctx, L, body, blen); if (r != ST_APPLIED) *status = r; }
        if (*status == ST_APPLIED || *status == ST_CAPACITY) { extra[0] = (uint8_t)(ctx->evicted > 255u ? 255u : ctx->evicted); *extra_len = 1; }
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
         * kind and record type. Size them first to find the reference list,
         * and pin every referenced resident object so no inline put can evict
         * what this SHOW is about to display (G2 scene.c). */
        uint32_t puts_end = off;
        for (uint8_t i = 0; i < puts && puts_end < blen; i++) {
            uint32_t elen = put_entry_len(body + puts_end, blen - puts_end);
            if (!elen || puts_end + elen > blen) { *status = ST_REFUSED; return 1; }
            puts_end += elen;
        }
        if (puts_end + 2 > blen) { *status = ST_REFUSED; return 1; }
        uint16_t count = rd16(body + puts_end);
        if (count > CACHE_OBJECTS || puts_end + 2 + 8u * count > blen) { *status = ST_REFUSED; return 1; }
        if ((flags & SHOW_FLAG_KEEP) && count) { *status = ST_REFUSED; return 1; }
        for (uint16_t i = 0; puts && i < count; i++) {
            cfw_object *o = object_find(L, rd32(body + puts_end + 2 + 8u * i));
            if (o) {
                if (!txn_save_object(ctx, o)) { *status = ST_CAPACITY; return 1; }
                o->pinned = 1;
            }
        }
        uint8_t put_status = ST_APPLIED;
        for (uint8_t i = 0; i < puts && off < puts_end; i++) {
            uint32_t elen = put_entry_len(body + off, blen - off);
            uint8_t r = (uint8_t)put_entry(ctx, L, body + off, elen);
            if (r != ST_APPLIED) { put_status = r; break; }
            off += elen;
        }
        if (put_status != ST_APPLIED) {
            *status = put_status;
            if (put_status == ST_CAPACITY) { extra[0] = (uint8_t)(ctx->evicted > 255u ? 255u : ctx->evicted); *extra_len = 1; }
            return 1;
        }
        off = puts_end + 2;
        /* resolve the references; missing ones are listed and the scene is unchanged */
        uint16_t next[CACHE_OBJECTS];
        uint32_t missing = 0;
        extra[0] = 0; extra[1] = 0;
        for (uint16_t i = 0; i < count; i++) {
            uint32_t id = rd32(body + off);
            uint32_t ver = rd32(body + off + 4);
            off += 8;
            cfw_object *o = object_find(L, id);
            if (!o || o->version != ver) {
                if (2u + 4u * (missing + 1u) <= REPLY_EXTRA_MAX) { cfw_put32(extra + 2 + 4 * missing, id); missing++; }
                else extra[1] = 1;              /* more than fit */
                continue;
            }
            next[i] = (uint16_t)(o - L->objects);
        }
        if (missing) {
            extra[0] = (uint8_t)missing;
            *extra_len = (uint8_t)(2 + 4 * missing);
            *status = ST_MISSING;
            return 1;
        }
        /* the paths the scene would show share one edge budget per lens */
        uint32_t edges = 0;
        for (uint16_t i = 0; i < count; i++) {
            const cfw_object *o = &L->objects[next[i]];
            if (o->type == SH_PATH && o->path_bytes) edges += ((const cfw_path *)(const void *)(ctx->store + o->path_off))->count;
        }
        if (flags & SHOW_FLAG_KEEP)
            for (uint32_t i = 0; i < L->active_count; i++) {
                const cfw_object *o = &L->objects[L->active_list[i]];
                if (o->type == SH_PATH && o->path_bytes) edges += ((const cfw_path *)(const void *)(ctx->store + o->path_off))->count;
            }
        if (edges > PATH_SCENE_EDGES) { *status = ST_REFUSED; ctx->refuse_reason = REFUSE_EDGES; return 1; }
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
            uint8_t reason = 0;
            uint8_t r = cfw_op_check(ctx, L, body + pos, &reason);
            if (r != ST_APPLIED) {
                /* undo the active marks: the displayed scene is unchanged */
                if (!(flags & SHOW_FLAG_KEEP)) {
                    for (uint16_t i = 0; i < count; i++) L->objects[next[i]].active = 0;
                    for (uint32_t i = 0; i < L->active_count; i++) L->objects[L->active_list[i]].active = 1;
                }
                *status = r;
                ctx->refuse_reason = reason;
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
            /* shown objects and their assets are the most recently used */
            uint32_t now = ++ctx->use_clock;
            for (uint16_t i = 0; i < count; i++) {
                cfw_object *o = &L->objects[next[i]];
                o->last_use = now;
                cfw_asset *a;
                if (o->asset_id && (a = asset_find(ctx, o->asset_id))) a->last_use = now;
                if (o->font_id && (a = asset_find(ctx, o->font_id))) a->last_use = now;
            }
        }
        if (flags & SHOW_FLAG_FREEZE) cfw_anim_freeze_all(L);
        for (uint32_t pos = ops; pos < blen; pos += cfw_op_len(body + pos, blen - pos)) cfw_op_apply(L, body + pos);
        L->bg = bg;
        L->last_show_request = *request;
        L->last_show_valid = 1;
        extra[0] = (uint8_t)(ctx->evicted > 255u ? 255u : ctx->evicted); *extra_len = 1;
        return 1;
    }
    return 0;
}

/* ---- rendering into the lens canvas (ctx->canvas_buf) ---------------------- */
/* Texture draw options (the G2 texture_cache.c): the record's color byte is
 * [inverse 0x20][transparent 0x10][ink 0..15]. The 4-bit LUT scales the
 * 0..15 source range into 0..ink (15 = identity), inverse reverses the
 * completed ramp; transparency tests the source value before the LUT, so
 * source 0 is skipped even for an inverse ramp. A color tween on an image or
 * text therefore fades it. */
#define TEX_OPT_TRANSPARENT 0x10u
#define TEX_OPT_INVERSE     0x20u
static void make_lut(uint8_t options, uint8_t *lut) {
    uint32_t top = options & 0x0fu;
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t source = (options & TEX_OPT_INVERSE) ? 15u - i : i;
        lut[i] = (uint8_t)((source * top) / 15u);
    }
}

/* Blit a 4bpp pixel stream of iw*ih pixels, rows unpadded, through the LUT.
 * `hi_first`: the first pixel of each byte is the high nibble (the RLE
 * decoder's layout, rle.c); the builtin font's raw rows are low nibble first
 * (gen_font.py; even glyph widths keep rows byte-aligned). Until revision 7 this
 * read every stream low nibble first and padded rows: cached images drew with
 * each pixel pair swapped and odd widths drifted a pixel per row. */
static void blit_rows(customCfwContext *ctx, const uint8_t *px, uint32_t iw, uint32_t ih, int hi_first,
                      int x, int y, const uint8_t *lut, int transparent) {
    if (!ctx->canvas_buf || !iw || !ih) return;
    /* clip once: rows [j0, j1) and columns [i0, i1) of the image land on the canvas */
    int j0 = y < 0 ? -y : 0, j1 = (int)ih;
    if (y + j1 > CFW_H) j1 = CFW_H - y;
    int i0 = x < 0 ? -x : 0, i1 = (int)iw;
    if (x + i1 > CFW_W) i1 = CFW_W - x;
    if (j0 >= j1 || i0 >= i1) return;
    uint8_t ink[16];
    for (uint32_t g = 0; g < 16; g++) ink[g] = (uint8_t)(lut[g] * 17u);
    for (int j = j0; j < j1; j++) {
        uint8_t *dst = ctx->canvas_buf + (uint32_t)(y + j) * CFW_W + (uint32_t)(x + i0);
        uint32_t n = (uint32_t)j * iw + (uint32_t)i0;
        int i = i0;
        if (n & 1u) {                               /* odd start: one pixel, then byte-aligned pairs */
            uint8_t g = hi_first ? (px[n >> 1] & 0x0f) : (px[n >> 1] >> 4);
            if (!transparent || g) *dst = ink[g];
            i++; n++; dst++;
        }
        const uint8_t *src = px + (n >> 1);
        for (; i + 1 < i1; i += 2, dst += 2, src++) {   /* two pixels per byte; a blank byte of a glyph costs one test */
            uint8_t b = *src;
            if (transparent && !b) continue;
            uint8_t g0 = hi_first ? (b >> 4) : (b & 0x0f), g1 = hi_first ? (b & 0x0f) : (b >> 4);
            if (!transparent || g0) dst[0] = ink[g0];
            if (!transparent || g1) dst[1] = ink[g1];
        }
        if (i < i1) {
            uint8_t g = hi_first ? (*src >> 4) : (*src & 0x0f);
            if (!transparent || g) *dst = ink[g];
        }
    }
}

/* Blit one cached image `[w u8][h u8][4bpp RLE]` (len bytes) at (x, y): the
 * RLE covers exactly w*h pixels with no row padding (G2CfwTextureCache), so it
 * is decoded as one row into a scratch region at the store tail (not
 * persisted). Returns the image width, 0 when it did not decode. */
static uint32_t blit_image(customCfwContext *ctx, const uint8_t *img, uint32_t len, int x, int y, const uint8_t *lut, int transparent) {
    if (len < 2) return 0;
    uint32_t iw = img[0], ih = img[1];
    if (!iw || !ih) return 0;
    uint32_t need = (iw * ih + 1u) / 2u;
    uint32_t scratch = store_alloc(ctx, need);       /* transient decode buffer */
    if (scratch == 0xffffffffu) return 0;
    uint8_t *dst = ctx->store + scratch;
    cfw_zero(dst, need);
    rle_state rs;
    rle_init(&rs, dst, need, need, 1);
    rle_feed(&rs, img + 2, len - 2);
    uint32_t out = 0;
    if (!rs.err) { blit_rows(ctx, dst, iw, ih, 1, x, y, lut, transparent); out = iw; }
    store_free(ctx, scratch, need);
    return out;
}

static void draw_image(customCfwContext *ctx, cfw_object *o) {
    cfw_asset *a = asset_find(ctx, o->asset_id);
    if (!a || !(a->state & 1) || a->length < 2) return;
    uint8_t lut[16];
    make_lut(o->color, lut);
    blit_image(ctx, ctx->store + a->offset, a->length, o->p[0], o->p[1], lut, (o->color & TEX_OPT_TRANSPARENT) != 0);
}

/* Draw `n` bytes of `str` glyph by glyph with a font table. A cached font
 * (G2 texture_cache.h) is `[96 x u32 offsets][RLE glyph images]` for ASCII
 * 32..127, x advancing by the image width. The builtin font (font_builtin.h,
 * `raw`) is `[224 x u32 offsets][kerning pairs][glyph [w][h][adv][raw rows]]`
 * for Latin-1 0x20..0xFF: the string's UTF-8 two-byte sequences decode to
 * Latin-1, any other non-ASCII character draws '?', x advances by the glyph's
 * advance plus the face's pair kerning, as the G2's stock font does. Bytes
 * 1..31 are explicit kerning (x += byte - 11). 1 when drawn, 0 on a bad table. */
#define FONT_TABLE_CHARS 96u

uint8_t cfw_font_line_height(void) { return (uint8_t)FONT_BUILTIN_HEIGHT; }

static int builtin_kern(uint32_t a, uint32_t b) {
    const uint8_t *k = font_builtin + FONT_BUILTIN_KERN_OFF;
    uint32_t lo = 0, hi = rd16(k), key = (a << 8) | b;
    k += 2;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        uint32_t cur = ((uint32_t)k[mid * 3u] << 8) | k[mid * 3u + 1u];
        if (cur == key) return (int)(int8_t)k[mid * 3u + 2u];
        if (cur < key) lo = mid + 1u; else hi = mid;
    }
    return 0;
}

/* The next character of a builtin-font string as a Latin-1 code, `*i` moved
 * past its UTF-8 bytes; a sequence outside Latin-1 (or a stray continuation
 * byte) is '?'. */
static uint32_t builtin_next(const uint8_t *str, uint32_t n, uint32_t *i) {
    uint32_t ch = str[(*i)++];
    if (ch < 0x80u) return ch;
    uint32_t extra = ch >= 0xF0u ? 3u : ch >= 0xE0u ? 2u : ch >= 0xC0u ? 1u : 0u;
    uint32_t code = '?';
    if ((ch == 0xC2u || ch == 0xC3u) && *i < n && (str[*i] & 0xC0u) == 0x80u) {
        code = ((ch & 0x1Fu) << 6) | (str[*i] & 0x3Fu);
    } else if (extra == 2u && *i + 1 < n && (str[*i] & 0xC0u) == 0x80u && (str[*i + 1] & 0xC0u) == 0x80u) {
        /* three-byte sequences: the font's extra glyphs (curly quotes, dashes, ...) sit after the Latin-1 slots */
        uint32_t cp = ((ch & 0x0Fu) << 12) | ((str[*i] & 0x3Fu) << 6) | (str[*i + 1] & 0x3Fu);
        for (uint32_t k = 0; k < FONT_BUILTIN_EXTRA_COUNT; k++)
            if (font_builtin_extra[k] == cp) { code = 32u + FONT_BUILTIN_LATIN1_CHARS + k; break; }
    }
    while (extra-- && *i < n && (str[*i] & 0xC0u) == 0x80u) (*i)++;
    return code;
}

static int draw_font_text(customCfwContext *ctx, const uint8_t *font, uint32_t flen, int raw,
                          const uint8_t *str, uint32_t n, int x, int y, const uint8_t *lut, int transparent) {
    uint32_t chars = raw ? (uint32_t)FONT_BUILTIN_CHARS : FONT_TABLE_CHARS;
    if (flen < chars * 4u) return 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t ch = raw ? builtin_next(str, n, &i) : str[i++];
        if (ch == 0) break;
        if (ch <= 31u) { x += (int)ch - 11; prev = 0; continue; }
        if (ch >= 32u + chars) ch = '?';
        uint32_t off = rd32(font + (ch - 32u) * 4u);
        if (off == 0 && ch != '?') { ch = '?'; off = rd32(font + (ch - 32u) * 4u); }   /* no glyph (0x80..0x9F) */
        if (off == 0 || off > flen - 3u) return 0;
        uint32_t w = font[off], h = font[off + 1];
        if (raw) {
            uint32_t rowbytes = (w + 1u) / 2u;
            if (off + 3u + rowbytes * h > flen || (w & 1u)) return 0;   /* raw rows: byte-aligned, low nibble first */
            if (prev) x += builtin_kern(prev, ch);
            blit_rows(ctx, font + off + 3, w, h, 0, x, y, lut, transparent);
            x += (int)font[off + 2];
            prev = ch;
        } else {
            w = blit_image(ctx, font + off, flen - off, x, y, lut, transparent);
            if (!w) return 0;
            x += (int)w;
        }
    }
    return 1;
}

static void draw_text(customCfwContext *ctx, cfw_object *o) {
    uint8_t lut[16];
    make_lut(o->color, lut);
    int transparent = (o->color & TEX_OPT_TRANSPARENT) != 0;
    if (o->type == SH_TEXT_INLINE) {
        if (o->text_len)
            draw_font_text(ctx, font_builtin, FONT_BUILTIN_BYTES, 1, ctx->store + o->text_off, o->text_len, o->p[0], o->p[1], lut, transparent);
        return;
    }
    cfw_asset *s = asset_find(ctx, o->asset_id);
    if (!s || !(s->state & 1)) return;
    if (o->type == SH_TEXT_CACHED && o->font_id) {
        cfw_asset *f = asset_find(ctx, o->font_id);
        if (f && (f->state & 1) &&
            draw_font_text(ctx, ctx->store + f->offset, f->length, 0, ctx->store + s->offset, s->length, o->p[0], o->p[1], lut, transparent))
            return;
    }
    draw_font_text(ctx, font_builtin, FONT_BUILTIN_BYTES, 1, ctx->store + s->offset, s->length, o->p[0], o->p[1], lut, transparent);
}

void cfw_scene_hide(customCfwContext *ctx, cfw_lens *L) {
    L->raster = 0;
    L->canvas_valid = 0;
    if (L->canvas) cfw_zero(L->canvas, (uint32_t)CFW_W * CFW_H);
    cfw_page_present(ctx);
}

static void render_object(customCfwContext *ctx, cfw_object *o) {
    if (!(o->flags & OBJ_VISIBLE)) return;
    uint8_t gray = (uint8_t)(o->color & 0x0f);
    switch (o->type) {
    case SH_LINE: case SH_RECT: case SH_RECT_FILL: case SH_CIRCLE: case SH_CIRCLE_FILL:
    case SH_TRI: case SH_TRI_FILL: case SH_QUAD: case SH_QUAD_FILL:
    case SH_BEZIER2: case SH_BEZIER3: case SH_ARC: case SH_PIE:
        ctx->draw_gray = (uint8_t)(gray * 17u);
        if (cfw_vec_rotated(&o->rotation) && ctx->vector_work) cfw_vec_shape_draw(ctx, ctx->vector_work, o);
        else cfw_geom_draw(ctx, o->type, o->width, o->p);
        break;
    case SH_PATH:
        if (o->path_bytes && ctx->vector_work) {
            ctx->draw_gray = (uint8_t)(gray * 17u);
            cfw_vec_path_draw(ctx, ctx->vector_work, (const cfw_path *)(const void *)(ctx->store + o->path_off), o->p, &o->rotation);
        }
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

/* Objects before the first animating one, in z order: the static layer. */
static uint16_t static_prefix(const cfw_lens *L) {
    for (uint16_t k = 0; k < L->active_count; k++) {
        const cfw_object *o = &L->objects[L->active_list[k]];
        if (o->frames || o->rotation.duration) return k;
    }
    return L->active_count;
}

/* Flat pages have identical per-eye scenes. Reuse the finished other eye
 * instead of rasterizing every glyph/shape twice. Compare actual draw state,
 * not ids/versions: two equal definitions may be at different tween phases. */
static int scene_same(customCfwContext *ctx, const cfw_lens *a, const cfw_lens *b) {
    if (!b->canvas || !b->canvas_valid || b->raster || b->canvas_serial != b->scene_serial ||
        a->bg != b->bg || a->active_count != b->active_count) return 0;
    for (uint32_t k = 0; k < a->active_count; k++) {
        const cfw_object *x = &a->objects[a->active_list[k]], *y = &b->objects[b->active_list[k]];
        if (x->type != y->type || x->flags != y->flags || x->color != y->color || x->width != y->width ||
            x->asset_id != y->asset_id || x->font_id != y->font_id || memcmp(x->p, y->p, sizeof(x->p)) ||
            x->rotation.angle != y->rotation.angle || x->rotation.px != y->rotation.px || x->rotation.py != y->rotation.py ||
            x->text_len != y->text_len || x->path_bytes != y->path_bytes) return 0;
        if (x->text_len && memcmp(ctx->store + x->text_off, ctx->store + y->text_off, x->text_len)) return 0;
        if (x->path_bytes && memcmp(ctx->store + x->path_off, ctx->store + y->path_off, x->path_bytes)) return 0;
    }
    return 1;
}

static void scene_render_canvas(customCfwContext *ctx, cfw_lens *L) {
    const cfw_lens *other = L == &ctx->lens[0] ? &ctx->lens[1] : &ctx->lens[0];
    if (scene_same(ctx, L, other)) {
        memcpy(L->canvas, other->canvas, (uint32_t)CFW_W * CFW_H);
        L->base_valid = 0;
        ctx->scene_copies++;
        return;
    }
    ctx->scene_renders++;
    ctx->canvas_buf = L->canvas;
    uint16_t prefix = static_prefix(L);
    int cached = L->base && prefix && L->base_valid && L->base_count == prefix && L->base_serial == L->scene_serial;
    if (cached) {
        memcpy(L->canvas, L->base, (uint32_t)CFW_W * CFW_H);
    } else {
        memset(L->canvas, (uint8_t)(L->bg * 17u), (uint32_t)CFW_W * CFW_H);
        for (uint16_t k = 0; k < prefix; k++) render_object(ctx, &L->objects[L->active_list[k]]);
        if (L->base && prefix < L->active_count) {   /* something animates on top: worth keeping */
            memcpy(L->base, L->canvas, (uint32_t)CFW_W * CFW_H);
            L->base_valid = 1;
            L->base_count = prefix;
            L->base_serial = L->scene_serial;
        } else {
            L->base_valid = 0;
        }
    }
    for (uint16_t k = prefix; k < L->active_count; k++) render_object(ctx, &L->objects[L->active_list[k]]);
    ctx->canvas_buf = 0;
}

void cfw_scene_render(customCfwContext *ctx, cfw_lens *L) {
    if (!L->canvas) return;
    uint32_t started = fw_tick();
    L->raster = 0;
    scene_render_canvas(ctx, L);
    L->canvas_serial = L->scene_serial;
    L->canvas_valid = 1;
    uint32_t elapsed = fw_tick() - started;
    ctx->scene_last_ms = elapsed > 65535u ? 65535u : (uint16_t)elapsed;
    if (ctx->scene_last_ms > ctx->scene_max_ms) ctx->scene_max_ms = ctx->scene_last_ms;
    cfw_page_present(ctx);
}

/* ---- immediate draws for the raster family (raster.c) ---------------------- */
/* A raster message takes the canvas over from the scene, as on the G2
 * (scene.c raster takeover): animations freeze, the active list empties (the
 * objects stay cached), a pending tag reports, and nothing re-renders the
 * canvas until the next SHOW/HIDE. */
void cfw_scene_takeover(customCfwContext *ctx, cfw_lens *L) {
    cfw_anim_freeze_all(L);
    for (uint32_t i = 0; i < L->active_count; i++) L->objects[L->active_list[i]].active = 0;
    L->active_count = 0;
    L->last_show_valid = 0;
    cfw_anim_settled(ctx, L);
    L->raster = 1;
    L->canvas_valid = 0;
    L->scene_serial++;
}

/* Byte length of one shape record at rec (mode 36 lists them back to back),
 * 0 when it cannot be sized within `avail`; PATH is not allowed there. */
int cfw_scene_record_len(const uint8_t *rec, uint32_t avail) {
    if (avail < 1) return 0;
    uint8_t type = rec[0];
    uint32_t rlen;
    if (type >= SH_LINE && type <= SH_PIE) rlen = 20;
    else if (type == SH_IMAGE || type == SH_TEXT) rlen = 12;
    else if (type == SH_TEXT_CACHED) rlen = 16;
    else if (type == SH_TEXT_INLINE) { if (avail < 13) return 0; rlen = 13u + rec[12]; if (!rec[12] || rec[12] > 128) return 0; }
    else return 0;
    return rlen <= avail ? (int)rlen : 0;
}

/* The record's assets must be resident and complete before anything draws. */
int cfw_scene_record_check(customCfwContext *ctx, const uint8_t *rec, uint32_t len) {
    if (cfw_scene_record_len(rec, len) != (int)len) return 0;
    uint8_t type = rec[0];
    if (type == SH_IMAGE || type == SH_TEXT || type == SH_TEXT_CACHED) {
        cfw_asset *a = asset_find(ctx, rd32(rec + 8));
        if (!a || !(a->state & 1) || a->kind != (type == SH_IMAGE ? ASSET_IMAGE : ASSET_STRING)) return 0;
        if (type == SH_TEXT_CACHED) {
            uint32_t font = rd32(rec + 12);
            if (font) { cfw_asset *f = asset_find(ctx, font); if (!f || !(f->state & 1) || f->kind != ASSET_FONT) return 0; }
        }
    }
    return 1;
}

void cfw_scene_record_draw(customCfwContext *ctx, cfw_lens *L, const uint8_t *rec, uint32_t len) {
    if (!L->canvas) return;
    ctx->canvas_buf = L->canvas;
    if (rec[0] == SH_TEXT_INLINE) {
        /* straight from the record: no store round trip */
        uint8_t lut[16];
        make_lut(rec[2], lut);
        draw_font_text(ctx, font_builtin, FONT_BUILTIN_BYTES, 1, rec + 13, rec[12], rds16(rec + 4), rds16(rec + 6), lut, (rec[2] & TEX_OPT_TRANSPARENT) != 0);
    } else {
        cfw_object o;
        cfw_zero(&o, sizeof o);
        o.flags = OBJ_VISIBLE;
        if (shape_into(ctx, &o, rec, len) > 0) render_object(ctx, &o);
    }
    ctx->canvas_buf = 0;
}

/* Mode 19: an image asset at (x, y) with the texture options. 0 = no such asset. */
int cfw_scene_draw_image_asset(customCfwContext *ctx, cfw_lens *L, uint32_t asset, int x, int y, uint8_t opt) {
    cfw_asset *a = asset_find(ctx, asset);
    if (!a || !(a->state & 1) || a->kind != ASSET_IMAGE || a->length < 2 || !L->canvas) return 0;
    uint8_t lut[16];
    make_lut(opt, lut);
    ctx->canvas_buf = L->canvas;
    uint32_t w = blit_image(ctx, ctx->store + a->offset, a->length, x, y, lut, (opt & TEX_OPT_TRANSPARENT) != 0);
    ctx->canvas_buf = 0;
    a->last_use = ++ctx->use_clock;
    return w != 0;
}

/* Modes 15 and 20: `n` bytes of text with a font asset (0 = the builtin
 * font) at (x, y). 0 = no such font or a glyph the font lacks. */
int cfw_scene_draw_string(customCfwContext *ctx, cfw_lens *L, uint32_t font, int x, int y, uint8_t opt, const uint8_t *str, uint32_t n) {
    if (!L->canvas) return 0;
    uint8_t lut[16];
    make_lut(opt, lut);
    int transparent = (opt & TEX_OPT_TRANSPARENT) != 0;
    int ok;
    ctx->canvas_buf = L->canvas;
    if (font) {
        cfw_asset *f = asset_find(ctx, font);
        if (!f || !(f->state & 1) || f->kind != ASSET_FONT) { ctx->canvas_buf = 0; return 0; }
        ok = draw_font_text(ctx, ctx->store + f->offset, f->length, 0, str, n, x, y, lut, transparent);
        f->last_use = ++ctx->use_clock;
    } else {
        ok = draw_font_text(ctx, font_builtin, FONT_BUILTIN_BYTES, 1, str, n, x, y, lut, transparent);
    }
    ctx->canvas_buf = 0;
    return ok;
}
