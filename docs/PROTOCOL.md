# PROTOCOL — the Glassly CFW link, firmware view

This is the firmware side of the Glassly CFW link (business `0xF0`). It mirrors
the phone-side record/message/asset formats so the same encoders work for RayNeo
iO and for the Even Realities G2 CFW. Panel is 540×180, single display, 1-bit
(the firmware thresholds 4bpp gray `>= 8` to on; the L8 canvas writes 255/0).

## Layers

```
AA55 frame  (phone BLE transport)  -> payload after the business byte
  packets          [opt u8][stream u8][chunk]  (3.1)   opt: 0x01 LENS, 0x80 RESET, 0x40 END, 0x10 CONTROL
    control        [0x10][stream][ctrl u8][arg u8]  (3.2)   1 QUERY 2 PAGE_OPEN 3 PAGE_CLOSE 4 KEEPALIVE 6 COMPASS(arg: bits 0-1 = 0 stop / 1 headings / 2 headings + raw records, high nibble = axis mapping, 0 = the device's own (RayNeo iO: forward +y = 0x20): bits 4-5 forward axis 1 x 2 y 3 z, bit 6 negate, bit 7 mirror; revision 6, compass.c) 5 DISPLAY_PROBE(arg = mode; mode 8 = screen_power_force_off, the report ends in [fb power][page open][own][dirty][pan pending][takeover][screen cb][screen on]; revision 5 tooling)
    record stream  [flags][len u16 LE][crc u16 LE][body]   (3.3 L1)
      flags: bits 0-1 lens targets (0x01 left, 0x02 right, 0x03 both; revision 5 keeps a scene per lens, before only 0x01), 0x04 COMPRESSED (zlib SYNC_FLUSH, body ends 00 00 FF FF), 0x08 RESET_CONTEXT
      crc = CRC-16/CCITT-FALSE(poly 0x1021, init 0xFFFF) of the DECODED message
      messages (L2): [mode][epoch u16][request u16] body   (modes are DECIMAL 37..41, as G2CfwTransport.isCacheMessage)
        41 CONTROL  [41][epoch][request][sub]...  sub 0 RESET [cap] (adopts `epoch`), 1 DROP [n][id u32 x n] (inactive objects;
                    an active one refuses with reason 5), 2 PERIOD [ms] (animation frame period 10..250, default 33)
        40 STATE    [40][epoch][revision u32][page u16]  (no request field; the reply echoes the page) — the journal, revision 6:
                    UNCHANGED when the phone's mirror of that lens is at the lens's revision; DELTA [n][(kind u8, id u32, version u32) x n]
                    (kind 1 = asset, version 0 = gone) with the journal entries after `revision` when they fit one reply (64-entry ring
                    per lens; assets land in both); otherwise SNAPSHOT [total u16][page u16][n][(kind, id, version, last_use u32) x n],
                    12 entries per page (this lens's objects, then complete assets). epoch 0 asks for a snapshot; a wrong epoch is
                    STALE; before any RESET the status is RESET (8).
        37 PUT      PUT_ASSET  [00][id u32][ver u32][kind u8][total u32][offset u32][len u16][data]
                    PUT_OBJECT [01][id u32][ver u32][shape record]
        38 SHOW     [38][epoch][request][flags][bg&0x0F][puts u8][puts entries, no length prefix][count u16][count×(id,ver)][ops]
                    flags: bit0 PRESENT (always on RayNeo), bit1 KEEP (count must be 0: ops only, active list kept),
                           bit2 TAG (report SETTLED [request] once every animation this SHOW started has ended),
                           bit3 FREEZE (stop every running animation at its current value first)
                    ops (revision 4; id 0xffffffff = every active object for 2/3/4/6/7):
                      [2][id u32][visible u8]                              VISIBLE
                      [3][id][dx i16][dy i16]                              MOVE
                      [4][id][dx i16][dy i16][frames u8][curve x4]         GLIDE
                      [5][id][mask u16][frames u8][curve x4][i16 per bit]  TWEEN (bits 0-7 p0..p7, 0x100 color, 0x200 width)
                      [6][id] FREEZE   [7][id] FINISH
                      [9][id][angle i32 deg*256][pivot x i16][pivot y i16][ms u16][curve x4]  ROTATE (revision 6: geometry 1..13 and
                           PATH; a ms-clocked channel independent of the geometry tween, pivot in panel pixels, ±100 turns, retarget
                           from the elapsed angle; other types refuse with reason 3)
                    A SHOW/HIDE repeating the last applied request id is acknowledged APPLIED without re-applying.
        39 HIDE     [39][epoch][request][flags][bg]   (bit2 TAG → SETTLED [request] right away)
```

