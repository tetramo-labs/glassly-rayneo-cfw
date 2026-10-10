/* Compass: a heading from the sensor hub's synchronized IMU+magnetometer
 * stream (the G2 reads the stock quaternion; the RayNeo AP exports no fused
 * orientation, see docs/RAW-LINK-PLAN.md item 10).
 *
 * sensor_rpmsg_start_report_sync_imu_mag asks the BTH core to stream, and
 * the registered handler runs on the sensor rpmsg thread with the record the
 * stock factory handler prints as "timestamp(ms), ax ay az, gx gy gz, mx my
 * mz": a u64 timestamp in r0:r1, a word the stock ignores in r2, then nine
 * IEEE floats (softfp: in r3 and on the stack). The module is built without
 * float support, so the bits are decoded by hand into fixed point and the
 * heading is integer math: down = -accel, east = down x mag, north = east x
 * down, heading = atan2(f.east, f.north) for the device's forward axis f.
 * Hard iron: the midpoint of the magnetometer's min/max per axis since the
 * stream started (the wearer looks around once); accuracy counts the axes
 * whose span is wide enough. The page timer (UI thread) sends COMPASS
 * `[0x1b][heading u16 deci-degrees][accuracy u8]` when the heading moved
 * (change only, >= COMPASS_MIN_GAP_MS apart) and, in raw mode, COMPASS_RAW
 * `[0x1d][ax ay az][gx gy gz][mx my mz][heading u16][accuracy]` (i16 each:
 * accel x1000, gyro x100, mag x100) every COMPASS_RAW_MS for calibration.
 *
 * CONTROL 6 arg: bits 0-1 mode (0 stop, 1 headings, 2 headings + raw),
 * bits 4-5 forward axis (0 the device's own, 1 x, 2 y, 3 z), bit 6 forward
 * negated, bit 7 mirror (counter-clockwise sensor frame). The mapping is
 * settable live so a device's axes can be found from the harness without a
 * rebuild. The RayNeo iO's (2026-10-10, worn): +x up (the accelerometer
 * reads +g there, tilted 26 degrees toward +y), +y forward (a nod down
 * pitches +y below the horizon and spins +z positive, so +z is left), a turn
 * to the right spins the vertical axis negative (a right-handed IMU frame)
 * yet moved the field-derived heading the wrong way through a full turn and
 * through a 90 degree one: the magnetometer's frame is mirrored against the
 * IMU's, so the device map is forward +y with the mirror bit (0xa0). */
#include "cfw.h"

/* ---- float bits -> fixed point ------------------------------------------- */
/* value * scale, rounded toward zero; +-2^31 saturates. */
static int32_t cp_f2i(uint32_t bits, int32_t scale) {
    int32_t exp = (int32_t)((bits >> 23) & 0xffu);
    uint32_t mant = bits & 0x7fffffu;
    if (exp == 0) return 0;                          /* zero / denormal */
    if (exp == 255) return bits & 0x80000000u ? -2147483647 : 2147483647;
    mant |= 0x800000u;
    int64_t v = (int64_t)mant * scale;              /* value = mant * 2^(exp-150) */
    int32_t sh = exp - 150;
    if (sh > 0) { if (sh > 30) return bits & 0x80000000u ? -2147483647 : 2147483647; v <<= sh; }
    else if (sh < 0) { if (sh < -63) v = 0; else v >>= -sh; }
    if (v > 2147483647LL) v = 2147483647LL;
    int32_t r = (int32_t)v;
    return bits & 0x80000000u ? -r : r;
}

/* ---- integer square root ---------------------------------------------------- */
static int32_t cp_isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (int32_t)r;
}

/* ---- integer atan2, deci-degrees 0..3599 ----------------------------------- */
static uint32_t cp_atan2_deci(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    /* scale both into 15 bits so the products below fit 32 bits */
    int32_t big = ax > ay ? ax : ay;
    while (big > 0x7fff) { ax >>= 1; ay >>= 1; big >>= 1; }
    int32_t num = ax < ay ? ax : ay, den = big ? big : 1;
    int32_t r = (num << 15) / den;                   /* 0..32768, Q15 ratio */
    /* atan(r) ~ r*45deg - r*(r-1)*(14.02deg + 3.80deg*r), in deci-degrees */
    int32_t rm1 = r - 32768;
    int32_t poly = 140 + (int32_t)(((int64_t)38 * r) >> 15);
    int32_t a = (int32_t)(((int64_t)450 * r) >> 15) - (int32_t)((((int64_t)r * rm1) >> 15) * poly >> 15);
    if (ay > ax) a = 900 - a;                        /* first octant -> first quadrant */
    if (x < 0) a = 1800 - a;
    if (y < 0) a = -a;
    a %= 3600;
    if (a < 0) a += 3600;
    return (uint32_t)a;
}

