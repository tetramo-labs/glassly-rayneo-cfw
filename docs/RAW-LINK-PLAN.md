# Raw business-0xF0 link — verified facts and the v2 plan (2026-10-07)

Everything below was read from the stock 1.0.4.12 `nuttx_ap.bin` with capstone
(`./venv/bin/python`, `CS_MODE_THUMB|CS_MODE_MCLASS`, load base `0x10190000`,
RAM/data via `symbols.json` segments). Two flashes of revision 1 booted and
OTA-confirmed on hardware, but the Glassly link stayed silent. Causes, in order
found:

1. **Thumb bit** (fixed in `build.py`, commit 76c7586): module function pointers
   were even and +1.
2. **The hook is not a file sink.** `0x106d9ce4` is
   `LinkMonitor::registerLinkCallback`. It fills a 32-byte callback table at
   `0x19750d2c` and registers it with `rnlink_if_register_callback(cbs, 0, 15)`
   (business 15 = launcher), then a second table `0x19750d0c` for business 18
   (`onBigCoreFileRecvCallback`, which only logs). Slot `[0x18]` is RNLink's
   *file-received* callback, invoked by `rnlink_if_on_file_recv` for every
   registered table. The file transfer the phone speaks (business 18 INFO) is
   never acknowledged by this firmware (`handle_complete_packet` routes business
   `0x11` to `ft_on_file_transfer_recv`; 18 goes to a logger). The named-file
   downlink is dead on stock and CFW alike.
3. **message_processor module id.** `message_processor_send` and
   `register_handler_internal` accept module ids 1..15 only; the module used
   0xF0. Ids in use: 1 LAUNCHER, 2, 4, 6, 7, 8, 9 (AlwaysOn), 10, 11, 12 (display
   msg handler), 13 (A2UI), 14, 15 (RnLinkMsgChannelHost). **3 PHONE and 5
   MUSIC have no observed registration.** Duplicate registration is refused
   with `-3` ("already registered") without disturbing the existing handler.
4. **Uplink never flushed before a page exists.** `cfw_uplink_flush` runs only
   from the page timer created in `cfw_page_ensure_ui`, so IDENT would sit in
   the queue on a fresh boot.

## Verified ABI

* `rnlink_if_register_callback(const void *cbs32, uint32_t sub, uint8_t bus)`
  @ `0x107763dd`: 24-entry table at `0x1987c780`, entry 0x2c bytes =
  `{cbs copy[0x20], sub @0x20, in-use flag @0x24, bus @0x28}`. Slots: 0 data(bus,
  data, len) — only when the frame is complete; 1 conn state; 2 multipoint;
  3 bond; 4 device info; 5 send done; 6 file received(file*) for every table;
  7 data_ex(bus, data, len, flag). Business byte compared with `ldrb`, so
  0xF0 is addressable. `rnlink_if_on_data_recv` passes the business straight
  through; the only special cases upstream are business 0x10 (handshake) and
  0x11. The AA55 messageNumber is **not** available to the callback.
* `message_processor_register_c_handler(module, handler, user)`: handler is
  called as `handler(TIONativeMessage *copy, user)` from
  `message_processor_handle`, an LVGL timer (16 ms) created by
  `message_processor_init` — i.e. on the LVGL thread. `message_processor_send`
  copies `{id, data, bytes, reserved, context, mode}` (mode must be 0 or 1;
  mode 0 passes `data` through) into a 0x30-byte queue record. The module's
  `TIONativeMessage` layout is correct.
* LVGL is built with `LV_USE_OS == NONE` (`lv_mutex_lock` returns 1):
  `lv_timer_create`/`lv_async_call` are only safe on the LVGL thread.
* Uplink `rnlink_if_send_payload(bus, buf, len, cb)` has no business check.

## v2 design (not yet implemented)

Firmware (`GLASSLYCFW-IO/2`, IDENT flags `0x03`):

* Shim at `0x106d9d1a`: `push {r4, lr}; bl cfw_boot_register; pop {r4, lr}` then
  the stock sequence unchanged (slot `[0x18]` stays NULL). `cfw_boot_register`
  calls `rnlink_if_register_callback(&cfw_rn_cbs, 0, 0xF0)`; `cfw_rn_cbs` is a
  `const` 8-slot table in `.rodata` with only slot 0 = `cfw_rx_data`. Add the
  symbol to `FW_SYMBOLS`; assert the Thumb address of `cfw_rx_data` in the blob.
