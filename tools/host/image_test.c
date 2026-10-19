/* Host check of scene.c's cached-image path: a 3x2 image `[w][h][RLE]` (an
 * odd width, so no row padding may be assumed) drawn through the real RLE
 * decoder, LUT and blit must land pixel for pixel on the canvas, left to
 * right, and the texture options must apply.
 *
 *   clang -std=c11 -Ipatches -Ipatches/generated -o image_test tools/host/image_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfw.h"
#define fw_memalign(a, n) malloc(n)
#define fw_free(p) free(p)
#define fw_tick() 100u

void cfw_zero(void *p, uint32_t n) { memset(p, 0, n); }
void cfw_page_present(customCfwContext *ctx) { (void)ctx; }
int cfw_uplink_push(customCfwContext *ctx, const uint8_t *d, uint8_t n) { (void)ctx; (void)d; (void)n; return 1; }
void cfw_anim_stop(customCfwContext *ctx) { (void)ctx; }
void cfw_anim_arm(customCfwContext *ctx) { (void)ctx; }
void cfw_anim_settled(customCfwContext *ctx, cfw_lens *L) { (void)ctx; (void)L; }
uint8_t cfw_op_check(customCfwContext *ctx, cfw_lens *L, const uint8_t *p, uint8_t *reason) { (void)ctx; (void)L; (void)p; (void)reason; return 1; }
cfw_vector_work *cfw_vec_work(customCfwContext *ctx) { (void)ctx; return NULL; }
int cfw_vec_compile(cfw_vector_work *w, const uint8_t *src, uint32_t len) { (void)w; (void)src; (void)len; return 0; }
void cfw_vec_path_draw(customCfwContext *ctx, cfw_vector_work *w, const cfw_path *path, const int16_t *p, const cfw_rotation *t) { (void)ctx; (void)w; (void)path; (void)p; (void)t; }
void cfw_vec_shape_draw(customCfwContext *ctx, cfw_vector_work *w, const cfw_object *o) { (void)ctx; (void)w; (void)o; }
int cfw_vec_rotated(const cfw_rotation *t) { (void)t; return 0; }
void cfw_geom_draw(customCfwContext *ctx, uint8_t type, uint8_t width, const int16_t *params) { (void)ctx; (void)type; (void)width; (void)params; }

uint32_t cfw_op_len(const uint8_t *p, uint32_t avail) { (void)p; (void)avail; return 0; }
void cfw_anim_freeze_all(cfw_lens *L) { (void)L; }

#include "rle.c"
#include "scene.c"

static int fail;
static void expect(customCfwContext *ctx, int x, int y, int gray) {
    int got = ctx->lens[0].canvas[y * CFW_W + x] / 17;
    printf("  (%d,%d) expected %2d got %2d %s\n", x, y, gray, got, got == gray ? "ok" : "MISMATCH");
    if (got != gray) fail = 1;
}

int main(void) {
    static customCfwContext ctx;
    static uint8_t store[CFW_STORE_MAX_BYTES];
    static uint8_t canvas[CFW_W * CFW_H];
    ctx.store = store; ctx.store_bytes = CFW_STORE_MAX_BYTES; ctx.store_granules = STORE_GRANULES_MAX; ctx.lens[0].canvas = canvas;
    store_clear(&ctx);
    /* 3x2 image, pixels row 0: 1 2 3, row 1: 4 5 6 as six single-pixel RLE tokens [1<<4|c] */
    uint8_t img[] = { 3, 2, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    cfw_object o = {0};
    o.type = SH_IMAGE; o.p[0] = 10; o.p[1] = 20;
    uint8_t lut[16];
    puts("plain (ink 15)");
    o.color = 15; make_lut(o.color, lut);
    ctx.canvas_buf = canvas;
    if (!blit_image(&ctx, img, sizeof img, 10, 20, lut, 0)) { puts("  decode FAILED"); return 1; }
    expect(&ctx, 10, 20, 1); expect(&ctx, 11, 20, 2); expect(&ctx, 12, 20, 3);
    expect(&ctx, 10, 21, 4); expect(&ctx, 11, 21, 5); expect(&ctx, 12, 21, 6);
    puts("inverse, ink 15, transparent: source 0 skipped, 15-g otherwise");
    uint8_t img2[] = { 2, 1, 0x10, 0x1f };            /* pixels 0, 15 */
    memset(canvas, 7 * 17, sizeof canvas);
    make_lut(TEX_OPT_INVERSE | TEX_OPT_TRANSPARENT | 15, lut);
    blit_image(&ctx, img2, sizeof img2, 0, 0, lut, 1);
    expect(&ctx, 0, 0, 7); expect(&ctx, 1, 0, 0);
    puts("builtin font: 'I' has ink in its cell, ' ' has none");
    memset(canvas, 0, sizeof canvas);
    uint8_t text[] = "I ";
    draw_font_text(&ctx, font_builtin, FONT_BUILTIN_BYTES, 1, text, 2, 0, 0, (make_lut(15, lut), lut), 0);
    int ink = 0, blank = 0;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 12; x++) { ink += canvas[y * CFW_W + x] != 0; blank += canvas[y * CFW_W + 12 + x] != 0; }
    printf("  'I' ink pixels %d (%s), ' ' ink pixels %d (%s)\n", ink, ink > 10 ? "ok" : "MISMATCH", blank, blank == 0 ? "ok" : "MISMATCH");
    if (ink <= 10 || blank) fail = 1;
    return fail;
}
