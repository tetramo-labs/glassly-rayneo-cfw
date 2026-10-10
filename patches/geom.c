/* Geometry rasterizer for the 540x180 L8 canvas: the 20-byte shape records of
 * the object cache (types 1..13), ported from the G2 Glassly CFW shapes.c and
 * retargeted from packed 4bpp to one byte per pixel. The panel is 1-bit, so a
 * record whose gray is below CFW_GRAY_ON draws nothing and everything else
 * writes CFW_PANEL_ON; the caller decides.
 *
 * Integer math only (no floats, no 64-bit division). Circles, rings and
 * rounded corners are scan-converted per row from an integer square root;
 * polygons, wide lines and pie sectors go through one even-odd scanline filler
 * in 1/16 px (Q4) sampled at pixel centers; curves and arcs are flattened to
 * short segments; hairlines are Bresenham. Coordinates are clamped to +-1024.
 *
 *   1 LINE x1 y1 x2 y2 (width)      2 RECT x y w h r (width)   3 RECT_FILL x y w h r
 *   4 CIRCLE cx cy r (width)        5 CIRCLE_FILL cx cy r
 *   6 TRI / 7 TRI_FILL x1 y1 .. x3 y3   8 QUAD / 9 QUAD_FILL x1 y1 .. x4 y4
 *  10 BEZIER2 x0 y0 cx cy x1 y1    11 BEZIER3 x0 y0 c0x c0y c1x c1y x1 y1
 *  12 ARC cx cy r a0 a1 (width)    13 PIE cx cy r a0 a1    (degrees, 0 = +x, clockwise) */
#include "cfw.h"

#define GQ4           16
#define GCOORD_MAX    1023
#define GCOORD_MIN    (-1024)
#define GDIM_MAX      2047
#define GPOLY_MAX     8u
#define GBEZIER_SEGS  16u
#define GARC_MAX_SEGS 72u

static uint32_t g_isqrt(uint32_t v) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

static int32_t g_div_floor(int32_t num, int32_t den) {
    int32_t q = num / den;
    if ((num % den) != 0 && num < 0) q--;
    return q;
}

static int32_t g_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- pixel + span (L8) ----------------------------------------------------- */
static void g_pixel(customCfwContext *ctx, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= CFW_W || y >= CFW_H) return;
    ctx->canvas_buf[(uint32_t)y * CFW_W + (uint32_t)x] = ctx->draw_gray;
}

/* Inclusive [x0, x1] on row y; empty when x0 > x1. */
static void g_hspan(customCfwContext *ctx, int32_t x0, int32_t x1, int32_t y) {
    if (y < 0 || y >= CFW_H || x0 > x1 || x1 < 0 || x0 >= CFW_W) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= CFW_W) x1 = CFW_W - 1;
    uint8_t *row = ctx->canvas_buf + (uint32_t)y * CFW_W;
    for (int32_t i = x0; i <= x1; i++) row[i] = ctx->draw_gray;
}

/* ---- rectangles and circles ------------------------------------------------ */
static int32_t g_chord_half(int32_t rad16, int32_t dy16) {
    if (dy16 < 0) dy16 = -dy16;
    if (dy16 >= rad16) return -1;
    uint32_t rr = (uint32_t)rad16 * (uint32_t)rad16, dd = (uint32_t)dy16 * (uint32_t)dy16;
    return (int32_t)g_isqrt(rr - dd);
}

static int g_span_pixels(int32_t a16, int32_t b16, int32_t *first, int32_t *last) {
    *first = g_div_floor(a16 + 7, GQ4);
    *last = g_div_floor(b16 + 7, GQ4) - 1;
    return *first <= *last;
}

static int g_rrect_row(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, int32_t row, int32_t *l, int32_t *rt) {
    if (w <= 0 || h <= 0 || row < y || row >= y + h) return 0;
    int32_t maxr = (w < h ? w : h) / 2;
    if (rad > maxr) rad = maxr;
    if (rad < 0) rad = 0;
    *l = x;
    *rt = x + w - 1;
    if (rad == 0) return 1;
    int32_t dy16 = -1;
    if (row < y + rad) dy16 = (y + rad - row) * GQ4 - 8;
    else if (row >= y + h - rad) dy16 = (row - (y + h - rad)) * GQ4 + 8;
    if (dy16 < 0) return 1;
    int32_t half = g_chord_half(rad * GQ4, dy16);
    if (half < 0) return 0;
    int32_t first, last;
    if (!g_span_pixels(x * GQ4 + rad * GQ4 - half, (x + w) * GQ4 - rad * GQ4 + half, &first, &last)) return 0;
    *l = first;
    *rt = last;
    return 1;
}

