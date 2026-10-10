/* Animation engine for the object cache: the SHOW ops of the G2 Glassly CFW
 * (scene.c) on the RayNeo page. Objects carry a from/to geometry, color and
 * stroke width, a frame count and a cubic-bezier easing; an LVGL timer
 * (CFW_ANIM_PERIOD_MS) advances every active object one frame, re-renders the
 * canvas and, once nothing moves, reports SETTLED for a tagged SHOW.
 *
 * Ops (after the refs of a SHOW; id 0xffffffff = every active object for
 * VISIBLE/MOVE/GLIDE/FREEZE/FINISH):
 *   [2][id][visible u8]                                   VISIBLE
 *   [3][id][dx i16][dy i16]                               MOVE   (jump)
 *   [4][id][dx i16][dy i16][frames u8][curve x4]          GLIDE  (eased move)
 *   [5][id][mask u16][frames u8][curve x4][i16 per bit]   TWEEN  (bits 0-7 = p0..p7,
 *                                                         0x100 color, 0x200 width)
 *   [6][id]  FREEZE      [7][id]  FINISH
 *   [9][id]... ROTATE is refused (reason 3): no rotated primitives on RayNeo.
 * Easing is Q15 with no 64-bit division (as on the G2). Everything here runs
 * on the LVGL thread: the message handler applies, the timer advances. */
#include "cfw.h"

/* ---- easing (Q15) ---------------------------------------------------------- */
static uint32_t an_ease_byte_q15(uint8_t b) { return ((uint32_t)b * 32768u + 127u) / 255u; }

/* One axis of a cubic bezier with P0 = 0 and P3 = 1. */
static uint32_t an_ease_axis(uint32_t t, uint32_t p1, uint32_t p2) {
    uint32_t mt = 32768u - t;
    uint64_t a = ((uint64_t)3u * mt * mt) >> 15;
    a = (a * t) >> 15;
    uint64_t b = ((uint64_t)3u * mt * t) >> 15;
    b = (b * t) >> 15;
    uint64_t c = ((uint64_t)t * t) >> 15;
    c = (c * t) >> 15;
    return (uint32_t)((a * p1 + b * p2 + (c << 15)) >> 15);
}

/* Progress (Q15) at normalized time u (Q15): solve x(t) = u by bisection, evaluate y(t). */
static uint32_t an_ease_progress(uint32_t u, const uint8_t *curve) {
    if (u == 0) return 0;
    if (u >= 32768u) return 32768u;
    uint32_t x1 = an_ease_byte_q15(curve[0]), y1 = an_ease_byte_q15(curve[1]);
    uint32_t x2 = an_ease_byte_q15(curve[2]), y2 = an_ease_byte_q15(curve[3]);
    uint32_t lo = 0, hi = 32768u;
    for (uint32_t i = 0; i < 14; i++) {
        uint32_t mid = (lo + hi) >> 1;
        if (an_ease_axis(mid, x1, x2) < u) lo = mid; else hi = mid;
    }
    uint32_t y = an_ease_axis((lo + hi) >> 1, y1, y2);
    return y > 32768u ? 32768u : y;
}

static int32_t an_lerp_q15(int32_t from, int32_t to, uint32_t prog) {
    int32_t d = to - from;
    return from + (int32_t)(((int64_t)d * (int32_t)prog + 16384) >> 15);
}

