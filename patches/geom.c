/* Geometry rasterizer for the 540x220 L8 lens canvases: the 20-byte shape
 * records of the object cache (types 1..13), ported from the G2 Glassly CFW
 * shapes.c and retargeted from a 1-bit panel to 16 gray levels with
 * anti-aliased edges. Every primitive is scan-converted per pixel row at four
 * sub-rows, each sub-row's spans in 1/16 px (Q4), into a coverage row
 * (0..64 per pixel) that is then blended over the canvas with the record's
 * gray: edges get up to 4x16 shades, axis-aligned integer rectangles stay
 * crisp. Integer math only (no floats, no 64-bit division).
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
#define GSUB          4            /* sub-rows per pixel row */
#define GCOV_FULL     (GSUB * GQ4) /* coverage of a fully covered pixel */

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

/* ---- coverage row ----------------------------------------------------------- */
/* One pixel row being rasterized: per-pixel coverage in 1/16 of a sub-row,
 * accumulated by the sub-row spans, then blended into the canvas once. */
typedef struct {
    uint8_t cov[CFW_W];
    int32_t lo, hi;                /* pixel range touched (lo > hi: none) */
} g_row;

static void g_row_begin(g_row *r) { r->lo = CFW_W; r->hi = -1; }

/* Add the Q4 span [a, b) of one sub-row (weight +16 per fully covered pixel). */
static void g_row_add(g_row *r, int32_t a, int32_t b, int32_t sign) {
    if (a < 0) a = 0;
    if (b > CFW_W * GQ4) b = CFW_W * GQ4;
    if (a >= b) return;
    int32_t first = a >> 4, last = (b - 1) >> 4;
    /* extend the touched range, zeroing newly covered pixels */
    if (r->hi < r->lo) { for (int32_t i = first; i <= last; i++) r->cov[i] = 0; r->lo = first; r->hi = last; }
    else {
        for (int32_t i = first; i < r->lo; i++) r->cov[i] = 0;
        for (int32_t i = r->hi + 1; i <= last; i++) r->cov[i] = 0;
        if (first < r->lo) r->lo = first;
        if (last > r->hi) r->hi = last;
    }
    if (first == last) {
        int32_t c = (int32_t)r->cov[first] + sign * (b - a);
        r->cov[first] = (uint8_t)g_clamp(c, 0, 255);
        return;
    }
    int32_t c = (int32_t)r->cov[first] + sign * (GQ4 - (a & 15));
    r->cov[first] = (uint8_t)g_clamp(c, 0, 255);
    for (int32_t i = first + 1; i < last; i++) {
        c = (int32_t)r->cov[i] + sign * GQ4;
        r->cov[i] = (uint8_t)g_clamp(c, 0, 255);
    }
    c = (int32_t)r->cov[last] + sign * (((b - 1) & 15) + 1);
    r->cov[last] = (uint8_t)g_clamp(c, 0, 255);
}

/* Blend the row into the canvas: dst += (gray - dst) * cov / 64. */
static void g_row_flush(customCfwContext *ctx, g_row *r, int32_t y) {
    if (y < 0 || y >= CFW_H || r->hi < r->lo) return;
    uint8_t *row = ctx->canvas_buf + (uint32_t)y * CFW_W;
    int32_t gray = ctx->draw_gray;
    for (int32_t i = r->lo; i <= r->hi; i++) {
        int32_t c = r->cov[i];
        if (!c) continue;
        if (c >= GCOV_FULL) { row[i] = (uint8_t)gray; continue; }
        int32_t d = row[i];
        row[i] = (uint8_t)(d + ((gray - d) * c + GCOV_FULL / 2) / GCOV_FULL);
    }
}

/* Q4 y of sub-row s (0..3) of pixel row `row`. */
static int32_t g_sub_y(int32_t row, int32_t s) { return row * GQ4 + 2 + 4 * s; }