/* Rounded rectangle: filled when wd <= 0, else a border of width wd. */
static void g_rrect(customCfwContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, int32_t wd) {
    if (w <= 0 || h <= 0) return;
    int inner = wd > 0 && w > 2 * wd && h > 2 * wd;
    int32_t y0 = y < 0 ? 0 : y, y1 = y + h;
    if (y1 > CFW_H) y1 = CFW_H;
    for (int32_t row = y0; row < y1; row++) {
        int32_t ol, orr, il, ir;
        if (!g_rrect_row(x, y, w, h, rad, row, &ol, &orr)) continue;
        if (inner && g_rrect_row(x + wd, y + wd, w - 2 * wd, h - 2 * wd, rad - wd, row, &il, &ir)) {
            g_hspan(ctx, ol, il - 1, row);
            g_hspan(ctx, ir + 1, orr, row);
        } else {
            g_hspan(ctx, ol, orr, row);
        }
    }
}

/* Circle centered on pixel (cx,cy), odd 2r+1 px diameter: filled when wd <= 0, else a ring. */
static void g_circle(customCfwContext *ctx, int32_t cx, int32_t cy, int32_t rad, int32_t wd) {
    if (rad < 0) return;
    int32_t rad16 = rad * GQ4 + 8;
    int32_t in16 = wd > 0 && rad > wd ? (rad - wd) * GQ4 + 8 : -1;
    int32_t cx16 = cx * GQ4 + 8;
    for (int32_t row = cy - rad; row <= cy + rad; row++) {
        if (row < 0 || row >= CFW_H) continue;
        int32_t dy16 = (cy - row) * GQ4;
        int32_t ho = g_chord_half(rad16, dy16);
        if (ho < 0) continue;
        int32_t of, ol;
        if (!g_span_pixels(cx16 - ho, cx16 + ho, &of, &ol)) continue;
        int32_t hi = in16 > 0 ? g_chord_half(in16, dy16) : -1;
        int32_t inf, inl;
        if (hi > 0 && g_span_pixels(cx16 - hi, cx16 + hi, &inf, &inl)) {
            g_hspan(ctx, of, inf - 1, row);
            g_hspan(ctx, inl + 1, ol, row);
        } else {
            g_hspan(ctx, of, ol, row);
        }
    }
}

