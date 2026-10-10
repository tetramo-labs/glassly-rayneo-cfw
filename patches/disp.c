/* Revision 5 display probe (docs/RAW-LINK-PLAN.md, revision 5 plan item 1).
 *
 * Reports the stock LVGL fbdev driver's geometry and exercises the two ways of
 * presenting a frame without LVGL drawing it:
 *   fb path   write the mmapped framebuffer and issue the ioctls flush_cb issues
 *   dual path fill the LCDC's per-eye 4-bit buffers and call the dual update
 * plus the refr_pass flag that makes flush_cb a no-op (the stock driver's own
 * switch, lv_rayneo_fbdev_set_refr_pass). Everything runs on the LVGL thread
 * from the control channel; the Mac harness triggers it and the person wearing
 * the glasses reports what each pattern looks like.
 *
 * Offsets below are the lv_rayneo_fbdev driver data as lv_rayneo_fbdev_set_file
 * and flush_cb lay it out in Strix OS 1.0.4.12 (nuttx_ap.bin): a NuttX
 * fb_videoinfo_s at +4, fb_planeinfo_s at +0xc, the mmap at +0x28.
 */
#include "cfw.h"

#define FBD_FD         0x00u
#define FBD_FMT        0x04u   /* fb_videoinfo_s.fmt */
#define FBD_XRES       0x06u
#define FBD_YRES       0x08u
#define FBD_NPLANES    0x0au
#define FBD_PINFO      0x0cu   /* fb_planeinfo_s: fbmem, fblen, stride u16, display u8, bpp u8, xres_v, yres_v, xoff, yoff */
#define FBD_FBMEM      0x0cu
#define FBD_FBLEN      0x10u
#define FBD_STRIDE     0x14u
#define FBD_DISPLAY    0x16u
#define FBD_BPP        0x17u
#define FBD_XRES_V     0x18u
#define FBD_YRES_V     0x1cu
#define FBD_XOFF       0x20u
#define FBD_YOFF       0x24u
#define FBD_MAP        0x28u
#define FBD_BUF2       0x2cu
#define FBD_YOFF2      0x30u
#define FBD_CF         0xfcu
#define FBD_W          0x100u
#define FBD_H          0x104u
#define FBD_DSTRIDE    0x108u
#define FBD_REFR_PASS  0x238u
/* fb driver ioctls flush_cb issues, in this order */
#define FBIO_AREA      0xfau   /* {x u16, y u16, w u16, h u16}, y in virtual rows */
#define FBIO_DEPTH     0x106u  /* 0x8c-byte depth layer group bounds; zeroed = none */
#define FBIO_PAN       0x2818u /* FBIOPAN_DISPLAY(fb_planeinfo_s) */
#define FBIO_COMMIT    0x107u
#define DEPTH_BOUNDS_LEN 0x8cu
/* rayneo_lcdc per-eye 4-bit buffers: rayneo_lcdc_layer_dual_update_full(left)
 * pairs G4_LEFT with G4_RIGHT, 302-byte rows, 279 rows + 1 trailing row. */
#define G4_LEFT        0x202802c0u
#define G4_RIGHT       0x2026b840u
#define G4_STRIDE      0x12eu
#define G4_ROWS        280u
#define G4_SIZE        0x14a50u
#define G4_PX_BYTES    270u    /* 540 px at 4 bpp */
#define G4_MARGIN      16u     /* rayneo_lcdc_visible_row_ptr: row + 16 - parallax byte shift */
#define LCDC_STATE     0x1832d4f8u /* rayneo_lcdc state; +0xb5..+0xb8 col offsets */
#define LCDC_FRAME_PENDING 0x1832d5bau /* byte the stock sets (stlb) before waiting for frame done */
#define LCDC_KIND      0xb4u   /* last update kind: 0 full 1 partial 2 dual full 3 dual partial */
#define LCDC_COMPOSED  0xa2u   /* dual partial takes the pair (NULL) instead of 0x201e1fc0 */

static uint8_t *disp_driver(void **disp_out) {
    void *disp = fw_display_default();
    if (disp_out) *disp_out = disp;
    return disp ? (uint8_t *)fw_display_driver_data(disp) : 0;
}

static uint32_t ld32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Framebuffer path: a 16-step horizontal gray ramp with a white block at
 * x 200..259, y 60..119, into the first buffer, then flush_cb's ioctls. */
