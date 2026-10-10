/* Host check of compass.c: the integer atan2 against libm over a grid, and
 * the heading pipeline on raw records captured from the glasses (the Mac
 * harness's "CFW: compass raw ..." lines on stdin, as the monitor logs them).
 *
 *   clang -std=c11 -Ipatches -Ipatches/generated -o compass_test tools/host/compass_test.c -lm
 *   ./compass_test atan2
 *   ./compass_test [map] < session.log        # heading per record; map = the control byte's high nibble (0 = device)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfw.h"

static customCfwContext g_ctx;
customCfwContext *cfw_ctx_peek(void) { return &g_ctx; }
int cfw_uplink_push(customCfwContext *ctx, const uint8_t *data, uint8_t len) { (void)ctx; (void)data; (void)len; return 1; }

#include "compass.c"

static uint32_t fbits(double v) { float f = (float)v; uint32_t b; memcpy(&b, &f, 4); return b; }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "atan2")) {
        double worst = 0;
        for (int y = -3000; y <= 3000; y += 37)
            for (int x = -3000; x <= 3000; x += 41) {
                if (!x && !y) continue;
                double want = fmod(atan2(y, x) * 1800.0 / M_PI + 3600.0, 3600.0);
                double got = cp_atan2_deci(y, x);
                double d = fabs(got - want); if (d > 1800) d = 3600 - d;
                if (d > worst) worst = d;
            }
        printf("atan2: worst error %.1f deci-degrees %s\n", worst, worst <= 5 ? "ok" : "TOO LARGE");
        return worst <= 5 ? 0 : 1;
    }
    g_ctx.compass_mode = 2;
    g_ctx.compass_axes = argc > 1 ? (uint8_t)strtoul(argv[1], NULL, 0) : 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *p = strstr(line, "compass raw a=(");
        if (!p) continue;
        int a[3], g[3], m[3];
        if (sscanf(p, "compass raw a=(%d,%d,%d)/1000 g=(%d,%d,%d)/100 m=(%d,%d,%d)/100", &a[0], &a[1], &a[2], &g[0], &g[1], &g[2], &m[0], &m[1], &m[2]) != 9) continue;
        cfw_compass_cb(0, 0, 0, fbits(a[0] / 1000.0), fbits(a[1] / 1000.0), fbits(a[2] / 1000.0),
                       fbits(g[0] / 100.0), fbits(g[1] / 100.0), fbits(g[2] / 100.0),
                       fbits(m[0] / 100.0), fbits(m[1] / 100.0), fbits(m[2] / 100.0));
        printf("%.12s heading %5.1f accuracy %u  g=(%d,%d,%d)\n", line, g_ctx.compass_heading / 10.0, g_ctx.compass_accuracy, g[0], g[1], g[2]);
        n++;
    }
    fprintf(stderr, "%d records\n", n);
    return n ? 0 : 1;
}