* `cfw_rx_data(bus, data, len)` (RNLink thread): bus must be 0xF0, 2 ≤ len ≤
  4096; lazily `register_c_handler(5 → 3, cfw_ui_handler)` and record the id in
  `ctx->ui_module` (replaces `ui_registered`); deep-copy and
  `message_processor_send(ctx->ui_module, &msg)`, free on failure. The
  registration races the LVGL-thread map lookup; accepted, document it.
* Raw packet becomes `[opt][stream][chunk]`; control `[0x10][stream][ctrl]`.
  `cfw_ui_handler` parses one packet per message, sets `stream.stream_id` on
  RESET, creates the 100 ms service timer if absent (LVGL thread), and calls
  `cfw_uplink_flush` after each packet. Delete `cfw_file_rx`, `TIONativeFile`,
  `FILE_MAX_BYTES`, `UI_MODULE`.
* Docs: PROTOCOL.md (3.1 packet layout, flags 0x03, drop the named-file
  section), HOOKS.md (facts above; the hooked function's real identity),
  README/SAFETY, `tools/probe.py` (`[0x10, 0, 1]`), `hooks.json`.

Phone (both platforms):

* `packetize` inserts the stream byte (low byte of the RESET frame's message
  number; Android already receives `firstMessageNumber`, iOS computes
  `cfwNextStreamId` first), `controlPacket` gains a stream byte (0 is fine).
* Remove the file downlink: `RayNeoIOCfwFileLink`, `cfwDownlink`, `fileSender`
  use in `probeFirmware`/`cfwSend` (iOS); `fileBlock`/`fileBodies`/
  `parseFileBody`, `transmitFile`, `sendRecordAsFiles`, the file half of
  `queryCfwIdentity` (Android). Arm the 2 s stock verdict right after the QUERY
  frame. `minimumRevision`/`MINIMUM_REVISION` and the catalog revision → 2.
* Update `RayNeoIOProtocolTests` / `RayNeoIOProtocolTest` packet and control
  expectations; delete the file-link tests (keep `RayNeoIOFileTransfer` ones).

## 2026-10-09 — revision 2 result and revision 3

Revision 2 flashed, booted, OTA-confirmed and reconnected; the handshake
(which needs a business-15 reply) completed; the QUERY went out 2 s before the
stock verdict (`flashing` was already cleared on iOS); no IDENT. Further
disassembly:

* `LinkMonitor::registerLinkCallback` calls `rnlink_get_interface()->register`
  (vtable `0x182d33f4` + 8), which is `rnlink_if_register_callback` — the
  module's direct call is equivalent. It is reached from
  `LinkMonitor::doStartMonitoring`.
* `handle_complete_packet` has no business filter beyond 0x10/0x11; every
  other complete message reaches `rnlink_if_on_data_recv` with complete=1.
* `Launcher::Launcher` stores `this` at `0x19a1f954`;
  `ViewManager::ensureAppListView` keeps the view at `+0x10` but creates it
  lazily (`onEnterState`/`eventHandler`), so `cfw_ctx()` had no home until the
  app list was shown — a silent "no context" on every QUERY at connect time.
* The registry's `sub` word (+0x20) is read only by the registrar's duplicate
  check and by `rnlink_if_unregister_callback`; no dispatcher calls it.

Revision 3: the context pointer lives in the module's own registry entry
(`sub` word), the AppListView allocation hook is gone, IDENT (+ a DIAG) is
sent synchronously from the RX callback before the LVGL hop, and a second DIAG
is queued from the LVGL handler. Phone: logs DIAG, minimum revision 3, Android
re-probes after a flash (its probe at the post-reboot reconnect was blocked by
the still-active flasher).

## 2026-10-09 — revision 3 on hardware, iterated from a Mac

Flashing and probing moved to the SDK's Mac harness
(`glassly/mobile/modules/bluetooth-sdk/tools/rayneo-io-mac`), which runs the
same `RayNeoIOProtocol` / `RayNeoIOFlasher` / `G2CfwTransport` sources as the
app; `tools/suite` here is the acceptance run. Six flashes in one afternoon:

1. `entry=0 ctx=0` in DIAG: `rnlink_if_register_callback` stores the constant
   1 at +0x24 (in use), not the table pointer. Lookup now keys on in-use +
   business 0xF0 + slot 0.
2. The glasses reset ~10 s after every QUERY: `release_message_data` frees a
   mode-0 message's data after the handler, and `cfw_ui_handler` freed it too.
3. PAGE_OPEN refused with reason 2: `real_get_rnlink_paired_status` is
   device-info pair state == 2, never true for a guest-authenticated phone.
   Gate dropped.
4. Phone/firmware byte mismatches from reading `[11]`/`[13]` and `0x41`.. as
   the other radix: IDENT/PAGE kinds are 0x11/0x13 (the SDK had 11/13), the
   cache message modes are decimal 37..41 (the firmware had 0x37..0x41), and
   SHOW's inline PUT entries carry no length prefix (G2CfwScene.showPayload).

Also: macOS CoreBluetooth can leave `canSendWriteWithoutResponse` false for
good; the harness forces queued writes after 2 s (the glasses re-request a
slice after 20 s, four misses abort the OTA with errcode 8).

Same afternoon, two ports from the G2 CFW once the link was stable:

* `geom.c`: the G2 geometry rasterizer (shapes.c) retargeted to the L8 canvas,
  so all 13 record types the phone's `G2CfwScene` emits (lines, rounded rects,
  circles, triangles, quads, beziers, arcs, pies) render; before, only
  RECT/RECT_FILL did and the example miniapp's shapes suite would have been
  refused for half its cases. Suite cases `geometry-fill` / `geometry-stroke`.