static int disp_fb_pattern(uint8_t *d, int touch_bounds) {
    uint8_t *map = (uint8_t *)ld32(d + FBD_MAP);
    uint32_t w = ld32(d + FBD_W), h = ld32(d + FBD_H), stride = ld32(d + FBD_DSTRIDE);
    uint8_t bpp = d[FBD_BPP];
    int fd = (int)ld32(d + FBD_FD);
    if (!map || !w || !h || fd < 0) return -1;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = map + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t level = (x * 16u) / w;                 /* 0..15 */
            uint8_t g = (uint8_t)(level * 17u);
            if (x >= 200 && x < 260 && y >= 60 && y < 120) g = 255;
            if (bpp == 16) {
                uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
            } else if (bpp == 32) {
                row[x * 4] = g; row[x * 4 + 1] = g; row[x * 4 + 2] = g; row[x * 4 + 3] = 0xff;
            } else {
                row[x] = g;
            }
        }
    }
    uint16_t area[4] = {0, 0, (uint16_t)w, (uint16_t)h};
    uint8_t bounds[DEPTH_BOUNDS_LEN];
    cfw_zero(bounds, sizeof(bounds));
    (void)fw_ioctl(fd, FBIO_AREA, area);
    if (touch_bounds) (void)fw_ioctl(fd, FBIO_DEPTH, bounds);
    cfw_put32(d + FBD_YOFF, 0);
    int rc = fw_ioctl(fd, FBIO_PAN, d + FBD_PINFO);
    (void)fw_ioctl(fd, FBIO_COMMIT, 0);
    return rc;
}

/* Dual path, diagnostic per-eye images (540 px × 280 rows each, 4 bpp):
 *   dark gray ground, a 4 px white frame around the whole buffer (shows the
 *   visible window and any clipping), a ruler down the left edge (a tick every
 *   20 rows, each 8 px longer than the last: tells which rows are visible and
 *   whether anything scrolls), a 16-step gray strip at rows 230..249.
 *   Left eye: white block x 40..119 y 40..99 and a diagonal from the top-left
 *   corner to the bottom-right. Right eye: block x 420..499 and the opposite
 *   diagonal. Each eye identifies itself. */
static void g4_set(uint8_t *buf, uint32_t x, uint32_t y, uint8_t level) {
    if (x >= 540 || y >= G4_ROWS) return;
    uint8_t *b = buf + y * G4_STRIDE + G4_MARGIN + (x >> 1);
    if (x & 1) *b = (uint8_t)((*b & 0xf0) | level);          /* right pixel: low nibble */
    else *b = (uint8_t)((*b & 0x0f) | (level << 4));         /* left pixel: high nibble */
}

static void g4_rect(uint8_t *buf, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint8_t level) {
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) g4_set(buf, x, y, level);
}

static void disp_fill_dual_pattern(void) {
    uint8_t *eye[2] = {(uint8_t *)G4_LEFT, (uint8_t *)G4_RIGHT};
    for (unsigned e = 0; e < 2; e++) {
        uint8_t *buf = eye[e];
        cfw_zero(buf, G4_SIZE);
        g4_rect(buf, 0, 0, 540, G4_ROWS, 3);                       /* ground */
        g4_rect(buf, 0, 0, 540, 4, 15); g4_rect(buf, 0, G4_ROWS - 4, 540, 4, 15);
        g4_rect(buf, 0, 0, 4, G4_ROWS, 15); g4_rect(buf, 536, 0, 4, G4_ROWS, 15);
        for (uint32_t y = 20; y < G4_ROWS; y += 20)                  /* ruler */
            g4_rect(buf, 8, y, 8 + (y / 20) * 8, 2, 15);
        for (uint32_t i = 0; i < 16; i++)                            /* gray strip */
            g4_rect(buf, 150 + i * 15, 230, 15, 20, (uint8_t)i);
        if (e == 0) {
            g4_rect(buf, 40, 40, 80, 60, 15);
            for (uint32_t t = 0; t < 540; t++) { g4_set(buf, t, (t * G4_ROWS) / 540, 15); g4_set(buf, t, (t * G4_ROWS) / 540 + 1, 15); }
        } else {
            g4_rect(buf, 420, 40, 80, 60, 15);
            for (uint32_t t = 0; t < 540; t++) { g4_set(buf, 539 - t, (t * G4_ROWS) / 540, 15); g4_set(buf, 539 - t, (t * G4_ROWS) / 540 + 1, 15); }
        }
    }
    fw_lcdc_cache_sync((void *)G4_LEFT, G4_SIZE);
    fw_lcdc_cache_sync((void *)G4_RIGHT, G4_SIZE);
}

static int disp_dual_pattern(void) {
    disp_fill_dual_pattern();
    fw_lcdc_dual_update_full((void *)G4_LEFT);
    return 0;
}

/* L8 lens canvases -> the LCDC pair (4 bpp, HIGH nibble = left pixel, as the
 * stock rgb565_to_g4_align_mve packs: `(p0 >> 3) & 0xf0 | p1 >> 7 & 0xf`): a lens
 * without a canvas shows the other one's, so a failed allocation degrades to
 * flat rather than one blind eye. */