/* ---- circles and rounded rectangles ------------------------------------------- */
static int32_t g_chord_half(int32_t rad16, int32_t dy16) {
    if (dy16 < 0) dy16 = -dy16;
    if (dy16 >= rad16) return -1;
    uint32_t rr = (uint32_t)rad16 * (uint32_t)rad16, dd = (uint32_t)dy16 * (uint32_t)dy16;
    return (int32_t)g_isqrt(rr - dd);
}

/* Q4 horizontal extent [l, r) of a rounded rect at Q4 y; 0 outside. */
static int g_rrect_span(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, int32_t y16, int32_t *l, int32_t *r) {
    if (w <= 0 || h <= 0 || y16 < y * GQ4 || y16 >= (y + h) * GQ4) return 0;
    int32_t maxr = (w < h ? w : h) / 2;
    if (rad > maxr) rad = maxr;
    if (rad < 0) rad = 0;
    *l = x * GQ4;
    *r = (x + w) * GQ4;
    if (rad == 0) return 1;
    int32_t dy16 = -1;
    if (y16 < (y + rad) * GQ4) dy16 = (y + rad) * GQ4 - y16;
    else if (y16 >= (y + h - rad) * GQ4) dy16 = y16 - (y + h - rad) * GQ4;
    if (dy16 < 0) return 1;
    int32_t half = g_chord_half(rad * GQ4, dy16);
    if (half < 0) return 0;
    *l = (x + rad) * GQ4 - half;
    *r = (x + w - rad) * GQ4 + half;
    return *l < *r;
}

/* Write gray over the pixel span [a, b) of row y, clipped to the canvas. */
static void g_hspan(customCfwContext *ctx, int32_t y, int32_t a, int32_t b) {
    if (y < 0 || y >= CFW_H) return;
    if (a < 0) a = 0;
    if (b > CFW_W) b = CFW_W;
    if (a >= b) return;
    memset(ctx->canvas_buf + (uint32_t)y * CFW_W + (uint32_t)a, ctx->draw_gray, (size_t)(b - a));
}

/* Rounded rectangle: filled when wd <= 0, else a border of width wd. Rows
 * between the corner arcs are straight runs at integer coordinates, so the
 * coverage pass would only ever find full pixels there: they are written
 * directly (a full-canvas card border used to cost more than eight lines
 * of text). The corner rows keep the anti-aliased pass. */
static void g_rrect(customCfwContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h, int32_t rad, int32_t wd) {
    if (w <= 0 || h <= 0) return;
    int inner = wd > 0 && w > 2 * wd && h > 2 * wd;
    int32_t y0 = y < 0 ? 0 : y, y1 = y + h;
    if (y1 > CFW_H) y1 = CFW_H;
    int32_t maxr = (w < h ? w : h) / 2;
    int32_t crad = rad > maxr ? maxr : (rad < 0 ? 0 : rad);
    int32_t straight0 = y + crad, straight1 = y + h - crad;   /* rows [straight0, straight1) have no corner arc */
    g_row r;
    for (int32_t row = y0; row < y1; row++) {
        if (row >= straight0 && row < straight1) {
            if (!inner) { g_hspan(ctx, row, x, x + w); continue; }
            if (row < y + wd || row >= y + h - wd) { g_hspan(ctx, row, x, x + w); continue; }   /* top / bottom band */
            g_hspan(ctx, row, x, x + wd);
            g_hspan(ctx, row, x + w - wd, x + w);
            continue;
        }
        g_row_begin(&r);
        for (int32_t s = 0; s < GSUB; s++) {
            int32_t ys = g_sub_y(row, s), ol, orr, il, ir;
            if (!g_rrect_span(x, y, w, h, rad, ys, &ol, &orr)) continue;
            g_row_add(&r, ol, orr, 1);
            if (inner && g_rrect_span(x + wd, y + wd, w - 2 * wd, h - 2 * wd, rad - wd, ys, &il, &ir)) g_row_add(&r, il, ir, -1);
        }
        g_row_flush(ctx, &r, row);
    }
}