Shape records (coords signed px on 540×180, clipped; `width` = stroke, byte 3): the 13 geometry types
`LINE(1) RECT(2) RECT_FILL(3) CIRCLE(4) CIRCLE_FILL(5) TRI(6) TRI_FILL(7)
QUAD(8) QUAD_FILL(9) BEZIER2(10) BEZIER3(11) ARC(12) PIE(13)` are 20 B
(`[type][flags][gray][width]` + eight i16 params, the G2 shapes.h layouts,
rasterized by `geom.c`); `IMAGE(14)` 12 B, `TEXT_CACHED(16)` 16 B,
`TEXT_INLINE(17)` 13+len. The `[gray]` byte of IMAGE/TEXT/TEXT_CACHED/TEXT_INLINE
records is the G2 texture options byte `[inverse 0x20][transparent 0x10][ink 0..15]`
(revision 6): source grays scale into 0..ink, inverse reverses the ramp,
transparent skips source 0; a colour TWEEN (bit 0x100) on these records fades
them. `PATH(18)` (revision 6, the G2 VECTOR_PROTOCOL.md):
`[18][flags][gray][rule 0 nonzero/1 even-odd][x i16][y i16][scale Q8 u16 0..2048][len u16][commands]`,
commands `0 M x y`, `1 L x y`, `2 Q cx cy x y`, `3 C c0x c0y c1x c1y x y`, `4 Z` in signed
Q4 (1/16 px), every contour closed; compiled at PUT into at most 512 edges
(8192 command bytes, 1024 commands, subdivision depth 8) kept with the object
in the store; a SHOW whose paths exceed 2048 edges per lens refuses with reason
6, a path that does not compile with reason 2. TWEEN on a path: bits 0 x, 1 y,
2 scale (0..2048), 8 color (0..15). Paths are anti-aliased like the primitives
(coverage rows, winding rule). Image
asset = `[w u8][h u8][4bpp RLE]`; RLE tokens `[cnt<<4|c]`, `[c][cnt u8]`,
`[c][00][cnt u16]`. `ops` are always empty on RayNeo; no warm-up SHOW/TWEEN is
sent — the first cache message after RESET is a real PUT or SHOW.

## Replies (glasses → phone, business 0xF0, ≤192 B, one at a time ≥100 ms apart; kind bytes in hex: IDENT is 0x11, PAGE 0x13, DIAG 0x15)

```
ACK   [01][stream][ordinal u16][lens=1][size u16][crc u16]
NACK  [03][stream][ordinal u16][lens][size u16][crc u16]
REPLY [05][stream][ordinal u16][lens][mode][request u16][status u8][epoch u16][revision u32]   (before the ACK)
GESTURE [07][eventType u8][source u8][tick u32 LE]
IDENT [11][ver u8][w u16 LE][h u16 LE][maxChunk u16 LE][storeKiB u16 LE][revision u16 LE][flags u8]"GLASSLYCFW-IO/6"   (w,h = 540x220 since revision 5; flags bit 3 = stereo: two lens scenes)
PAGE  [13][state u8][reason u8]
DIAG  [15][source u8][entry u32][ctx u32][uiModule u8][regRc i8][sendRc i8][timer u32][tick u32]
ANCS  [17][seq u8][total u16 LE][offset u16 LE][json chunk]   (iOS notification relay, ancs.c)
DISP  [19][mode u8][rc i8][disp u32][drv u32][fd i32][fmt u8][xres u16][yres u16][nplanes u8][fbmem u32][fblen u32][stride u16][display u8][bpp u8][xresV u32][yresV u32][xoff u32][yoff u32][map u32][buf2 u32][yoff2 u32][cf u8][w u32][h u32][drawStride u32][refrPass u8][leftParallax i32][rightParallax i32][depthMode i32][dispMode i32][lcdc b5 b6 b7 b8]   (display probe, disp.c; tooling only, the phone ignores it)
```

* `stream` = the **low byte of the RESET frame's 16-bit AA55 messageNumber**,
  as carried in every packet's second byte (RNLink strips the frame header
  before the business callback runs, so the phone repeats it).
* `ordinal` = per-stream message counter (reset at RESET). The SDK packs a
  frame's left and right SHOWs into one stream (ordinals 0 and 1) so both
  lenses apply them in the same handler call; each lens acks and replies to
  the record addressed to it.
* IDENT: `ver=4`, `w=540`, `h=180`, `maxChunk=240`, `storeKiB=64`, `revision=4`,
  `flags = 0x07` normally. Flag bits: **bit0** = zlib COMPRESSED records
  accepted (the stock inflater is used); **bit1** = raw packets accepted on
  business 0xF0; **bit2** = the heap context existed when IDENT was built
  (diagnostic; the phone only logs it).
* DIAG is a diagnostics reply the phone logs. `source` 1 = sent directly from
  the RNLink RX callback (right after IDENT), 2 = queued from the LVGL message
  handler (proves the thread hop). `entry` is the module's RNLink registry
  entry, `ctx` the heap context, `uiModule` the message-processor id in use,
  `regRc`/`sendRc` the last registration/send results, `timer` the LVGL
  service timer, `tick` `os_get_tick_count()`.
