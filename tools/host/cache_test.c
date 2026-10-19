/* Revision 11: real scene/animation/vector code, fake clock/heap/presentation.
 * Covers uploads, rollback under eviction/allocation failure, and stereo ticks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfw.h"

static uint32_t now = 100;
static int fail_after = -1, allocations, presents;
static void *host_alloc(uint32_t alignment, uint32_t n) {
    (void)alignment;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc(n); if (p) allocations++;
    return p;
}
static void host_free(void *p) { if (p) allocations--; free(p); }
static uint32_t host_tick(void) { return now; }
#define fw_memalign host_alloc
#define fw_free host_free
#define fw_tick host_tick
void cfw_zero(void *p, uint32_t n) { memset(p, 0, n); }
void cfw_page_present(customCfwContext *ctx) {
    ctx->disp_dirty = 1;
    if (!ctx->disp_hold) { presents++; ctx->disp_pan_pending = 1; }
}
int cfw_uplink_push(customCfwContext *ctx, const uint8_t *d, uint8_t n) { (void)ctx; (void)d; (void)n; return 1; }
#include "rle.c"
#define cfw_geom_draw raw_geom_draw
#include "geom.c"
#undef cfw_geom_draw
static unsigned geometry_draws;
void cfw_geom_draw(customCfwContext *c, uint8_t t, uint8_t w, const int16_t *p) {
    geometry_draws++; raw_geom_draw(c, t, w, p);
}
#include "vector.c"
#include "scene.c"
#include "anim.c"

static customCfwContext ctx;
static uint8_t canvas[2][CFW_W * CFW_H], base[2][CFW_W * CFW_H];
static cfw_vector_work work;
static uint16_t request_id;
static void reset(uint32_t bytes) {
    assert(allocations == 0);
    free(ctx.store); memset(&ctx, 0, sizeof ctx); memset(canvas, 0, sizeof canvas);
    ctx.store = malloc(bytes); memset(ctx.store, 0xa5, bytes);
    ctx.store_bytes = bytes; ctx.store_granules = bytes / STORE_GRANULE;
    ctx.page_open = ctx.cache_ready = 1; ctx.vector_work = &work;
    for (int e = 0; e < 2; e++) { ctx.lens[e].canvas = canvas[e]; ctx.lens[e].base = base[e]; ctx.lens[e].bit = 1 << e; }
    request_id = 0; presents = 0; fail_after = -1;
}
static uint8_t message(int lens, uint8_t *m, uint32_t n) {
    uint8_t status = 99, extra[REPLY_EXTRA_MAX], elen; uint16_t req;
    assert(cfw_scene_message(&ctx, &ctx.lens[lens], m, n, &status, &req, lens == 0, extra, &elen));
    assert(!ctx.transaction); assert(allocations == 0);
    return status;
}
static uint32_t asset_entry(uint8_t *b, uint32_t id, uint32_t ver, uint32_t total, uint32_t off, uint16_t n, uint8_t val) {
    b[0] = PUT_ASSET; cfw_put32(b + 1, id); cfw_put32(b + 5, ver); b[9] = ASSET_STRING;
    cfw_put32(b + 10, total); cfw_put32(b + 14, off); cfw_put16(b + 18, n); memset(b + 20, val, n); return 20u + n;
}
static uint8_t asset(uint32_t id, uint32_t ver, uint32_t total, uint32_t off, uint16_t n, uint8_t val) {
    uint8_t m[1024] = {MSG_PUT}; cfw_put16(m + 3, ++request_id);
    uint32_t size = asset_entry(m + 5, id, ver, total, off, n, val); return message(0, m, size + 5);
}
static uint32_t object_entry(uint8_t *b, uint32_t id, uint32_t ver, uint8_t color) {
    memset(b, 0, 29); b[0] = PUT_OBJECT; cfw_put32(b + 1, id); cfw_put32(b + 5, ver);
    b[9] = SH_RECT_FILL; b[11] = color; cfw_put16(b + 17, 10); cfw_put16(b + 19, 10); return 29;
}
static void object(uint32_t id, uint32_t ver, uint8_t color) {
    uint8_t m[34] = {MSG_PUT}; cfw_put16(m + 3, ++request_id); object_entry(m + 5, id, ver, color);
    assert(message(0, m, sizeof m) == ST_APPLIED);
}
static uint32_t show_start(uint8_t *m, uint8_t puts) {
    memset(m, 0, 8); m[0] = MSG_SHOW; cfw_put16(m + 3, ++request_id); m[7] = puts; return 8;
}
static uint32_t refs(uint8_t *m, uint32_t n, uint32_t id, uint32_t ver) {
    cfw_put16(m + n, 1); cfw_put32(m + n + 2, id); cfw_put32(m + n + 6, ver); return n + 10;
}
static void uploads(void) {
    reset(CFW_STORE_MAX_BYTES);
    assert(asset(1, 1, 16, 15, 1, 'Z') == ST_REFUSED); assert(!asset_find(&ctx, 1));
    assert(asset(1, 1, 16, 0, 8, 'A') == ST_APPLIED);
    assert(asset(1, 1, 16, 9, 7, 'B') == ST_REFUSED);
    assert(asset(1, 1, 16, 0xffffffffu, 1, 'B') == ST_REFUSED);
    assert(asset(1, 1, 65, 64, 1, 'B') == ST_REFUSED);
    assert(asset(1, 1, 16, 0, 8, 'A') == ST_APPLIED); // bilateral/retry
    assert(asset(1, 1, 16, 0, 8, 'B') == ST_REFUSED);
    assert(asset(1, 1, 16, 8, 8, 'B') == ST_APPLIED);
    cfw_asset *a = asset_find(&ctx, 1); assert(a->state & 1);
    assert(asset(1, 2, 16, 0, 8, 'C') == ST_APPLIED);
    assert(asset_find(&ctx, 1)->version == 1); assert(ctx.store[a->offset] == 'A');
    assert(asset(1, 2, 16, 8, 8, 'D') == ST_APPLIED);
    a = asset_find(&ctx, 1); assert(a->version == 2 && (a->state & 1));
    assert(!asset_version(&ctx, 1, 1)); assert(ctx.store_used == STORE_GRANULE);
    // Original heap overflow repro: target allocation ends at the store boundary.
    reset(64); assert(asset(1, 1, 16, 0, 16, 'A') == ST_APPLIED);
    assert(asset(1, 1, 64, 63, 1, 'B') == ST_REFUSED);
    assert(asset(1, 2, 16, 0, 1, 'B') == ST_CAPACITY);
    assert(asset_find(&ctx, 1)->version == 1 && ctx.store_used == 64);
    puts("uploads: bounds, holes, retries, atomic replacement, capacity ok");
}
static void rollback(void) {
    reset(128); object(42, 1, 15); cfw_object before = *object_find(&ctx.lens[0], 42);
    uint32_t rev = ctx.lens[0].revision;
    uint8_t m[256]; uint32_t n = show_start(m, 1); n += object_entry(m + n, 42, 2, 7); n = refs(m, n, 999, 1);
    assert(message(0, m, n) == ST_MISSING);
    assert(!memcmp(&before, object_find(&ctx.lens[0], 42), sizeof before)); assert(ctx.lens[0].revision == rev); assert(!presents);
    // Valid references but an invalid op must also roll the inline PUT back.
    cfw_put16(m + 3, ++request_id); cfw_put32(m + n - 8, 42); cfw_put32(m + n - 4, 2); m[n] = 255;
    assert(message(0, m, n + 1) == ST_REFUSED); assert(object_find(&ctx.lens[0], 42)->version == 1);
    m[n] = OP_MOVE; cfw_put32(m + n + 1, 999); cfw_put16(m + n + 5, 1); cfw_put16(m + n + 7, 1);
    cfw_put16(m + 3, ++request_id);
    assert(message(0, m, n + 9) == ST_REFUSED); assert(object_find(&ctx.lens[0], 42)->version == 1);
    fail_after = 1; cfw_put16(m + 3, ++request_id);
    assert(message(0, m, n) == ST_CAPACITY); fail_after = -1;
    assert(!memcmp(&before, object_find(&ctx.lens[0], 42), sizeof before));
    // Evict and overwrite a live store allocation, then reject SHOW. Restore bytes and journal.
    assert(asset(1, 1, 128, 0, 128, 'X') == ST_APPLIED);
    uint8_t old[128]; memcpy(old, ctx.store, sizeof old); rev = ctx.lens[0].revision;
    n = show_start(m, 1); n += asset_entry(m + n, 2, 1, 128, 0, 128, 'Y'); n = refs(m, n, 999, 1);
    assert(message(0, m, n) == ST_MISSING); assert(asset_find(&ctx, 1)); assert(!asset_find(&ctx, 2));
    assert(!memcmp(old, ctx.store, sizeof old)); assert(ctx.lens[0].revision == rev);
    // Every allocation failure preserves the old cache, including undo-log OOM.
    for (int k = 0; k < 3; k++) {
        fail_after = k; cfw_put16(m + 3, ++request_id); uint8_t st = message(0, m, n); fail_after = -1;
        assert(st == ST_CAPACITY || st == ST_MISSING); assert(asset_find(&ctx, 1)); assert(!asset_find(&ctx, 2));
        assert(!memcmp(old, ctx.store, sizeof old));
    }
    // A successful replacement still evicts and reclaims store capacity.
    assert(asset(2, 1, 128, 0, 128, 'Y') == ST_APPLIED); assert(!asset_find(&ctx, 1)); assert(ctx.store_used == 128);
    // Rejected PUT_OBJECT preserves the previous definition too.
    uint8_t bad[15] = {MSG_PUT}; cfw_put16(bad + 3, ++request_id); object_entry(m, 42, 2, 1);
    memcpy(bad + 5, m, 10); assert(message(0, bad, sizeof bad) == ST_REFUSED); assert(object_find(&ctx.lens[0], 42)->version == 1);
    puts("transactions: missing refs, bad ops, eviction bytes/journal, OOM, invalid PUT ok");
}
static void asset_validation(void) {
    reset(1024);
    uint8_t m[420] = {MSG_PUT};
    uint32_t n = asset_entry(m + 5, 9, 1, 3, 0, 3, 0);
    m[14] = ASSET_IMAGE; m[25] = 1; m[26] = 1; m[27] = 0x1f;
    cfw_put16(m + 3, ++request_id); assert(message(0, m, n + 5) == ST_APPLIED);
    cfw_put32(m + 10, 2); m[27] = 0x2f;
    cfw_put16(m + 3, ++request_id); assert(message(0, m, n + 5) == ST_REFUSED);
    assert(asset_find(&ctx, 9)->version == 1); assert(!asset_version(&ctx, 9, 2));
    n = asset_entry(m + 5, 10, 1, 384, 0, 384, 0); m[14] = ASSET_FONT;
    cfw_put32(m + 25, 0xffffffffu); cfw_put16(m + 3, ++request_id);
    assert(message(0, m, n + 5) == ST_REFUSED); assert(!asset_find(&ctx, 10));
    puts("asset validation: invalid replacement RLE and overflowing font offsets refused");
}

static void motion(void) {
    reset(1024);
    cfw_object *o = &ctx.lens[0].objects[0]; o->id = 42; o->active = 1; o->type = SH_PATH; o->p[0] = 10; o->p[1] = 20;
    uint8_t move[14] = {OP_MOVE}; cfw_put32(move + 1, 42); cfw_put16(move + 5, 30); cfw_put16(move + 7, 40);
    cfw_op_apply(&ctx.lens[0], move); assert(o->p[0] == 40 && o->p[1] == 60);
    move[0] = OP_GLIDE; move[9] = 2; move[12] = move[13] = 255; cfw_op_apply(&ctx.lens[0], move);
    ctx.lens[0].active_count = 1; an_advance(&ctx.lens[0]); an_advance(&ctx.lens[0]); assert(o->p[0] == 70 && o->p[1] == 100);
    for (int e = 0; e < 2; e++) {
        cfw_lens *L = &ctx.lens[e]; o = &L->objects[0]; memset(o, 0, sizeof *o);
        o->id = 42; o->type = SH_RECT_FILL; o->flags = OBJ_VISIBLE; o->active = 1; o->color = 15; o->p[2] = o->p[3] = 5;
        L->active_count = 1; L->active_list[0] = 0;
        static const uint8_t linear[4] = {0,0,255,255}; an_glide(o, 60, 0, 10, linear);
    }
    ctx.anim_active = 1; ctx.anim_last_tick = 100; now = 133;
    cfw_anim_tick(&ctx); assert(ctx.anim_renders == 2 && ctx.anim_skipped == 0 && presents == 1);
    assert(!memcmp(canvas[0], canvas[1], sizeof canvas[0])); assert(canvas[0][6] == 255);
    now = 166; cfw_anim_tick(&ctx); assert(ctx.anim_renders == 2 && ctx.anim_skipped == 2);
    ctx.disp_pan_pending = 0; now = 199; cfw_anim_tick(&ctx); assert(ctx.anim_renders == 4 && presents == 2);
    assert(!memcmp(canvas[0], canvas[1], sizeof canvas[0]));
    puts("motion: PATH move/glide; both eyes render before one present; shared backpressure ok");
}
static void flat_render(void) {
    reset(1024);
    object(1, 1, 15);
    uint8_t put[34] = {MSG_PUT}; object_entry(put + 5, 1, 1, 15); cfw_put16(put + 3, ++request_id);
    assert(message(1, put, sizeof put) == ST_APPLIED);
    uint8_t m[64]; uint32_t n = show_start(m, 0); n = refs(m, n, 1, 1);
    assert(message(0, m, n) == ST_APPLIED); assert(message(1, m, n) == ST_APPLIED);
    assert(ctx.scene_renders == 1 && ctx.scene_copies == 1);
    assert(!memcmp(canvas[0], canvas[1], sizeof canvas[0]));
    // STATE queries preserve the render cache; an unequal right-eye object must rasterize.
    uint32_t serial = ctx.lens[0].scene_serial;
    uint8_t state[9] = {MSG_STATE}; message(0, state, sizeof state); assert(ctx.lens[0].scene_serial == serial);
    object_entry(put + 5, 1, 2, 7); cfw_put16(put + 3, ++request_id); assert(message(1, put, sizeof put) == ST_APPLIED);
    n = show_start(m, 0); n = refs(m, n, 1, 2); assert(message(1, m, n) == ST_APPLIED);
    assert(ctx.scene_renders == 2 && ctx.scene_copies == 1); assert(canvas[0][0] != canvas[1][0]);
    // No-puts SHOWs don't leave the references pinned forever.
    assert(!ctx.lens[0].objects[0].pinned && !ctx.lens[1].objects[0].pinned);
    puts("flat scenes: one rasterization + one copy; stereo differences and STATE handled correctly");
}

static void static_prefix_cache(void) {
    reset(1024); geometry_draws = 0;
    cfw_lens *L = &ctx.lens[0];
    for (unsigned i = 0; i < 2; i++) {
        cfw_object *o = &L->objects[i];
        o->id = i + 1; o->type = SH_RECT_FILL; o->flags = OBJ_VISIBLE; o->active = 1;
        o->color = 15; o->p[0] = (int16_t)(i * 40); o->p[2] = o->p[3] = 10;
        L->active_list[i] = (uint16_t)i;
    }
    L->active_count = 2;
    static const uint8_t linear[4] = {0,0,255,255};
    an_glide(&L->objects[1], 60, 0, 10, linear);
    cfw_scene_render(&ctx, L); assert(geometry_draws == 2);
    ctx.anim_active = 1; ctx.anim_last_tick = 100; now = 133; ctx.disp_pan_pending = 0;
    cfw_anim_tick(&ctx); assert(geometry_draws == 3); // the static rectangle was not redrawn
    puts("static prefix: animation keeps its cached background");
}

static void text_reuse(void) {
    reset(2048);
    uint8_t m[28] = {MSG_PUT};
    uint8_t *e = m + 5; e[0] = PUT_OBJECT; cfw_put32(e + 1, 1); cfw_put32(e + 5, 1);
    e[9] = SH_TEXT_INLINE; e[11] = 31; cfw_put16(e + 17, 100); cfw_put16(e + 19, 25); e[21] = 1; e[22] = 'I';
    cfw_put16(m + 3, ++request_id); assert(message(0, m, sizeof m) == ST_APPLIED);
    assert(message(1, m, sizeof m) == ST_APPLIED);
    assert(ctx.lens[0].objects[0].text_off != ctx.lens[1].objects[0].text_off);
    uint8_t sh[32]; uint32_t n = show_start(sh, 0); n = refs(sh, n, 1, 1);
    assert(message(0, sh, n) == ST_APPLIED); assert(message(1, sh, n) == ST_APPLIED);
    assert(ctx.scene_renders == 1 && ctx.scene_copies == 1); assert(!memcmp(canvas[0], canvas[1], sizeof canvas[0]));
    e[22] = 'L'; cfw_put32(e + 5, 2); cfw_put16(m + 3, ++request_id); assert(message(1, m, sizeof m) == ST_APPLIED);
    n = show_start(sh, 0); n = refs(sh, n, 1, 2); assert(message(1, sh, n) == ST_APPLIED);
    assert(ctx.scene_renders == 2 && ctx.scene_copies == 1); assert(memcmp(canvas[0], canvas[1], sizeof canvas[0]));
    puts("text: matching strings at distinct store offsets copy; different strings render");
}

int main(void) { uploads(); rollback(); asset_validation(); motion(); flat_render(); static_prefix_cache(); text_reuse(); free(ctx.store); assert(!allocations); puts("cache/animation regression tests passed"); }
