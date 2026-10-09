# rayneo-io-suite — on-device acceptance run

The checklist to go through after every flash, as code: a Swift command that
connects to the glasses from a Mac and drives the Glassly CFW link with the
Glassly App SDK's own codecs, case by case, with a verdict per case. It is the
RayNeo iO counterpart of the G2 `shapes-suite` / `vector-suite`.

It builds against a sibling `glassly` checkout: `Sources/rayneo-io-suite/`
symlinks the SDK's `RayNeoIOProtocol.swift`, `RayNeoIOFlasher.swift`,
`G2CfwTransport.swift` and `BinaryCodec.swift`, plus the SDK's Mac harness
driver (`mobile/modules/bluetooth-sdk/tools/rayneo-io-mac`: `Link.swift`,
`Bridge.swift`, `Watcher.swift`). Keep `~/dev/glassly` and this repo side by
side, or point the links elsewhere. The harness itself flashes:

```bash
cd ../../../glassly/mobile/modules/bluetooth-sdk/tools/rayneo-io-mac
swift build && .build/debug/rayneo-io flash cfw      # build/rayneo-io-glassly-cfw-*.ota.zip, ~11 min
```

Then run the suite:

```bash
cd tools/suite
swift run rayneo-io-suite                 # every case, in order (~1.5 min + the 15 s hold)
swift run rayneo-io-suite cfw page        # by group
swift run rayneo-io-suite ident stale     # by id (connect is implied)
swift run rayneo-io-suite --list
swift run rayneo-io-suite --hold 60 --out results.json --verbose
```

Groups and cases, in run order:

| group | id | what passes |
|---|---|---|
| link | connect | scan, connect, version exchange, guest auth, launcher status |
| link | version | OTA version reply: `1.0.4.12`, confirmed, idle |
| link | battery | launcher general status carries a battery level |
| cfw | ident | QUERY → IDENT in 2 s: `GLASSLYCFW-IO/`, revision ≥ expected, flags raw+zlib+context, 540×180 |
| cfw | diag-rx | DIAG source 1: entry inside the RNLink registry, 8-aligned heap context |
| cfw | diag-ui | DIAG source 2: UI module 5 or 3, rc 0, service timer present |
| cfw | singleton | three more QUERYs report the same context pointer |
| cfw | stability | link up for `--hold` seconds; tick advances with wall time (±30 %) |
| scene | nack-closed | a cache message with the page closed is NACKed |
| page | open | PAGE_OPEN granted (retries while the launcher is busy) |
| page | keepalive | KEEPALIVE answered `PAGE [1][0]` |
| scene | reset | RESET → REPLY applied + ACK |
| scene | show-rect | inline filled rectangle applied (visible 2 s) |
| scene | show-text | inline text next to it applied (visible 2 s) |
| scene | geometry-fill | filled circle, triangle, quad, pie, rounded rect applied (visible 2 s) |
| scene | geometry-stroke | hairline + wide line, ring, outlines, two beziers, arc, rounded border applied (visible 2 s) |
| scene | compressed | zlib-compressed SHOW inflated and applied |
| scene | missing | SHOW of an unknown object → MISSING |
| scene | stale | wrong epoch → STALE |
| scene | bad-crc | wrong record CRC → NACK |
| scene | hide | HIDE applied |
| page | close | PAGE_CLOSE → closed, reason 0 |
| page | reopen | opens and closes again |
| cfw | settle | link still up afterwards |

Not covered: the ANCS relay (`patches/ancs.c`). macOS serves no ANCS, so it
can only be exercised from an iPhone running the Glassly App (watch for
`RAYNEO: CFW ANCS` / `phone_notification` in its log). The suite's watcher
does print any ANCS record that arrives.

Exit status is 0 only when every selected case passed. Hardware findings the
suite now guards against are recorded in `docs/HOOKS.md` (registry lookup,
message-queue ownership, the paired gate).