/* Circle centered on pixel (cx,cy), odd 2r+1 px diameter: filled when wd <= 0, else a ring. */
static void g_circle(customCfwContext *ctx, int32_t cx, int32_t cy, int32_t rad, int32_t wd) {
    if (rad < 0) return;
    int32_t rad16 = rad * GQ4 + 8;
    int32_t in16 = wd > 0 && rad > wd ? (rad - wd) * GQ4 + 8 : -1;
    int32_t cx16 = cx * GQ4 + 8, cy16 = cy * GQ4 + 8;
    g_row r;
    for (int32_t row = cy - rad - 1; row <= cy + rad + 1; row++) {
        if (row < 0 || row >= CFW_H) continue;
        g_row_begin(&r);
        for (int32_t s = 0; s < GSUB; s++) {
            int32_t dy16 = cy16 - g_sub_y(row, s);
            int32_t ho = g_chord_half(rad16, dy16);
            if (ho < 0) continue;
            g_row_add(&r, cx16 - ho, cx16 + ho, 1);
            int32_t hi = in16 > 0 ? g_chord_half(in16, dy16) : -1;
            if (hi > 0) g_row_add(&r, cx16 - hi, cx16 + hi, -1);
        }
        g_row_flush(ctx, &r, row);
    }
}

/* ---- polygons (Q4 vertices, even-odd, four sub-rows per pixel row) ---------- */
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
    g_row r;
    for (int32_t row = row0; row <= row1; row++) {
        g_row_begin(&r);
        for (int32_t s = 0; s < GSUB; s++) {
            int32_t yc = g_sub_y(row, s);
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
            for (uint32_t i = 0; i + 1 < k; i += 2) g_row_add(&r, xs[i], xs[i + 1], 1);
        }
        g_row_flush(ctx, &r, row);
    }
}

/* ---- lines ----------------------------------------------------------------- */
/* Line between Q4 points as a filled quad with square caps; width 1 for hairlines. */
static void g_line_q4(customCfwContext *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t wd) {
    if (wd < 1) wd = 1;
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
    /* a hairline keeps its endpoints (no caps), wider strokes get square caps */
    int32_t cx = wd > 1 ? ex : 0, cy = wd > 1 ? ey : 0;
    int32_t px[4] = { x0 - cx + nx, x1 + cx + nx, x1 + cx - nx, x0 - cx - nx };
    int32_t py[4] = { y0 - cy + ny, y1 + cy + ny, y1 + cy - ny, y0 - cy - ny };
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

/* Pie sector: one polygon (centre + rim points), so the seams between the
 * flattening segments blend instead of double-covering. */
static void g_pie(customCfwContext *ctx, int32_t cx, int32_t cy, int32_t rad, int32_t a0, int32_t a1) {
    if (rad <= 0) return;
    int32_t span = g_clamp(a1 - a0, -360, 360);
    if (span == 0) return;
    uint32_t n = g_arc_segments(span);
    /* GPOLY_MAX vertices per polygon: draw the sector as fans of up to 6 rim points */
    int32_t px[GPOLY_MAX], py[GPOLY_MAX];
    px[0] = g_q4(cx);
    py[0] = g_q4(cy);
    uint32_t i = 0;
    while (i < n) {
        uint32_t k = 1;
        g_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &px[k], &py[k]);
        k++;
        while (i < n && k < GPOLY_MAX) {
            i++;
            g_arc_point(cx, cy, rad, a0 + span * (int32_t)i / (int32_t)n, &px[k], &py[k]);
            k++;
        }
        g_fill_poly_q4(ctx, px, py, k);
    }
}

/* ---- dispatch -------------------------------------------------------------- */
/* Draw one geometry record (type 1..13) with its eight i16 parameters and
 * stroke width in ctx->draw_gray. */
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
