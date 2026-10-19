/* Host check of raster.c: every direct-framebuffer mode applied to a lens
 * canvas through the real decoder, draw and bundle code.
 *
 *   clang -std=c11 -Ipatches -Ipatches/generated -o raster_test tools/host/raster_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfw.h"
#define fw_memalign(a, n) malloc(n)
#define fw_free(p) free(p)
#define fw_tick() 100u

void cfw_zero(void *p, uint32_t n) { memset(p, 0, n); }
static int presents;
void cfw_page_present(customCfwContext *ctx) { ctx->disp_dirty = 1; if (!ctx->disp_hold) presents++; }
int cfw_uplink_push(customCfwContext *ctx, const uint8_t *d, uint8_t n) { (void)ctx; (void)d; (void)n; return 1; }
void cfw_anim_stop(customCfwContext *ctx) { (void)ctx; }
void cfw_anim_arm(customCfwContext *ctx) { (void)ctx; }
void cfw_anim_settled(customCfwContext *ctx, cfw_lens *L) { (void)ctx; (void)L; }
uint8_t cfw_op_check(customCfwContext *ctx, cfw_lens *L, const uint8_t *p, uint8_t *reason) { (void)ctx; (void)L; (void)p; (void)reason; return ST_APPLIED; }
cfw_vector_work *cfw_vec_work(customCfwContext *ctx) { (void)ctx; return NULL; }
int cfw_vec_compile(cfw_vector_work *w, const uint8_t *src, uint32_t len) { (void)w; (void)src; (void)len; return -1; }
void cfw_vec_path_draw(customCfwContext *ctx, cfw_vector_work *w, const cfw_path *path, const int16_t *p, const cfw_rotation *t) { (void)ctx; (void)w; (void)path; (void)p; (void)t; }
void cfw_vec_shape_draw(customCfwContext *ctx, cfw_vector_work *w, const cfw_object *o) { (void)ctx; (void)w; (void)o; }
int cfw_vec_rotated(const cfw_rotation *t) { (void)t; return 0; }
uint32_t cfw_op_len(const uint8_t *p, uint32_t avail) { (void)p; (void)avail; return 0; }
void cfw_op_apply(cfw_lens *L, const uint8_t *p) { (void)L; (void)p; }
void cfw_anim_freeze_all(cfw_lens *L) { (void)L; }
int cfw_anim_any(const cfw_lens *L) { (void)L; return 0; }

#include "rle.c"
#include "geom.c"
#include "scene.c"
#include "raster.c"

static customCfwContext ctx;
static uint8_t store[CFW_STORE_MAX_BYTES];
static uint8_t canvas[2][CFW_W * CFW_H];
static int fail;

static int px(int lens, int x, int y) { return canvas[lens][y * CFW_W + x] / 17; }
static void expect(const char *what, int got, int want) {
    printf("  %-44s %4d %s\n", what, got, got == want ? "ok" : "MISMATCH");
    if (got != want) fail = 1;
}
static int send(int lens, const uint8_t *msg, uint32_t len) {
    uint8_t mode, status, extra[REPLY_EXTRA_MAX], elen;
    uint16_t request;
    return cfw_raster_message(&ctx, &ctx.lens[lens], msg, len, lens == 0, &mode, &status, &request, extra, &elen);
}
static int send_both(const uint8_t *msg, uint32_t len) { int a = send(0, msg, len); int b = send(1, msg, len); return a && b; }
/* append RLE tokens for `n` pixels of `color` (split past the 16-bit count) */
static uint32_t run(uint8_t *out, uint8_t color, uint32_t n) {
    uint32_t k = 0;
    while (n) {
        uint32_t c = n > 65535 ? 65535 : n;
        out[k++] = color; out[k++] = 0; out[k++] = (uint8_t)c; out[k++] = (uint8_t)(c >> 8);
        n -= c;
    }
    return k;
}