* `ancs.c`: the ANCS relay. The stock firmware already runs a full ANCS client
  and serialises each notification as JSON for its own notification service;
  one retargeted `bl` (`0x10702e7e`) queues that JSON as business-0xF0 ANCS
  records (kind 0x17, chunked) before the stock handler runs. The phone
  reassembles and feeds `phone_notification` like the G2 relay. Not testable
  from the Mac (no ANCS server); iPhone only.

## Revision 4 (built 2026-10-09, not flashed) — animation, and what depth is here

* `anim.c` ports the G2 object-cache animation: per-object from/to geometry,
  color and width with a cubic-bezier easing, the SHOW ops VISIBLE/MOVE/GLIDE/
  TWEEN/FREEZE/FINISH (ROTATE refused: no rotated primitives without the
  vector engine), the KEEP/TAG/FREEZE SHOW flags, request-id dedupe, hidden
  objects, background gray, a 33 ms LVGL frame timer and SETTLED reports.
  The phone keeps element transitions for revision >= 4 (`RayNeoIOCfwSession`)
  and still zeroes rotation and depth. The suite's `anim` cases run only on
  revision 4 firmware and skip otherwise.
* Depth: the RayNeo iO is binocular. The stock LCDC composer copies the one
  540x180 framebuffer to each eye with a signed parallax byte
  (`rayneo_fb_get_left_parallax` / `_right_parallax`, read by
  `rayneo_lcdc_layer_update`), which the launcher's `display_config`
  `distance` sets; the app already drives that through `setDashboardPosition`.
  The G2's per-element depth (each lens its own SHOW, elements shifted by
  depth × parallax) has no equivalent on a single canvas; the composer does
  have per-layer depth (`rayneo_lcdc_layer_fac_depth_update_partial`), so an
  element-depth design would mean one LVGL layer per depth level with its own
  parallax. Revision 5 takes the other route: own the per-eye raster and
  bypass LVGL for presenting (see the revision 5 plan).

## Revision 5 plan — parity with the G2 Glassly CFW, LVGL bypassed (direction set 2026-10-09)

Match g2flash as closely as the hardware allows, protocol and behaviour, so
the phone treats both devices the same. Decision (2026-10-09): revision 5
stops drawing through LVGL. The G2 patch already has every piece (its own L8
raster, per-lens compose, builtin font, direct framebuffer present), and the
revision 3/4 code already rasterizes geometry, images and cached fonts into an
L8 buffer; only the stock label text and the final present still go through
LVGL objects. Bypassing it buys per-element depth for free (we own the per-eye
raster), removes the theme/scroll/style fights from page.c, and makes the
frame path identical to the G2's. LVGL keeps only two roles: the
message_processor timer thread we already run on (the 33 ms frame timer stays
an `lv_timer`), and the stock UI underneath us when no page is open.

Design:

* **Raster.** One 540×H L8 scene canvas rendered by scene.c/geom.c/anim.c as
  today, then composed into two per-eye framebuffers, each element blitted at
  `x ± depth × parallax` (left/right), exactly the G2's per-lens SHOW. The
  phone passes `depth` through instead of zeroing it; no protocol change.