/* ---- polygons (Q4 vertices, even-odd, center sampled) ---------------------- */
static void g_fill_poly_q4(customCfwContext *ctx, const int32_t *px, const int32_t *py, uint32_t n) {
    if (n < 3 || n > GPOLY_MAX) return;
    int32_t ymin = py[0], ymax = py[0];
    for (uint32_t i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    int32_t row0 = g_div_floor(ymin, GQ4), row1 = g_div_floor(ymax, GQ4);
    if (row0 < 0) row0 = 0;
    if (row1 >= CFW_H) row1 = CFW_H - 1;
    for (int32_t row = row0; row <= row1; row++) {
        int32_t yc = row * GQ4 + 8;
        int32_t xs[GPOLY_MAX];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t j = (i + 1 == n) ? 0 : i + 1;
            int32_t ax = px[i], ay = py[i], bx = px[j], by = py[j];
            if (ay == by) continue;
            if (ay > by) { int32_t t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
            if (yc < ay || yc >= by) continue;
            xs[k++] = ax + g_div_floor((yc - ay) * (bx - ax), by - ay);
        }
        for (uint32_t i = 1; i < k; i++) {
            int32_t v = xs[i];
            uint32_t j = i;
            while (j > 0 && xs[j - 1] > v) { xs[j] = xs[j - 1]; j--; }
            xs[j] = v;
        }
        for (uint32_t i = 0; i + 1 < k; i += 2) {
            int32_t first, last;
            if (g_span_pixels(xs[i], xs[i + 1], &first, &last)) g_hspan(ctx, first, last, row);
        }
    }
}

/* ---- lines ----------------------------------------------------------------- */
static void g_hairline(customCfwContext *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;
    for (;;) {
        g_pixel(ctx, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/* Line between Q4 points; widths >= 2 become a filled quad with square caps. */
static void g_line_q4(customCfwContext *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t wd) {
    if (wd <= 1) {
        g_hairline(ctx, g_div_floor(x0, GQ4), g_div_floor(y0, GQ4), g_div_floor(x1, GQ4), g_div_floor(y1, GQ4));
        return;
    }
    int32_t dx = x1 - x0, dy = y1 - y0;
    uint32_t len = g_isqrt((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
    int32_t hw = wd * 8;
    if (len == 0) {
        int32_t px[4] = { x0 - hw, x0 + hw, x0 + hw, x0 - hw };
        int32_t py[4] = { y0 - hw, y0 - hw, y0 + hw, y0 + hw };
        g_fill_poly_q4(ctx, px, py, 4);
        return;
    }
    int32_t ex = dx * hw / (int32_t)len, ey = dy * hw / (int32_t)len;
    int32_t nx = -ey, ny = ex;
    int32_t px[4] = { x0 - ex + nx, x1 + ex + nx, x1 + ex - nx, x0 - ex - nx };
    int32_t py[4] = { y0 - ey + ny, y1 + ey + ny, y1 + ey - ny, y0 - ey - ny };
    g_fill_poly_q4(ctx, px, py, 4);
}

static int32_t g_q4(int32_t v) { return v * GQ4 + 8; }

static void g_line(customCfwContext *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t wd) {
    g_line_q4(ctx, g_q4(x0), g_q4(y0), g_q4(x1), g_q4(y1), wd);
}

static void g_stroke_poly(customCfwContext *ctx, const int32_t *px, const int32_t *py, uint32_t n, int32_t wd) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1 == n) ? 0 : i + 1;
        g_line(ctx, px[i], py[i], px[j], py[j], wd);
    }
}

/* ---- curves ---------------------------------------------------------------- */
static void g_bezier2(customCfwContext *ctx, int32_t x0, int32_t y0, int32_t cx, int32_t cy, int32_t x1, int32_t y1, int32_t wd) {
    int32_t px = g_q4(x0), py = g_q4(y0);
    for (uint32_t i = 1; i <= GBEZIER_SEGS; i++) {
        int32_t u = (int32_t)(GBEZIER_SEGS - i), t = (int32_t)i;
        int32_t nx = (u * u * x0 + 2 * u * t * cx + t * t * x1) * GQ4 / 256 + 8;
        int32_t ny = (u * u * y0 + 2 * u * t * cy + t * t * y1) * GQ4 / 256 + 8;
        g_line_q4(ctx, px, py, nx, ny, wd);
        px = nx; py = ny;
    }
}

static void g_bezier3(customCfwContext *ctx, int32_t x0, int32_t y0, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                      int32_t x1, int32_t y1, int32_t wd) {
    int32_t px = g_q4(x0), py = g_q4(y0);
    for (uint32_t i = 1; i <= GBEZIER_SEGS; i++) {
        int32_t u = (int32_t)(GBEZIER_SEGS - i), t = (int32_t)i;
        int32_t uu = u * u, tt = t * t;
        int32_t nx = (uu * u * x0 + 3 * uu * t * ax + 3 * u * tt * bx + tt * t * x1) * GQ4 / 4096 + 8;
        int32_t ny = (uu * u * y0 + 3 * uu * t * ay + 3 * u * tt * by + tt * t * y1) * GQ4 / 4096 + 8;
        g_line_q4(ctx, px, py, nx, ny, wd);
        px = nx; py = ny;
    }
}

/* sin(deg) * 32768 for 0..90 degrees. */
static const uint16_t g_sin_q15[91] = {
    0, 572, 1144, 1715, 2286, 2856, 3425, 3993, 4560, 5126, 5690, 6252, 6813,
    7371, 7927, 8481, 9032, 9580, 10126, 10668, 11207, 11743, 12275, 12803, 13328, 13848,
    14365, 14876, 15384, 15886, 16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174,
    20622, 21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730, 25102, 25466,
    25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088, 28378, 28660, 28932, 29197, 29452,
    29698, 29935, 30163, 30382, 30592, 30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928,
    32052, 32166, 32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763, 32767,
};

static int32_t g_sin_deg(int32_t a) {
    a %= 360;
    if (a < 0) a += 360;
    int32_t sign = 1;
    if (a >= 180) { a -= 180; sign = -1; }
    if (a > 90) a = 180 - a;
    return sign * (int32_t)g_sin_q15[a];
}

static int32_t g_cos_deg(int32_t a) { return g_sin_deg(a + 90); }

static void g_arc_point(int32_t cx, int32_t cy, int32_t rad, int32_t deg, int32_t *x, int32_t *y) {
    int32_t r16 = rad * GQ4;
    *x = g_q4(cx) + ((r16 * g_cos_deg(deg) + 16384) >> 15);
    *y = g_q4(cy) + ((r16 * g_sin_deg(deg) + 16384) >> 15);
}

static uint32_t g_arc_segments(int32_t span) {
    uint32_t n = (uint32_t)(span < 0 ? -span : span) / 6u + 1u;
    return n > GARC_MAX_SEGS ? GARC_MAX_SEGS : n;
}

static void g_arc(customCfwContext *ctx, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1, int32_t wd) {
    if (rad <= 0) return;
    int32_t span = g_clamp(a1 - a0, -360, 360);
    uint32_t n = g_arc_segments(span);
    int32_t px, py;
    g_arc_point(cx, cy, rad, a0, &px, &py);
    for (uint32_t i = 1; i <= n; i++) {
        int32_t nx, ny;
        g_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &nx, &ny);
        g_line_q4(ctx, px, py, nx, ny, wd);
        px = nx; py = ny;
    }
}

static void g_pie(customCfwContext *ctx, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1) {
    if (rad <= 0) return;
    int32_t span = g_clamp(a1 - a0, -360, 360);
    if (span == 0) return;
    uint32_t n = g_arc_segments(span);
    int32_t px[3], py[3];
    px[0] = g_q4(cx);
    py[0] = g_q4(cy);
    g_arc_point(cx, cy, rad, a0, &px[1], &py[1]);
    for (uint32_t i = 1; i <= n; i++) {
        g_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &px[2], &py[2]);
        g_fill_poly_q4(ctx, px, py, 3);
        px[1] = px[2];
        py[1] = py[2];
    }
}

/* ---- dispatch -------------------------------------------------------------- */
/* Draw one geometry record (type 1..13) with its eight i16 parameters and
 * stroke width; the caller has already decided the gray is "on". */
void cfw_geom_draw(customCfwContext *ctx, uint8_t type, uint8_t width, const int16_t *params) {
    if (!ctx->canvas_buf) return;
    int32_t p[8];
    for (uint32_t i = 0; i < 8; i++) p[i] = g_clamp(params[i], GCOORD_MIN, GCOORD_MAX);
    int32_t wd = width;
    switch (type) {
    case SH_LINE:
        g_line(ctx, p[0], p[1], p[2], p[3], wd);
        break;
    case SH_RECT:
    case SH_RECT_FILL:
        g_rrect(ctx, p[0], p[1], g_clamp(p[2], 0, GDIM_MAX), g_clamp(p[3], 0, GDIM_MAX), g_clamp(p[4], 0, GDIM_MAX),
                type == SH_RECT ? (wd < 1 ? 1 : wd) : 0);
        break;
    case SH_CIRCLE:
    case SH_CIRCLE_FILL:
        g_circle(ctx, p[0], p[1], g_clamp(p[2], 0, GCOORD_MAX), type == SH_CIRCLE ? (wd < 1 ? 1 : wd) : 0);
        break;
    case SH_TRI:
    case SH_QUAD: {
        uint32_t n = type == SH_TRI ? 3u : 4u;
        int32_t px[4] = { p[0], p[2], p[4], p[6] };
        int32_t py[4] = { p[1], p[3], p[5], p[7] };
        g_stroke_poly(ctx, px, py, n, wd);
        break;
    }
    case SH_TRI_FILL:
    case SH_QUAD_FILL: {
        uint32_t n = type == SH_TRI_FILL ? 3u : 4u;
        int32_t px[4], py[4];
        for (uint32_t i = 0; i < n; i++) { px[i] = g_q4(p[2 * i]); py[i] = g_q4(p[2 * i + 1]); }
        g_fill_poly_q4(ctx, px, py, n);
        break;
    }
    case SH_BEZIER2:
        g_bezier2(ctx, p[0], p[1], p[2], p[3], p[4], p[5], wd);
        break;
    case SH_BEZIER3:
        g_bezier3(ctx, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], wd);
        break;
    case SH_ARC:
        g_arc(ctx, p[0], p[1], g_clamp(p[2], 0, GCOORD_MAX), p[3], p[4], wd);
        break;
    case SH_PIE:
        g_pie(ctx, p[0], p[1], g_clamp(p[2], 0, GCOORD_MAX), p[3], p[4]);
        break;
    default:
        break;
    }
}