static void disp_compose_scene(customCfwContext *ctx) {
    uint8_t *eye[2] = {(uint8_t *)G4_LEFT, (uint8_t *)G4_RIGHT};
    for (unsigned e = 0; e < 2; e++) {
        const uint8_t *src = ctx->lens[e].canvas ? ctx->lens[e].canvas : ctx->lens[e ^ 1].canvas;
        uint8_t *buf = eye[e];
        for (uint32_t y = 0; y < G4_ROWS; y++) {
            uint8_t *row = buf + y * G4_STRIDE;
            if (y >= CFW_H || !src) { cfw_zero(row, G4_STRIDE); continue; }
            const uint8_t *s = src + y * CFW_W;
            cfw_zero(row, G4_MARGIN);
            uint8_t *d = row + G4_MARGIN;
            for (uint32_t x = 0; x < CFW_W; x += 2) {
                *d++ = (uint8_t)((s[x] & 0xf0) | (s[x + 1] >> 4));
            }
            cfw_zero(row + G4_MARGIN + G4_PX_BYTES, G4_STRIDE - G4_MARGIN - G4_PX_BYTES);
        }
    }
    fw_lcdc_cache_sync((void *)G4_LEFT, G4_SIZE);
    fw_lcdc_cache_sync((void *)G4_RIGHT, G4_SIZE);
}

/* UI thread: park (1) or resume (0) the stock LVGL flush. */
void cfw_disp_pass(int on) {
    uint8_t *d = disp_driver(0);
    if (d) d[FBD_REFR_PASS] = on ? 1 : 0;
}

/* UI thread: ask the display thread for a present. The pan request is what
 * flush_cb issues (minus the pixels): the display thread wakes, runs
 * rayneo_pandisp_exec and reaches cfw_lcdc_hook, which presents our buffers.
 * One request in flight at a time; the page timer re-arms a lost one. */
void cfw_page_present(customCfwContext *ctx) {
    ctx->disp_dirty = 1;
    if (!ctx->page_open || !ctx->disp_own || ctx->disp_pan_pending) return;
    if (!fw_fb_get_power()) return;                  /* panel off: the display thread would skip it anyway */
    uint8_t *d = disp_driver(0);
    if (!d) return;
    int fd = (int)ld32(d + FBD_FD);
    if (fd < 0) return;
    ctx->disp_pan_pending = 1;
    ctx->disp_pan_tick = fw_tick();
    (void)fw_ioctl(fd, FBIO_PAN, d + FBD_PINFO);
    (void)fw_ioctl(fd, FBIO_COMMIT, 0);
}

/* Display thread, in place of both rayneo_lcdc_layer_update calls inside
 * rayneo_pandisp_exec (build.py LCDC_SITES). While the module holds a frame
 * the stock conversion/compose is skipped and the module's eye buffers go
 * out as they are; otherwise the stock update runs unchanged. */
void cfw_lcdc_hook(void *src, void *area, uint32_t c, uint32_t flag, void *depth) {
    customCfwContext *ctx = cfw_ctx_peek();
    if (ctx && ctx->disp_own) {
        /* compose here, on the display thread, so no stock compose can be in
         * flight on the same buffers (the LVGL-thread fill glitched the first
         * frames on hardware) */
        if (ctx->disp_dirty) {
            ctx->disp_dirty = 0;
            if (ctx->disp_pattern) disp_fill_dual_pattern(); else disp_compose_scene(ctx);
        }
        /* The stock never shows the pair through the full dual update (that
         * one drifted a row per frame on hardware); rayneo_lcdc_update_self
         * re-presents the current buffers the stock way: with kind 3 and the
         * compose flag set it runs dual_update_partial(pair, 220-row window at
         * the pan y offset) and the frame-done wait. */
        volatile uint8_t *lcdc = (volatile uint8_t *)LCDC_STATE;
        lcdc[LCDC_KIND] = 3;
        lcdc[LCDC_COMPOSED] = 1;
        fw_lcdc_update_self();
        ctx->disp_pan_pending = 0;
        return;
    }
    fw_lcdc_layer_update(src, area, c, flag, depth);
}

/* DISP [19][mode][rc i8][disp u32][drv u32][fd i32][fmt u8][xres u16][yres u16][nplanes u8]
 *      [fbmem u32][fblen u32][stride u16][display u8][bpp u8][xres_v u32][yres_v u32][xoff u32][yoff u32]
 *      [map u32][buf2 u32][yoff2 u32][cf u8][w u32][h u32][dstride u32][refr_pass u8]
 *      [left_parallax i32][right_parallax i32][depth_mode i32][disp_mode i32][lcdc b5 b6 b7 b8] */
