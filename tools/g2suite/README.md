# g2suite — the g2flash shapes suite on the RayNeo iO

The G2 Glassly CFW's on-device test suite (`g2flash/demos/shapes-suite.ts`:
the example miniapp's shapes cases, run through a port of the phone's scene
pipeline and object-cache encoder), pointed at a RayNeo iO on the Glassly
CFW revision 5. The cases, the pipeline and the encoder are the G2 files;
only the session differs: instead of g2-kit's BLE session the suite talks to
the Mac harness's bridge socket.

```
cd ../../../glassly/mobile/modules/bluetooth-sdk/tools/rayneo-io-mac
swift build && .build/debug/rayneo-io bridge          # connects, listens on 127.0.0.1:7310

cd ~/dev/glassly-rayneo-cfw/tools/g2suite
bun shapes-suite.ts                 # every case
bun shapes-suite.ts rect animation  # by group
bun shapes-suite.ts anim-glide      # by id
bun shapes-suite.ts --list
G2_DRY_RUN=1 bun shapes-suite.ts    # host pipeline only, no glasses
G2_TRACE=1 bun shapes-suite.ts      # per-frame op counts and timings
bun vector-suite.ts                 # SVG paths and rotation (vector-cases.ts), revision 6
bun cache-state.ts                  # the journal: STATE, DROP, PERIOD (revision 6)
bun raster-replay.ts <dump dir>     # the phone driver's raster keyframe + delta (revision 7)
bun anim-timing.ts                  # glide settle times vs nominal, with the firmware's frame statistics (revision 8)
```

Files:

* `shapes-suite.ts` — the G2 suite with `openLink()` on the bridge, the
  540×220 canvas, no panel origin and the 20 px builtin font's line height.
  Keep it diffable against the g2flash original.
* `object-cache.ts` — the encoder, verbatim from g2flash.
* `rayneo-transport.ts` — the G2 `cfw-transport.ts` over the bridge: the
  same record layout, ACK gate (every targeted lens acks) and `CacheLink`.
* `vector-protocol.ts`, `vector-cases.ts` — verbatim from g2flash (the SVG
  path compiler and the revision 38 fixtures); `vector-suite.ts` runs the
  fixtures over the bridge (no Bad Apple playback). The probes are host
  side: `../host/run_vector_tests.sh` rasterizes them with the firmware's own
  vector.c and geom.c.
* `cache-state.ts` — journal checks (not in g2flash, which only has
  `cache-probe.ts`): STATE answers, DROP, PERIOD, the 64-entry ring overflow.
* `raster-replay.ts` — the direct-framebuffer family as the phone drives it:
  replays the mode-6 keyframe and mode-3 delta the SDK's `G2CfwRenderer`
  encodes for the RayNeo panel (dumped by its `G2CfwRendererTests` with
  `TEST_RUNNER_RAYNEO_RASTER_DUMP` in xcodebuild's environment), then a
  scene SHOW taking the panel back, a raster takeover again, and HIDE.

Differences the verdicts know about: the RayNeo iO has no image path in
this port (as on the G2 port), its canvas is 540×220, and revision 5 keeps
a scene per lens exactly as the two G2 lenses do, so lens handling needs no
change. The vector fixtures keep the G2's 576-wide coordinates, so a few
reach below the 220-row panel.