* ANCS: the JSON the stock firmware's ANCS client builds for one iOS
  notification (`notificationUID`, `appId`, `appName`, `title`, `subtitle`,
  `content`, `location`, `carrier`, `timestamp` `yyyyMMdd'T'HHmmss`, `eventId`
  0 added / 1 modified / 2 removed, `eventFlags` bit2 = pre-existing, bit1 =
  important, `categoryID`, `categoryCount`), split into <= 185-byte chunks with
  one `seq`; reassemble by (seq, offset) until offset + chunk == total. Sent
  only after a QUERY on this connection; a notification that does not fit the
  free uplink slots is dropped whole. Testable only from an iPhone (macOS has
  no ANCS server).
* SETTLED `[09][tag u16 LE]`: the request id of a TAG-flagged SHOW (or HIDE)
  once every animation it started has ended, immediately when none did. The
  frame timer runs at 33 ms (`CFW_ANIM_PERIOD_MS`); easing is a cubic bezier
  `x1 y1 x2 y2` in /255, Q15 math as on the G2.
* REPLY `status`: 0 APPLIED, 1 STALE (epoch mismatch), 2 MISSING (unknown id/ver),
  3 CAPACITY, 4 REFUSED [reason: 2 path commands, 3 op target, 4 tween mask/value,
  5 DROP of an active object, 6 edge budget], 5 UNCHANGED, 6 DELTA, 7 SNAPSHOT,
  8 RESET (STATE only). The reply's `epoch` is the current one and `revision`
  the lens's journal revision after the message (revision 6).
* COMPASS `[1b][heading u16 LE deci-degrees][accuracy u8 0..3]` (revision 6): the
  heading of the device's forward axis from the sensor hub's synchronized
  IMU+magnetometer stream, tilt-compensated, hard iron removed by the midpoint
  of the magnetometer range seen since the stream started (look around once);
  sent when it moved >= 2° and >= 200 ms after the last one. COMPASS_RAW
  `[1d][ax ay az][gx gy gz][mx my mz][heading u16][accuracy]` (i16 LE: accel
  x1000, gyro x100, mag x100) every 100 ms in raw mode, for calibration.
* GESTURE `eventType`: 0 single_tap, 1 swipe_up, 2 swipe_down, 3 double_tap,
  11 tap_then_hold; `source` 0 touchpad/crown, 1 button.
* PAGE `state` 1 open/0 closed; `reason` 0 ok, 1 launcher busy, 2 not paired (no longer sent),
  3 lease expired, 4 user backed out.

## Control / session rules the firmware enforces

* **QUERY → IDENT** is answered synchronously from the RNLink RX callback,
  without the page open, without `isBussinessIdle`, and without the LVGL hop
  (the phone classifies as stock after 2 s of silence). A DIAG follows it; a
  second DIAG arrives through the normal queued uplink once the packet has
  crossed to the LVGL thread.
* Cache messages (RESET/PUT/SHOW/HIDE) are **NACKed while the page is closed**.
* **PAGE_OPEN** gates on `isBussinessIdle()` (the paired() gate was dropped: a guest-authenticated phone never reaches pair state 2). Failure replies
  `PAGE [0][reason]` (reason 1/2 are retried by the phone at 2/5/10 s); success
  replies `PAGE [1][0]`, arms the 120 s lease, and reports display activity.
* **KEEPALIVE** renews the 120 s lease (`PAGE [1][0]`). The lease expiring, a
  PAGE_CLOSE, or a >2 s long press close the page (`PAGE [0][reason]`).
* With IDENT `flags bit0 = 0` the phone would send every record uncompressed with
  RESET_CONTEXT only on the first; this build advertises bit0 = 1.

## Downlink

Every packet is the payload of one business-0xF0 AA55 frame. A record stream
is RESET … END with consecutive frame numbers; `stream` in every packet is the
low byte of the RESET frame's number and comes back in ACK/NACK/REPLY. A
control packet is a single frame `[0x10][stream][ctrl][arg?]` (`stream` ignored;
`arg` only for DISPLAY_PROBE: 0 report, 1 framebuffer test pattern, 2 dual-eye
test pattern, 3/4 stock flush pass on/off).
The firmware receives frames through an RNLink callback table it registers for
business 0xF0 at launcher start (see `docs/HOOKS.md`); the stock file transfer
is not involved. Revision 1's named-file downlink no longer exists.

Replies: business 0xF0 via `rnlink_if_send_payload`, ≤ 192 B, one at a time
≥ 100 ms apart, drained by the module's 100 ms LVGL timer, which the first
handled packet creates. IDENT and the first DIAG bypass the queue: they are
sent from the RX callback itself (the stock RX path sends its own
acknowledgements from that context through the same protocol layer).