int main(void) {
    ctx.store = store; ctx.store_bytes = CFW_STORE_MAX_BYTES; ctx.store_granules = STORE_GRANULES_MAX;
    for (int e = 0; e < 2; e++) { ctx.lens[e].canvas = canvas[e]; ctx.lens[e].bit = e ? LENS_RIGHT : LENS_LEFT; }
    store_clear(&ctx);
    ctx.page_open = 1;

    puts("mode 6: whole canvas, one 16-bit run of gray 9 then the rest gray 2");
    {
        uint32_t total = CFW_W * CFW_H, first = 1000;
        uint8_t m[16] = { 6 };
        uint32_t k = 1 + run(m + 1, 9, first);
        k += run(m + k, 2, total - first);
        expect("applied to both lenses", send_both(m, k), 1);
        expect("pixel 999 (row 1, col 459) is 9", px(0, 459, 1), 9);
        expect("pixel 1000 (row 1, col 460) is 2", px(1, 460, 1), 2);
        expect("last pixel is 2", px(0, CFW_W - 1, CFW_H - 1), 2);
        uint8_t shortm[5] = { 6, 0x09, 0x00, 0x10, 0x00 };
        expect("a stream short of the canvas is refused", send(0, shortm, 5), 0);
    }
    puts("mode 3: stereo boxes at different x, 8x4 of gray 15");
    {
        /* L box at (40,10), R box at (80,10), 8x4 = 32 pixels: runs 15x15, 15x15, 2x15 */
        uint8_t m[] = { 0x83, 10, 5, 2, 2, 20, 5, 2, 2, 0x34, 0x12, 0xff, 0xff, 0x2f };
        expect("applied to both lenses", send_both(m, sizeof m), 1);
        expect("left lens box pixel (40,10)", px(0, 40, 10), 15);
        expect("left lens outside box (48,10) untouched", px(0, 48, 10), 2);
        expect("right lens box pixel (80,13)", px(1, 80, 13), 15);
        expect("right lens (40,10) untouched", px(1, 40, 10), 2);
        uint8_t bad[] = { 0x83, 10, 5, 2, 2, 20, 5, 3, 2, 0, 0, 0xff, 0xff, 0x2f };
        expect("boxes of different size refused", send(0, bad, sizeof bad), 0);
        uint8_t mono[] = { 3, 0, 0, 1, 1, 0, 0, 0x8f };
        expect("mono box 4x2 at origin applied", send(0, mono, sizeof mono), 1);
        expect("(3,1) is 15", px(0, 3, 1), 15);
        uint8_t oob[] = { 3, 134, 0, 2, 1, 0, 0, 0x8f };
        expect("box past the right edge refused", send(0, oob, sizeof oob), 0);
    }
    puts("mode 9: copy the 8x4 box right by 100");
    {
        uint8_t m[17] = { 9, 40, 0, 10, 0, 8, 0, 4, 0, 140, 0, 10, 0, 8, 0, 4, 0 };
        expect("applied", send(0, m, 17), 1);
        expect("(140,10) is 15", px(0, 140, 10), 15);
        expect("(40,10) still 15", px(0, 40, 10), 15);
        uint8_t stereo[33] = { 0x89, 40, 0, 10, 0, 8, 0, 4, 0, 40, 0, 100, 0, 8, 0, 4, 0, 80, 0, 10, 0, 8, 0, 4, 0, 80, 0, 100, 0, 8, 0, 4, 0 };
        expect("stereo copy applied", send_both(stereo, 33), 1);
        expect("left (40,100)", px(0, 40, 100), 15);
        expect("right (80,100)", px(1, 80, 100), 15);
        uint8_t mism[17] = { 9, 0, 0, 0, 0, 8, 0, 4, 0, 0, 0, 50, 0, 8, 0, 5, 0 };
        expect("size mismatch refused", send(0, mism, 17), 0);
    }
    puts("mode 15: builtin text at (200,100)");
    {
        uint8_t m[] = { 15, 200, 0, 100, 0, 0x0f, 1, 'I' };
        expect("applied", send(0, m, sizeof m), 1);
        int ink = 0; for (int y = 100; y < 120; y++) for (int x = 200; x < 212; x++) ink += px(0, x, y) == 15;
        expect("ink in the glyph cell (> 10 px)", ink > 10, 1);
        uint8_t badlen[] = { 15, 200, 0, 100, 0, 0x0f, 2, 'I' };
        expect("wrong len refused", send(0, badlen, sizeof badlen), 0);
    }
    puts("mode 19/20: image and string assets");
    {
        /* asset 7: 2x2 image pixels 3 6 9 12 */
        uint8_t img[] = { 2, 2, 0x13, 0x16, 0x19, 0x1c };
        uint32_t off = store_alloc(&ctx, sizeof img);
        memcpy(store + off, img, sizeof img);
        ctx.assets[0].id = 7; ctx.assets[0].version = 1; ctx.assets[0].offset = off; ctx.assets[0].length = sizeof img; ctx.assets[0].kind = ASSET_IMAGE; ctx.assets[0].state = 1;
        uint8_t m[10] = { 19, 7, 0, 0, 0, 0x2c, 0x01, 50, 0, 0x0f };   /* x = 0x012c = 300 */
        expect("image draw applied", send(0, m, 10), 1);
        expect("(300,50) is 3", px(0, 300, 50), 3);
        expect("(301,51) is 12", px(0, 301, 51), 12);
        uint8_t inv[10] = { 19, 7, 0, 0, 0, 0x2c, 0x01, 60, 0, 0x2f };   /* inverse */
        send(0, inv, 10);
        expect("inverse: (300,60) is 12", px(0, 300, 60), 12);
        uint8_t missing[10] = { 19, 8, 0, 0, 0, 0x2c, 0x01, 60, 0, 0x0f };
        expect("unknown asset refused", send(0, missing, 10), 0);
        uint8_t s20[] = { 20, 0, 0, 0, 0, 10, 0, 10, 0, 0x0f, 1, 'I' };
        expect("string with font 0 (builtin) applied", send(0, s20, sizeof s20), 1);
        uint8_t s20bad[] = { 20, 9, 0, 0, 0, 10, 0, 10, 0, 0x0f, 1, 'I' };
        expect("string with an unknown font refused", send(0, s20bad, sizeof s20bad), 0);
    }
    puts("mode 36: a filled rect and an inline text");
    {
        uint8_t m[2 + 20 + 14];
        m[0] = 36; m[1] = 2;
        uint8_t rect[20] = { SH_RECT_FILL, 0, 0x0b, 0, 20, 0, 150, 0, 30, 0, 10, 0 };
        memcpy(m + 2, rect, 20);
        uint8_t text[14] = { SH_TEXT_INLINE, 0, 0x0f, 0, 100, 0, 150, 0, 12, 0, 20, 0, 1, 'I' };
        memcpy(m + 22, text, 14);
        expect("applied", send(0, m, sizeof m), 1);
        expect("rect pixel (25,155) is 11", px(0, 25, 155), 11);
        int ink = 0; for (int y = 150; y < 170; y++) for (int x = 100; x < 112; x++) ink += px(0, x, y) == 15;
        expect("inline text drew ink", ink > 10, 1);
        expect("trailing bytes refused", send(0, m, sizeof m - 1), 0);
        uint8_t path[14] = { 36, 1, SH_PATH, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0 };
        expect("PATH record refused", send(0, path, sizeof path), 0);
    }
    puts("mode 8: bundle = full black + text, presented once");
    {
        uint32_t total = CFW_W * CFW_H;
        uint8_t full[12] = { 6 };
        uint32_t fl = 1 + run(full + 1, 0, total);
        uint8_t text[8] = { 15, 10, 0, 10, 0, 0x0f, 1, 'I' };
        uint8_t m[2 + 2 + 12 + 2 + 8];
        m[0] = 8; m[1] = 2; m[2] = (uint8_t)fl; m[3] = 0; memcpy(m + 4, full, fl); m[4 + fl] = 8; m[5 + fl] = 0; memcpy(m + 6 + fl, text, 8);
        presents = 0;
        expect("bundle applied", send(0, m, 6 + fl + 8), 1);
        expect("one present", presents, 1);
        expect("(300,50) blanked", px(0, 300, 50), 0);
        int ink = 0; for (int y = 10; y < 30; y++) for (int x = 10; x < 22; x++) ink += px(0, x, y) == 15;
        expect("text on top of the blank", ink > 10, 1);
        uint8_t nested[2 + 2 + 2] = { 8, 1, 2, 0, 8, 0 };
        expect("nested bundle refused", send(0, nested, sizeof nested), 0);
    }
    puts("takeover: an active scene empties when a raster lands; HIDE takes the canvas back");
    {
        cfw_lens *L = &ctx.lens[0];
        L->objects[0].id = 1; L->objects[0].active = 1; L->active_list[0] = 0; L->active_count = 1; L->last_show_valid = 1;
        uint8_t m[8] = { 15, 10, 0, 10, 0, 0x0f, 1, 'I' };
        send(0, m, 8);
        expect("active list empty", L->active_count, 0);
        expect("raster flag set", L->raster, 1);
        cfw_scene_hide(&ctx, L);
        expect("raster flag cleared by HIDE", L->raster, 0);
        expect("canvas blank", px(0, 10, 10), 0);
    }
    puts("mode 11: cleanup drops the cache");
    {
        ctx.cache_ready = 1; ctx.stream.epoch = 5;
        uint8_t m[1] = { 11 };
        expect("applied", send_both(m, 1), 1);
        expect("epoch changed", ctx.stream.epoch != 5, 1);
        expect("cache not ready", ctx.cache_ready, 0);
        expect("asset table cleared", ctx.assets[0].id, 0);
    }
    puts("mode byte with bit 7 on a mode other than 3/9 is not a raster mode");
    expect("0x86", cfw_raster_mode(0x86), 0);
    expect("0x83", cfw_raster_mode(0x83), 1);
    return fail;
}