/* ---- the stream handler (sensor rpmsg thread) ------------------------------ */
#define CP_ACCEL_SCALE 1000
#define CP_GYRO_SCALE  100
#define CP_MAG_SCALE   100
#define COMPASS_DEVICE_MAP 0xa0u  /* forward +y, mirrored: see the header comment */
#define CP_SPAN_MIN    1500          /* 15 uT: an axis counts for accuracy past this */
#define CP_SPAN_GOOD   3000          /* 30 uT */

void cfw_compass_cb(uint32_t ts_lo, uint32_t ts_hi, uint32_t unused,
                    uint32_t fax, uint32_t fay, uint32_t faz, uint32_t fgx, uint32_t fgy, uint32_t fgz,
                    uint32_t fmx, uint32_t fmy, uint32_t fmz) {
    (void)ts_lo; (void)ts_hi; (void)unused;
    customCfwContext *ctx = cfw_ctx_peek();
    if (!ctx || !ctx->compass_mode) return;
    int32_t a[3] = { cp_f2i(fax, CP_ACCEL_SCALE), cp_f2i(fay, CP_ACCEL_SCALE), cp_f2i(faz, CP_ACCEL_SCALE) };
    int32_t g[3] = { cp_f2i(fgx, CP_GYRO_SCALE), cp_f2i(fgy, CP_GYRO_SCALE), cp_f2i(fgz, CP_GYRO_SCALE) };
    int32_t m[3] = { cp_f2i(fmx, CP_MAG_SCALE), cp_f2i(fmy, CP_MAG_SCALE), cp_f2i(fmz, CP_MAG_SCALE) };
    int32_t m0[3] = { m[0], m[1], m[2] };
    /* hard iron: midpoint of the range seen since the stream started, but
     * only once an axis has swept enough of it. Before that the midpoint is
     * the reading itself and subtracting it leaves nothing but noise (the
     * first hardware session: a still device reported every heading). The
     * uncorrected field gives a stable, if offset, heading meanwhile. */
    uint32_t axes = 0;
    for (unsigned i = 0; i < 3; i++) {
        if (!ctx->compass_span_valid) { ctx->compass_min[i] = m[i]; ctx->compass_max[i] = m[i]; }
        if (m[i] < ctx->compass_min[i]) ctx->compass_min[i] = m[i];
        if (m[i] > ctx->compass_max[i]) ctx->compass_max[i] = m[i];
        int32_t span = ctx->compass_max[i] - ctx->compass_min[i];
        if (span >= CP_SPAN_GOOD) axes += 2; else if (span >= CP_SPAN_MIN) axes += 1;
        if (span >= CP_SPAN_MIN) m[i] -= (ctx->compass_min[i] + ctx->compass_max[i]) / 2;
    }
    /* a light low-pass on both vectors (1/4 of each new sample): the
     * magnetometer's +-0.5 uT noise on a ~25 uT field is a few degrees */
    for (unsigned i = 0; i < 3; i++) {
        if (!ctx->compass_span_valid) { ctx->compass_avg_m[i] = m[i]; ctx->compass_avg_a[i] = a[i]; }
        ctx->compass_avg_m[i] += (m[i] - ctx->compass_avg_m[i]) / 4;
        ctx->compass_avg_a[i] += (a[i] - ctx->compass_avg_a[i]) / 4;
        m[i] = ctx->compass_avg_m[i]; a[i] = ctx->compass_avg_a[i];
    }
    ctx->compass_span_valid = 1;
    uint8_t accuracy = (uint8_t)(axes >= 5 ? 3 : (axes >= 3 ? 2 : (axes >= 1 ? 1 : 0)));
    /* down = -accel (the accelerometer reads the reaction, +g up), as a
     * Q10 unit vector so east and north come out on one scale: east = d x m
     * carries |d| once and north = east x d twice, and atan2 of the two needs
     * them equal (the first build shifted both by 8 bits and every heading
     * collapsed toward 0 or 180). */
    int32_t d[3] = { -a[0], -a[1], -a[2] };
    int32_t len = cp_isqrt((uint32_t)(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    if (len < 64) return;                            /* free fall: no down */
    for (unsigned i = 0; i < 3; i++) d[i] = (d[i] << 10) / len;
    /* mag ~ +-20000 after the offset, d ~ +-1024: e fits 2^26; shift e to
     * 2^16 so n = e x d (another 2^11) fits, then drop d's Q10 from n */
    for (unsigned i = 0; i < 3; i++) m[i] >>= 2;
    int32_t e[3];
    e[0] = d[1] * m[2] - d[2] * m[1];
    e[1] = d[2] * m[0] - d[0] * m[2];
    e[2] = d[0] * m[1] - d[1] * m[0];
    for (unsigned i = 0; i < 3; i++) e[i] >>= 8;
    int32_t n[3];
    n[0] = e[1] * d[2] - e[2] * d[1];
    n[1] = e[2] * d[0] - e[0] * d[2];
    n[2] = e[0] * d[1] - e[1] * d[0];
    for (unsigned i = 0; i < 3; i++) n[i] >>= 10;
    /* the mapping: the control byte's high nibble, or the device's own when
     * it is zero */
    uint32_t map = ctx->compass_axes & 0xf0u ? ctx->compass_axes : COMPASS_DEVICE_MAP;
    uint32_t axis = ((map >> 4) & 3u) - 1u;
    if (axis > 2) axis = 0;
    int32_t fe = e[axis], fn = n[axis];
    if (map & 0x40u) { fe = -fe; fn = -fn; }
    uint32_t heading = cp_atan2_deci(fe, fn);
    if (map & 0x80u) heading = (3600u - heading) % 3600u;
    ctx->compass_heading = (uint16_t)heading;
    ctx->compass_accuracy = accuracy;
    if (ctx->compass_mode >= 2) {
        for (unsigned i = 0; i < 3; i++) {
            ctx->compass_raw[i] = (int16_t)(a[i] > 32767 ? 32767 : (a[i] < -32768 ? -32768 : a[i]));
            ctx->compass_raw[3 + i] = (int16_t)(g[i] > 32767 ? 32767 : (g[i] < -32768 ? -32768 : g[i]));
            ctx->compass_raw[6 + i] = (int16_t)(m0[i] > 32767 ? 32767 : (m0[i] < -32768 ? -32768 : m0[i]));
        }
    }
    ctx->compass_seq++;
}

/* ---- control (UI thread) ----------------------------------------------------- */
#define COMPASS_MIN_GAP_MS 200u
#define COMPASS_RAW_MS     100u
#define COMPASS_MIN_DELTA  20u       /* deci-degrees */

void cfw_compass_control(customCfwContext *ctx, uint8_t arg) {
    uint8_t mode = arg & 3u;
    ctx->compass_axes = arg & 0xf0u;
    if (!mode) {
        if (ctx->compass_started) { fw_sensor_stop_sync_imu_mag(); ctx->compass_started = 0; }
        ctx->compass_mode = 0;
        return;
    }
    ctx->compass_mode = mode;
    if (!ctx->compass_started) {
        ctx->compass_span_valid = 0;
        ctx->compass_seq = ctx->compass_sent_seq = 0;
        ctx->compass_last_heading = 0xffffu;
        fw_sensor_register_sync_imu_mag(cfw_compass_cb);
        int rc = fw_sensor_start_sync_imu_mag();
        ctx->compass_started = rc == 0;
        ctx->compass_rc = (int8_t)rc;
    }
}

void cfw_compass_stop(customCfwContext *ctx) {
    if (ctx->compass_started) { fw_sensor_stop_sync_imu_mag(); ctx->compass_started = 0; }
    ctx->compass_mode = 0;
}

/* Page timer: report a moved heading, and the raw record in raw mode. */
void cfw_compass_tick(customCfwContext *ctx) {
    if (!ctx->compass_mode || ctx->compass_seq == ctx->compass_sent_seq) return;
    uint32_t now = fw_tick();
    uint16_t h = ctx->compass_heading;
    uint8_t acc = ctx->compass_accuracy;
    if (ctx->compass_mode >= 2 && (now - ctx->compass_raw_tick) >= COMPASS_RAW_MS) {
        uint8_t b[1 + 18 + 3];
        b[0] = RPL_COMPASS_RAW;
        for (unsigned i = 0; i < 9; i++) cfw_put16(b + 1 + 2 * i, (uint16_t)ctx->compass_raw[i]);
        cfw_put16(b + 19, h); b[21] = acc;
        if (cfw_uplink_push(ctx, b, sizeof(b))) ctx->compass_raw_tick = now;
    }
    uint32_t delta = h > ctx->compass_last_heading ? h - ctx->compass_last_heading : ctx->compass_last_heading - h;
    if (delta > 1800u) delta = 3600u - delta;
    if (ctx->compass_last_heading != 0xffffu && (delta < COMPASS_MIN_DELTA || (now - ctx->compass_sent_tick) < COMPASS_MIN_GAP_MS)) {
        ctx->compass_sent_seq = ctx->compass_seq;
        return;
    }
    uint8_t b[4] = { RPL_COMPASS, (uint8_t)h, (uint8_t)(h >> 8), acc };
    if (cfw_uplink_push(ctx, b, sizeof(b))) {
        ctx->compass_last_heading = h;
        ctx->compass_sent_tick = now;
        ctx->compass_sent_seq = ctx->compass_seq;
    }
}