static int16_t an_sat16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }
static uint8_t an_sat8(int32_t v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* ---- per-type parameter roles (G2 shapes.c) -------------------------------- */
/* Which params are x / y coordinates (glides translate exactly these). */
static const uint8_t an_xmask_tab[18] = {
    0x00, 0x05, 0x01, 0x01, 0x01, 0x01, 0x15, 0x15, 0x55, 0x55,
    0x15, 0x55, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};
static const uint8_t an_ymask_tab[18] = {
    0x00, 0x0a, 0x02, 0x02, 0x02, 0x02, 0x2a, 0x2a, 0xaa, 0xaa,
    0x2a, 0xaa, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
};

static void an_masks(uint8_t type, uint8_t *xm, uint8_t *ym) {
    if (type >= 18) type = 0;
    *xm = an_xmask_tab[type];
    *ym = an_ymask_tab[type];
}

/* Parameters a TWEEN may target (bits 0-7 = p0..p7, 0x100 color, 0x200 width). */
static uint32_t an_tween_mask(uint8_t type) {
    switch (type) {
    case SH_LINE:        return 0x30fu;
    case SH_RECT: case SH_RECT_FILL:
    case SH_ARC:  case SH_PIE:         return 0x31fu;
    case SH_CIRCLE: case SH_CIRCLE_FILL: return 0x307u;
    case SH_TRI: case SH_TRI_FILL: case SH_BEZIER2: return 0x33fu;
    case SH_QUAD: case SH_QUAD_FILL: case SH_BEZIER3: return 0x3ffu;
    case SH_IMAGE: case SH_TEXT_CACHED: return 0x103u;
    case SH_TEXT_INLINE: return 0x10fu;
    default:             return 0;
    }
}

/* ---- per-object motion ----------------------------------------------------- */
static void an_snap_end(cfw_object *o) {
    for (uint32_t k = 0; k < 8; k++) o->p[k] = o->to[k];
    o->color = o->color_to;
    o->width = o->width_to;
    o->frames = 0;
    o->frame = 0;
}

static void an_finish(cfw_object *o) { if (o->frames) an_snap_end(o); }

static void an_freeze(cfw_object *o) { o->frames = 0; o->frame = 0; }

/* Begin an eased move from the current values to `to`, `color_to`, `width_to` over `frames`. */
static void an_animate(cfw_object *o, const int16_t *to, uint8_t color_to, uint8_t width_to,
                       uint8_t frames, const uint8_t *curve) {
    if (frames <= 1u) {
        for (uint32_t k = 0; k < 8; k++) o->p[k] = to[k];
        o->color = color_to;
        o->width = width_to;
        o->frames = 0;
        o->frame = 0;
        return;
    }
    for (uint32_t k = 0; k < 8; k++) { o->from[k] = o->p[k]; o->to[k] = to[k]; }
    o->color_from = o->color; o->color_to = color_to;
    o->width_from = o->width; o->width_to = width_to;
    o->frame = 0;
    o->frames = frames;
    for (uint32_t k = 0; k < 4; k++) o->curve[k] = curve[k];
}

static void an_glide(cfw_object *o, int32_t dx, int32_t dy, uint8_t frames, const uint8_t *curve) {
    static const uint8_t linear[4] = {0, 0, 255, 255};
    uint8_t xm, ym;
    an_masks(o->type, &xm, &ym);
    int16_t to[8];
    for (uint32_t k = 0; k < 8; k++) {
        /* compose with an in-flight glide: continue toward the old target plus the delta */
        int32_t base = o->frames ? o->to[k] : o->p[k];
        if (xm & (1u << k)) base += dx;
        if (ym & (1u << k)) base += dy;
        to[k] = an_sat16(base);
    }
    uint8_t c = o->frames ? o->color_to : o->color;
    uint8_t w = o->frames ? o->width_to : o->width;
    an_animate(o, to, c, w, frames, curve ? curve : linear);
}

/* Advance every active object one frame; 1 while any is still moving. */
static int an_advance(cfw_lens *L) {
    int moving = 0;
    for (uint32_t i = 0; i < L->active_count; i++) {
        cfw_object *o = &L->objects[L->active_list[i]];
        if (o->id == 0 || o->frames == 0) continue;
        uint32_t frame = (uint32_t)o->frame + 1u;
        if (frame >= o->frames) { an_snap_end(o); continue; }
        uint32_t prog = an_ease_progress(frame * 32768u / o->frames, o->curve);
        for (uint32_t k = 0; k < 8; k++) o->p[k] = an_sat16(an_lerp_q15(o->from[k], o->to[k], prog));
        o->color = an_sat8(an_lerp_q15(o->color_from, o->color_to, prog));
        o->width = an_sat8(an_lerp_q15(o->width_from, o->width_to, prog));
        o->frame = (uint8_t)frame;
        moving = 1;
    }
    return moving;
}

int cfw_anim_any(const cfw_lens *L) {
    for (uint32_t i = 0; i < L->active_count; i++) {
        const cfw_object *o = &L->objects[L->active_list[i]];
        if (o->id != 0 && o->frames) return 1;
    }
    return 0;
}

/* ---- ops ------------------------------------------------------------------- */
static uint32_t an_popcount10(uint32_t v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 10; i++) n += (v >> i) & 1u;
    return n;
}

/* Byte length of the op at p, or 0 when malformed (syntax only). */
uint32_t cfw_op_len(const uint8_t *p, uint32_t avail) {
    if (avail < 5u) return 0;
    uint32_t need;
    switch (p[0]) {
    case OP_VISIBLE: need = 6u; break;
    case OP_MOVE:    need = 9u; break;
    case OP_GLIDE:   need = 14u; break;
    case OP_FREEZE:
    case OP_FINISH:  need = 5u; break;
    case OP_ROTATE:  need = 19u; break;
    case OP_TWEEN: {
        if (avail < 12u) return 0;
        uint32_t mask = rd16(p + 5);
        if (mask == 0 || (mask & ~TWEEN_MASK_VALID)) return 0;
        need = 12u + 2u * an_popcount10(mask);
        break;
    }
    default: return 0;
    }
    if (need > avail) return 0;
    if (rd32(p + 1) == OP_ALL_OBJECTS && (p[0] == OP_TWEEN || p[0] == OP_ROTATE)) return 0;
    return need;
}

/* Semantic check against the incoming active set (objects marked `active`):
 * ST_APPLIED, or a REFUSED reason in *reason. */