* **Present.** Write the per-eye buffers to the LCDC layer directly through
  whatever `lv_rayneo_fbdev_create`'s flush_cb calls (`rayneo_lcdc_layer_update`
  and the per-eye parallax registers), not through `lv_disp_flush`. While a
  page is open, stock LVGL flushes must not land on top of us: the
  fbguard-lease equivalent is a retargeted call in the flush_cb that drops
  flushes while `page_open` (one hook, like the ANCS site). On close, force a
  full stock invalidate so the launcher repaints.
* **First flash is a probe**, from the Mac: a build that writes a test
  pattern through the driver call on PAGE_OPEN and reports the layer
  geometry/format in DIAG, before any raster work moves over. The display
  driver cannot be read blind: pixel format of the LCDC layer (L8 vs ARGB),
  whether both eyes are one surface or two, and whether the flush_cb owns
  the parallax write are the three facts the probe must settle.
* **Text** needs the builtin bitmap font (G2 `TEXT(15)`) in firmware because
  no LVGL label survives; the cached-font path (`draw_cached_font_text`)
  already covers phone-rastered fonts.
* **Input** stays below LVGL too: the raw wheel listener (done) and, for the
  button, an interceptor in the RayNeo button driver's chain
  (`rayneo_register_btn_event_interceptor`, walked by
  `rayneo_intercept_btn_event` on the button poll thread; the button is not a
  BES HAL key). Ours sits right behind the display manager's wake interceptor,
  so a press still wakes the glasses, and returns 1 for CLICK, DOUBLE_CLICK
  and HOLD_1S while the page is open: the launcher's key monitor never
  publishes them and the LVGL buttons indev never queues them.

Display driver, read from the stock image (2026-10-09, before the probe ran):

* LVGL's display is `lv_rayneo_fbdev_create` over a NuttX framebuffer device:
  `lv_rayneo_fbdev_set_file` opens it, reads `fb_videoinfo_s` (driver data
  +4: fmt, xres, yres, nplanes) and `fb_planeinfo_s` (+0xc), mmaps the plane
  (+0x28) and splits it into two LVGL draw buffers (+0x2c second buffer,
  +0x30 its row offset). `flush_cb` (0x10690373), only on the last area of a
  refresh, issues four ioctls on that fd: `0xfa` dirty area `{x,y,w,h}` (y in
  virtual rows), `0x106` the 0x8c-byte depth-layer-group bounds, `0x2818`
  FBIOPAN_DISPLAY with the planeinfo (yoffset = active buffer), `0x107`
  commit. Driver data +0x238 is `refr_pass` (`lv_rayneo_fbdev_set_refr_pass`):
  when set, flush_cb returns after `lv_display_flush_ready` without touching
  the device. That is the fbguard lease, provided by the stock driver.
* The fb driver's ioctl op (0x107566fd, cases 0xfa..0x107) hands the pan to
  `rayneo_display_if_exec_pan_display` → `rayneo_pandisp_exec` →
  `rayneo_lcdc_layer_update` on the display side (vsync-driven), which
  converts the RGB565 source to 4-bit gray (`rgb565_to_g4_align_mve`, into
  0x201e1fd0, 0x14a50 bytes = 302-byte rows × 280), clamps the per-eye column
  offsets (`rayneo_fb_get_left/right_parallax` → state 0x1832d4f8 +0xb7/+0xb8)
  and presents. With depth layer groups it soft-composes two eye buffers
  (`rayneo_lcdc_depth_soft_compose_*`) and calls
  `rayneo_lcdc_layer_dual_update_full(left)`, which pairs 0x202802c0 (left)
  with 0x2026b840 (right): layer buffer set per eye, chip-select 1 then 2,
  279 rows at LCDC width 0x5a (6 px units = 540 px) plus one trailing row.
  So the panel takes two independent 4-bit surfaces: G2-style per-element
  depth is possible by rendering both eyes ourselves and calling the dual
  update, no LVGL involved.
* The probe (`disp.c`, control 5, reply 0x19) reports all of the above live
  and tries both presents: mode 1 writes a ramp + block into the mmapped
  plane and replays flush_cb's ioctls; mode 2 fills the two eye buffers with
  the block at different x and calls the dual update. Mode 3/4 toggle
  refr_pass. The suite's `disp-probe` case runs mode 0.

