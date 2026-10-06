# Glassly CFW for RayNeo iO

Custom firmware ("Glassly CFW") for the **RayNeo iO** smart glasses
(Bestechnic BES2800, NuttX-based Strix OS 1.0.4.12). It patches the stock
application image `nuttx_ap.bin` to add one thing: a Glassly link on BLE business
`0xF0` that drives a 540×180 on-glass page from the phone (the Glassly App) and
forwards touch/crown gestures back. It is the firmware counterpart of the
phone-side RayNeo iO support in the Glassly repo.

**Revision: `GLASSLYCFW-IO/1`.**

> ⚠️ **Unverified on hardware.** This firmware has never run on a RayNeo iO. It
> is verified only at the compile/unit-test level. Flashing can brick the device
> and there is no verified recovery path. Read [`docs/SAFETY.md`](docs/SAFETY.md)
> first. The protocol knowledge here comes from prior firmware analysis of the
> stock image; the object cache and RLE code are derived from the sibling Even
> Realities G2 Glassly CFW sources.

## What it does

* Adds a business-`0xF0` handler: the Glassly CFW record protocol (the same
  record / message / asset formats the G2 Glassly CFW uses) plus a small control
  channel (QUERY/IDENT, PAGE_OPEN/CLOSE, KEEPALIVE).
* Object cache (RESET/PUT/SHOW/HIDE) with image/string/font assets, 4bpp RLE,
  and RECT/RECT_FILL/IMAGE/TEXT_CACHED/TEXT_INLINE shape records, rendered into a
  540×180 L8 canvas (1-bit panel: gray ≥ 8 → on) plus positioned `lv_label`s for
  text.
* A phone-initiated page with a 120 s lease and a 100 ms service timer, gated on
  `paired() && isBussinessIdle()`.
* Gesture forwarding: click → single_tap, two clicks < 300 ms → double_tap, long
  press → tap_then_hold, crown detents → swipe_up/down.
* IDENT identity probe so the phone can classify the firmware and learn the
  canvas size, chunk size, store size and whether compressed records are
  accepted.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the wire format and
[`docs/HOOKS.md`](docs/HOOKS.md) for every patched address, the zlib decision,
the transport/demux decision, and the explicitly **unverified** assumptions.

## Build

```
./build_cfw.sh
```

This creates `./venv`, downloads and verifies the pinned stock OTA archive into
`./cache`, applies the committed patch set (`patches/cfw_patches.json`) to
`nuttx_ap.bin`, verifies the patched-AP hash, repacks **both** the CFW and the
stock OTA archives deterministically, and writes `build/pins.json`.

Outputs in `build/`:

| File | What |
|---|---|
| `rayneo-io-glassly-cfw-1.0.4.12.ota.zip` | the flashable Glassly CFW OTA package |
| `rayneo-io-stock-1.0.4.12.ota.zip` | the stock OTA, repacked by the same tool |
| `pins.json` | sizes + SHA-256 of both packages, the AP SHA-256/MD5, the revision |

The patch set is committed as JSON and applied with **pure-stdlib Python**
(`patches/apply_patches.py`), so the build reproduces the pinned image with or
without a clang cross-compiler. When clang is present, `build_cfw.sh` also
regenerates the patch set and checks it still matches the committed one. Running
the build twice produces byte-identical archives (the repack is deterministic —
members are stored uncompressed with fixed timestamps, which trades a larger zip
for cross-machine reproducibility of the pinned SHA-256).

### Regenerate the patch set after editing the C

```
./build_cfw.sh --update-patches     # needs a clang that targets thumbv8m.main
```

`patches/build.py` compiles the single-TU module, resolves its internal
relocations with no cross linker (absolute `MOVW/MOVT`, `ABS32`, intra-module
`BL/B.W`), appends it 32-byte aligned after the stock image, and relocates the
build-info footer. Every firmware address is resolved by name from the AP symbol
table (`patches/symbols.json`) and verified against the stock bytes; a mismatch
fails the build.

## Flash

Flash through the **Glassly App → Settings → Custom firmware** screen, which runs
the OTA flow (business 9) and bundles both the CFW and the matching stock image
from `build/`. Do not flash by hand.

**Before flashing:** charge to ≥ 50 %, keep the phone beside the glasses, and
never interrupt the transfer. See [`docs/SAFETY.md`](docs/SAFETY.md) — there is
no verified recovery path.

## Test

```
./venv/bin/python -m pytest tools/            # protocol codecs + state machine
./venv/bin/python patches/apply_patches.py --verify   # patch replay round-trip
```

## Repo layout

```
build_cfw.sh            full build: download, verify, apply, repack, pin
requirements.txt        deps for the tests + on-device probe (bleak, pytest)
patches/
  patches_main.c        single translation unit; includes the modules below
  cfw.h                 shared types, verified firmware-call wrappers, forward decls
  mem.c rle.c rle.h     freestanding mem*, 4bpp RLE (from the G2 Glassly CFW)
  ctx.c                 heap singleton via the AppListView tail (app+0xdc)
  scene.c               object cache + shape records + L8 render
  link.c                business 0xF0: records, zlib, control, uplink; file-sink shim
  page.c input.c ident.c  page lifecycle/lease, gesture capture, IDENT/PAGE replies
  symbols.json          AP symbol table (name_ptr, address, tableOffset)
  build.py              clang -> single blob, no-linker reloc resolver, AP splice
  gen_patches.py        build.py -> committed cfw_patches.json
  apply_patches.py      pure-stdlib replay of cfw_patches.json (+ --verify)
  cfw_patches.json      committed patch set (the source of truth)
  hooks.json            every patched address, length, displaced bytes, purpose
tools/
  ota_pack.py           deterministic OTA (un)packer; rewrites the AP manifest row
  probe.py              bleak: scan, version exchange, QUERY -> IDENT, test frame
  test_protocol.py      pytest: CRCs, AA55 frame, RLE, packet/record/cache model
docs/                   PROTOCOL.md, HOOKS.md, SAFETY.md
cache/ build/ venv/     gitignored
```

## License

MIT (see [`LICENSE`](LICENSE)), matching the Glassly App. The stock firmware
image this repo patches is RayNeo's and is never redistributed here.