uint8_t cfw_op_check(cfw_lens *L, const uint8_t *p, uint8_t *reason) {
    uint32_t id = rd32(p + 1);
    if (p[0] == OP_ROTATE) { *reason = REFUSE_TARGET; return ST_REFUSED; }
    if (id == OP_ALL_OBJECTS) return ST_APPLIED;
    cfw_object *o = object_find(L, id);
    if (!o || !o->active) { *reason = REFUSE_TARGET; return ST_REFUSED; }
    if (p[0] == OP_TWEEN) {
        uint32_t mask = rd16(p + 5);
        if (mask & ~an_tween_mask(o->type)) { *reason = REFUSE_MASK; return ST_REFUSED; }
    }
    return ST_APPLIED;
}

static void an_apply_one(cfw_object *o, const uint8_t *p) {
    switch (p[0]) {
    case OP_VISIBLE:
        if (p[5]) o->flags |= OBJ_VISIBLE; else o->flags &= (uint8_t)~OBJ_VISIBLE;
        break;
    case OP_MOVE:
        an_glide(o, (int16_t)rd16(p + 5), (int16_t)rd16(p + 7), 0, 0);
        break;
    case OP_GLIDE:
        an_glide(o, (int16_t)rd16(p + 5), (int16_t)rd16(p + 7), p[9], p + 10);
        break;
    case OP_TWEEN: {
        uint32_t mask = rd16(p + 5);
        uint8_t frames = p[7];
        const uint8_t *curve = p + 8;
        const uint8_t *v = p + 12;
        int16_t to[8];
        for (uint32_t k = 0; k < 8; k++) {
            to[k] = o->p[k];
            if (mask & (1u << k)) { to[k] = (int16_t)rd16(v); v += 2; }
        }
        uint8_t c = o->color, w = o->width;
        if (mask & TWEEN_COLOR_BIT) { c = (uint8_t)rd16(v); v += 2; }
        if (mask & TWEEN_WIDTH_BIT) { w = (uint8_t)rd16(v); v += 2; }
        an_animate(o, to, c, w, frames, curve);
        break;
    }
    case OP_FREEZE: an_freeze(o); break;
    case OP_FINISH: an_finish(o); break;
    default: break;
    }
}

void cfw_op_apply(cfw_lens *L, const uint8_t *p) {
    uint32_t id = rd32(p + 1);
    if (id == OP_ALL_OBJECTS) {
        for (uint32_t i = 0; i < L->active_count; i++) an_apply_one(&L->objects[L->active_list[i]], p);
        return;
    }
    cfw_object *o = object_find(L, id);
    if (o) an_apply_one(o, p);
}

void cfw_anim_freeze_all(cfw_lens *L) {
    for (uint32_t i = 0; i < L->active_count; i++) an_freeze(&L->objects[L->active_list[i]]);
}

/* ---- settle + timer -------------------------------------------------------- */
/* SETTLED per lens, as each G2 lens reports its own. */
void cfw_anim_settled(customCfwContext *ctx, cfw_lens *L) {
    if (!L->settle_pending) return;
    L->settle_pending = 0;
    uint8_t b[3] = {RPL_SETTLED, (uint8_t)L->settle_tag, (uint8_t)(L->settle_tag >> 8)};
    cfw_uplink_push(ctx, b, sizeof(b));
}

void cfw_anim_stop(customCfwContext *ctx) {
    ctx->anim_active = 0;
}

/* One animation frame, driven by the page's service timer (page.c) every
 * CFW_ANIM_PERIOD_MS while anything moves: no LVGL timer is created or
 * deleted per animation (the first revision 5 build lost frames that way). */
void cfw_anim_tick(customCfwContext *ctx) {
    if (!ctx->anim_active) return;
    if (!ctx->page_open) { cfw_anim_stop(ctx); return; }
    uint32_t now = fw_tick();
    if (ctx->anim_last_tick && (now - ctx->anim_last_tick) < CFW_ANIM_PERIOD_MS) return;
    ctx->anim_last_tick = now;
    int moving = 0;
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        int was = cfw_anim_any(L);
        if (!was) continue;
        if (an_advance(L)) moving = 1;
        cfw_scene_render(ctx, L);
        if (!cfw_anim_any(L)) cfw_anim_settled(ctx, L);
    }
    if (!moving) cfw_anim_stop(ctx);
}

/* After a SHOW/HIDE: run the frame timer while anything moves in either
 * lens; a lens with nothing moving settles now. */
void cfw_anim_arm(customCfwContext *ctx) {
    int any = 0;
    for (unsigned e = 0; e < 2; e++) {
        cfw_lens *L = &ctx->lens[e];
        if (cfw_anim_any(L)) any = 1; else cfw_anim_settled(ctx, L);
    }
    if (!any) { cfw_anim_stop(ctx); return; }
    if (!ctx->anim_active) ctx->anim_last_tick = fw_tick();
    ctx->anim_active = 1;
}