Probe results on hardware (2026-10-09, build 840fd9a1…):

* Report: fb device fd 880, fmt 11 (RGB565), 544×220 virtual 544×440 (two
  LVGL buffers of 1088-byte rows at 0x201f6a40 / 0x20231140), draw area
  540×220, color format 0x12, left parallax −2, right 0, depth mode 0, disp
  mode 3 (the stock update takes the per-eye path). The page's 540×180
  canvas sits inside a 220-row panel; the G4 eye buffers are 280 rows.
* Framebuffer path (ramp + block, replaying flush_cb's ioctls): visible and
  stable, but a small black rectangle jittered left/right about 10 px at
  high rate on top of it. Cause: the launcher keeps invalidating a region
  under the page (a depth-registered widget), every stock flush recomposes
  that rectangle with its per-eye shift, and the eyes disagree there. With
  `refr_pass` set (probe mode 3) before the present the rectangle is gone.
  Decision: while the page is open the module sets refr_pass and owns every
  present; on close it clears it and invalidates the launcher screen.
* Dual path, first tries: nothing (no cache clean: the LCDC reads by DMA,
  `bes_lcdc_cache_sync(buf, 0x14a50)` is required after CPU writes), then a
  partial picture that the stock refresh overwrote within a fraction of a
  second even with refr_pass set. The display thread (`rayneo_display` →
  `rayneo_pandisp_exec` → `rayneo_lcdc_layer_update`) re-presents from the
  RGB565 framebuffer on its own cadence, not only on LVGL flushes, so the
  per-eye buffers are its scratch output. Hence the LCDC hook: both
  `bl rayneo_lcdc_layer_update` inside pandisp_exec (0x10755db2, 0x10755e68)
  are retargeted to `cfw_lcdc_hook`, which presents the module's eye buffers
  while `disp_own` is set and forwards to the stock otherwise. Eye buffer
  layout (`rayneo_lcdc_visible_row_ptr`): 280 rows × 302 bytes, each row
  16 B margin + 270 B pixels (540 px, 4 bpp, the LEFT pixel of a pair in
  the HIGH nibble: `rgb565_to_g4_align_mve`'s scalar tail packs
  `(p0 >> 3) & 0xf0 | (p1 >> 7) & 0xf`; the first compose had it backwards
  and swapped every column pair, which garbled text) + 16 B margin, the
  parallax a byte shift into the margins (`rayneo_lcdc_clamp_g4_xoffset`, ±16);
  rows 0..278 go out in one transfer and row 279 separately. The panel is
  540×280; the launcher draws 220 rows of it at a configurable y offset.
* Presenting the pair with `rayneo_lcdc_layer_dual_update_full` drifted a
  row per refresh (a rising bar) with the picture surviving only in the
  bottom 60 rows, with or without the frame-done wait. The stock never
  shows the pair that way: `rayneo_lcdc_update_self` (its re-present of the
  current buffers) runs `dual_update_partial(NULL = the pair, {0,0,90,220},
  state[0] = pan y offset)` when state +0xb4 (last kind) is 3 and +0xa2
  (composed) is set, then the frame-done wait. The hook now sets those two
  bytes and calls update_self.
* Inline text was invisible since the theme-less page root (revision 4):
  the label inherited LVGL's default black. Fixed by setting the label's
  text color to white (scene.c).

Status after the probe (2026-10-09, evening): the compose is in. Each lens
has an L8 540×220 canvas (scene.c renders per lens; records carry a lens
mask, replies go per lens; IDENT flag 0x08 announces it), disp.c converts
both canvases into the LCDC pair on the display thread and presents via the
stock self refresh, page.c parks the stock flush (refr_pass) while the page
is open and asks for presents with the framebuffer pan ioctls. Text is the
builtin bitmap font (tools/gen_font.py → patches/font_builtin.h, Menlo 16 px
in a 20 px cell, the cached-font table layout with raw rows): TEXT(15),
TEXT_INLINE(17) and the TEXT_CACHED fallback. No LVGL object draws anything;
the page root remains for key focus. The SDK keeps element depth and sends
per-lens SHOWs through G2CfwRenderer's mirrors on stereo firmware. Built,
not yet flashed.

In order:

1. Display probe build: done (see above).
2. Per-eye compose, a scene per lens, phone passes `depth` through: built.
3. Builtin bitmap font, page.c without LVGL drawing: built.
4. Button input via the button driver's interceptor chain: built (2026-10-09 late).
5. `vector.c`: PATH records (SVG) and ROTATE ops, with the heap workspace: built
   (2026-10-10, revision 6; the G2 compiler and rotation, filled through the
   anti-aliased coverage rows; tools/host/run_vector_tests.sh checks the
   g2flash pixel probes on the host).
6. Object cache parity: built (2026-10-09 night): 256 objects per lens,
   128 assets, a granule-bitmap store with free, LRU eviction (unreferenced
   assets first, then the least recently used object that is neither active
   nor pinned by the SHOW being applied), MISSING id lists, the [evicted]
   and RESET [evicted][store KiB] reply extras. The journal (STATE: UNCHANGED /
   DELTA / SNAPSHOT, DROP, PERIOD) followed on 2026-10-10 (revision 6, a
   64-entry ring per lens, asset events in both) and the texture options on
   2026-10-10 (inverse/transparent/ink through a LUT, colour tweens fade).
   Also built: anti-aliased geometry (geom.c: four sub-rows per pixel row,
   Q4 spans into a coverage row blended with the record's gray) and the
   g2flash shapes suite ported to tools/g2suite over the harness bridge
   (`rayneo-io bridge`): 43 cases, the same data and pipeline as on the G2.
7. The glasses' own screens (hint.c, 2026-10-10): the pairing view's title
   reads "RayNeo iO Glassly CFW v6" (BondView's two label updates) and its
   four hints plus DisconnectView's hint are Glassly text in English (the
   i18n GetText/RegisterLabel calls of those labels are retargeted; the stock
   cycles every language while unpaired, so an English string patch would
   not have covered it). Other keys fall through to the stock lookup.
8. Page on wake: built and verified (2026-10-10). page.c registers a display
   manager screen-state callback; on screen-on the page timer re-presents an
   open page. The page timer does not run while the glasses are link-idle, so
   a lease that ran out during sleep is noticed on wake: with a retained
   scene it is renewed in place for one more lease (no close, no flash of the
   stock home) and the page is announced open to the phone, whose session
   resumes keepalives or replaces the scene; a page the lease closed while
   awake is reopened on wake the same way. One takeover per phone session
   (a PAGE_OPEN or KEEPALIVE re-arms it), so a stale scene is never shown
   twice. Found on the way: the stock holds a screen wakelock while a phone
   link is up (no screen timeout then), `screen_power_force_off` changes the
   state without blanking the panel, and `rayneo_fb_get_power` is a
   framebuffer flag, not the panel. The probe report's tail carries the
   page/wake flags and DISPLAY_PROBE mode 8 is the (ineffective) force-off.
9. Direct-framebuffer modes (3/6, mode-8 bundles, mode 36): natural once the
   present path is ours; enable when the phone starts selecting them.
10. Compass: built (2026-10-10, compass.c; the sensor-hub stream handler,
   integer tilt compensation, midpoint hard-iron, change-only COMPASS
   reports, raw records for calibration, the axis mapping selectable from the
   control byte). The device's mapping was read off the raw stream on
   hardware (2026-10-10, worn: +x up tilted 26 degrees toward +y, +y forward,
   +z left, a right turn raises the heading) and is the device default
   (0x20); a mirrored first guess from a poorly fitted hard iron was caught
   by switching maps live through the control byte. The hard-iron estimate is the min/max midpoint since
   the stream started, so a session that begins near a magnet (a laptop)
   skews it until the wearer looks around. Background: the hardware has a TDK IMU plus a
   magnetometer on the sensor-hub core, reported to the AP over rpmsg
   (`sensor_rpmsg_register_mag_data_handler` / `_sync_imu_mag_data_handler`,
   `sensor_rpmsg_start_report_mag`, `tdk_mag_data_handler`). No fused
   orientation is exported on the AP (the G2 patch read the stock
   quaternion), so: subscribe to the synchronized IMU+mag stream while the
   phone wants headings, compute a tilt-compensated heading from accel+mag
   with a simple hard-iron calibration, report on change only as a new
   0xF0 record (COMPASS `[kind][heading u16 deci-degrees][accuracy u8]`),
   phone side like the G2 compass reports. `process_compass_info` is the
   factory self-test parser, not this path.
Not applicable: mic routing, R1 ring, ALS, BLE link profile, the Cordio
keyboard bridge, the protobuf settings channel (business 0xF0 and the stock
inflater replace them).
