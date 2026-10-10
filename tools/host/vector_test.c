/* Host check of vector.c + geom.c on a 540x220 L8 canvas: compiles a path
 * (hex command bytes, the wire format of PATH records), draws it, and
 * compares pixel probes; or rotates a geometry record. Writes a PGM when
 * VECTOR_PGM names a file. Built by tools/host/run_vector_tests.sh.
 *
 *   vector_test path <rule 0|1> <scaleQ8> <x> <y> <angleDeg*256> <px> <py> <hex> [x,y,gray]...
 *   vector_test shape <type> <width> <p0..p7 (8 ints)> <angleDeg*256> <px> <py> [x,y,gray]...
 *
 * A probe passes when the canvas gray (0..15 after /17) matches; exit 1 on
 * the first mismatch, printing every probe. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfw.h"

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void cfw_zero(void *p, uint32_t n) { memset(p, 0, n); }

#include "geom.c"
#include "vector.c"

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static int probes(customCfwContext *ctx, int argc, char **argv, int from) {
    int fail = 0;
    for (int i = from; i < argc; i++) {
        int x, y, g;
        if (sscanf(argv[i], "%d,%d,%d", &x, &y, &g) != 3) { fprintf(stderr, "bad probe %s\n", argv[i]); return 1; }
        int got = (ctx->canvas_buf[y * CFW_W + x] + 8) / 17;
        int ok = got == g;
        printf("  probe (%d,%d) expected %d got %d %s\n", x, y, g, got, ok ? "ok" : "MISMATCH");
        if (!ok) fail = 1;
    }
    return fail;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see source\n"); return 2; }
    static customCfwContext ctx;
    static uint8_t canvas[CFW_W * CFW_H];
    static cfw_vector_work work;
    memset(&ctx, 0, sizeof ctx);
    memset(canvas, 0, sizeof canvas);
    ctx.canvas_buf = canvas;
    ctx.vector_work = &work;
    ctx.draw_gray = 15 * 17;
    int from;
    if (!strcmp(argv[1], "path")) {
        if (argc < 10) return 2;
        uint8_t rule = (uint8_t)atoi(argv[2]);
        int16_t p[8] = { (int16_t)atoi(argv[4]), (int16_t)atoi(argv[5]), (int16_t)atoi(argv[3]), 0, 0, 0, 0, 0 };
        cfw_rotation t;
        memset(&t, 0, sizeof t);
        t.angle = atoi(argv[6]); t.px = (int16_t)atoi(argv[7]); t.py = (int16_t)atoi(argv[8]);
        const char *hex = argv[9];
        size_t n = strlen(hex) / 2;
        uint8_t *cmds = malloc(n);
        for (size_t i = 0; i < n; i++) cmds[i] = (uint8_t)(hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]));
        int edges = cfw_vec_compile(&work, cmds, (uint32_t)n);
        if (edges < 0) { printf("compile refused\n"); return 1; }
        printf("compiled %zu command bytes into %d edges\n", n, edges);
        cfw_path *path = malloc(sizeof(cfw_path) + (size_t)edges * sizeof(cfw_edge));
        path->count = (uint16_t)edges; path->rule = rule; path->pad = 0;
        memcpy(path->edges, work.edges, (size_t)edges * sizeof(cfw_edge));
        cfw_vec_path_draw(&ctx, &work, path, p, &t);
        from = 10;
    } else if (!strcmp(argv[1], "shape")) {
        if (argc < 15) return 2;
        cfw_object o;
        memset(&o, 0, sizeof o);
        o.id = 1; o.type = (uint8_t)atoi(argv[2]); o.width = (uint8_t)atoi(argv[3]);
        for (int i = 0; i < 8; i++) o.p[i] = (int16_t)atoi(argv[4 + i]);
        o.rotation.angle = atoi(argv[12]); o.rotation.px = (int16_t)atoi(argv[13]); o.rotation.py = (int16_t)atoi(argv[14]);
        if (cfw_vec_rotated(&o.rotation)) cfw_vec_shape_draw(&ctx, &work, &o);
        else cfw_geom_draw(&ctx, o.type, o.width, o.p);
        from = 15;
    } else return 2;
    const char *pgm = getenv("VECTOR_PGM");
    if (pgm) {
        FILE *f = fopen(pgm, "wb");
        if (f) { fprintf(f, "P5\n%d %d\n255\n", CFW_W, CFW_H); fwrite(canvas, 1, sizeof canvas, f); fclose(f); }
    }
    return probes(&ctx, argc, argv, from);
}