void cfw_display_probe(customCfwContext *ctx, uint8_t mode) {
    void *disp = 0;
    uint8_t *d = disp_driver(&disp);
    int rc = 0;
    if (!d) {
        rc = -2;
    } else {
        switch (mode) {
        case DISP_REPORT: break;
        case DISP_FB_PATTERN:   rc = ctx->page_open ? disp_fb_pattern(d, 1) : -3; break;
        case DISP_FB_KEEP_BOUNDS: rc = ctx->page_open ? disp_fb_pattern(d, 0) : -3; break;
        case DISP_DUAL_PATTERN: rc = ctx->page_open ? disp_dual_pattern() : -3; break;
        case DISP_OWN:                 /* the diagnostic images instead of the scenes */
            if (!ctx->page_open) { rc = -3; break; }
            ctx->disp_pattern = 1;
            ctx->disp_own = 1;
            cfw_page_present(ctx);
            break;
        case DISP_RELEASE:             /* back to the scenes */
            ctx->disp_pattern = 0;
            cfw_page_present(ctx);
            break;
        case DISP_PASS_ON:  d[FBD_REFR_PASS] = 1; break;
        case DISP_PASS_OFF: d[FBD_REFR_PASS] = 0; break;
        case DISP_SCREEN_OFF: fw_screen_force_off(); break;   /* screen_power_off is refused while a wakelock is held */
        default: rc = -4; break;
        }
    }
    uint8_t b[112];
    uint8_t *p = b;
    *p++ = RPL_DISP;
    *p++ = mode;
    *p++ = (uint8_t)rc;
    cfw_put32(p, (uint32_t)(uintptr_t)disp); p += 4;
    cfw_put32(p, (uint32_t)(uintptr_t)d); p += 4;
    if (d) {
        cfw_put32(p, ld32(d + FBD_FD)); p += 4;
        *p++ = d[FBD_FMT];
        cfw_put16(p, ld16(d + FBD_XRES)); p += 2;
        cfw_put16(p, ld16(d + FBD_YRES)); p += 2;
        *p++ = d[FBD_NPLANES];
        cfw_put32(p, ld32(d + FBD_FBMEM)); p += 4;
        cfw_put32(p, ld32(d + FBD_FBLEN)); p += 4;
        cfw_put16(p, ld16(d + FBD_STRIDE)); p += 2;
        *p++ = d[FBD_DISPLAY];
        *p++ = d[FBD_BPP];
        cfw_put32(p, ld32(d + FBD_XRES_V)); p += 4;
        cfw_put32(p, ld32(d + FBD_YRES_V)); p += 4;
        cfw_put32(p, ld32(d + FBD_XOFF)); p += 4;
        cfw_put32(p, ld32(d + FBD_YOFF)); p += 4;
        cfw_put32(p, ld32(d + FBD_MAP)); p += 4;
        cfw_put32(p, ld32(d + FBD_BUF2)); p += 4;
        cfw_put32(p, ld32(d + FBD_YOFF2)); p += 4;
        *p++ = d[FBD_CF];
        cfw_put32(p, ld32(d + FBD_W)); p += 4;
        cfw_put32(p, ld32(d + FBD_H)); p += 4;
        cfw_put32(p, ld32(d + FBD_DSTRIDE)); p += 4;
        *p++ = d[FBD_REFR_PASS];
    } else {
        cfw_zero(p, 69); p += 69;
    }
    cfw_put32(p, (uint32_t)fw_fb_left_parallax()); p += 4;
    cfw_put32(p, (uint32_t)fw_fb_right_parallax()); p += 4;
    cfw_put32(p, (uint32_t)fw_fb_depth_mode()); p += 4;
    cfw_put32(p, (uint32_t)fw_fb_disp_mode()); p += 4;
    const uint8_t *lcdc = (const uint8_t *)LCDC_STATE;
    *p++ = lcdc[0xb5]; *p++ = lcdc[0xb6]; *p++ = lcdc[0xb7]; *p++ = lcdc[0xb8];
    /* page / wake state (revision 6): fb power, page open, own, dirty, pan pending, takeover, screen cb, screen on */
    *p++ = (uint8_t)(fw_fb_get_power() != 0);
    *p++ = ctx->page_open; *p++ = ctx->disp_own; *p++ = ctx->disp_dirty; *p++ = ctx->disp_pan_pending;
    *p++ = ctx->wake_takeover; *p++ = ctx->screen_cb_registered;
    *p++ = (uint8_t)(fw_display_is_screen_on() != 0);
    cfw_uplink_push(ctx, b, (uint8_t)(p - b));
}
