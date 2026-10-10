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
```

Files:

* `shapes-suite.ts` — the G2 suite with `openLink()` on the bridge, the
  540×220 canvas, no panel origin and the 20 px builtin font's line height.
  Keep it diffable against the g2flash original.
* `object-cache.ts` — the encoder, verbatim from g2flash.
* `rayneo-transport.ts` — the G2 `cfw-transport.ts` over the bridge: the
  same record layout, ACK gate (every targeted lens acks) and `CacheLink`.

Differences the verdicts know about: the RayNeo iO has no image path in
this port (as on the G2 port), its canvas is 540×220, and revision 5 keeps
a scene per lens exactly as the two G2 lenses do, so lens handling needs no
change. The vector suite (SVG paths, rotation) waits for `vector.c`.
