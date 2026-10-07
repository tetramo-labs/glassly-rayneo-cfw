# HOOKS — every patched address and why

The Glassly CFW module is appended after the stock `nuttx_ap.bin`, 32-byte
aligned, absolutely linked at `module_va = 0x10a972c0`, with the 8-byte
build-info footer (`1d3457be 00001930`) relocated to the new end. The patched AP
is 9,472,764 bytes (stock 9,466,560; ceiling 9,600,000; partition capacity
`0x9f0000`). Every firmware address the module calls is resolved by name from the
AP symbol table and verified byte-for-byte against the stock image at build time
(`patches/build.py`); the resolved set is written to `patches/generated/fw_syms.h`.

The machine-readable list is `patches/hooks.json`. Only `nuttx_ap.bin` changes.

## Verified patch sites (byte-for-byte against stock)

| Site (VA) | File off | Len | Stock bytes | New bytes | Purpose |
|---|---|---|---|---|---|
| `0x1079fe2c` | `0x60fe2c` | 2 | `dc 20` | `fc 20` | Enlarge the launcher `AppListView` allocation 0xdc→0xfc. The 32-byte tail at `app+0xdc` holds `{magic, ctx*}` — the CFW heap-context pointer. |
| `0x106d9d1a` | `0x549d1a` | 10 | `00 24 1f 48 20 4b 84 61 03 60` | `bl cfw_register_shim` + 3×`nop` | Point the launcher's extra file-receive callback slot at `cfw_file_rx` (CFW downlink). The shim reproduces the displaced `movs r4,#0` and the two stores, preserving the stock callback at `[0x19750d2c]` and installing ours at `[+0x18]`. |
| `0x10a972b8` | `0x9072b8` | 8 | `1d 34 57 be 00 00 19 30` | `00`×8 | Zero the old footer slot (relocated to the new end). |
| `0x10a972c0` | `0x9072c0` | 6204 | *(append)* | module blob + 4-align + footer | The CFW module + relocated footer. |

The `0x106d9d1a` shim additionally asserts, and fails the build on any mismatch,
the pointer constants that pin the mechanism against a rebase:
`[0x106d9d9c] = 0x19750d2c` (the file-sink struct), `[0x106d9da0] = 0x106d97f1`
(the stock file-complete callback, preserved), `[0x10799030] = 0x19a1f954`
(the launcher global used by the app-pointer chain).

## Verified firmware symbols (36)

All resolved by name and verified `(name_ptr, address)` at `tableOffset` plus the
name string at `name_ptr`. Highlights: `memalign`/`free`, `rnlink_if_send_payload`
(`0x10776171`, uplink), `message_processor_send` (`0x107d5135`) and
`message_processor_register_c_handler` (`0x107d4f61`), the LVGL v9 object/canvas/
label/event/timer API, `lv_display_get_default`/`lv_display_get_screen_active`,
`lv_rayneo_display_report_activity`, `LauncherAPI::isBussinessIdle` (`0x106d334d`),
`real_get_rnlink_paired_status` (`0x1077fa41`), and the zlib inflater
(`inflateInit2_ 0x10835ad1`, `inflate 0x10835b6d`, `inflateEnd 0x10836b4d`).

### zlib decision — PRESENT, used

The stock image exports a zlib inflater (the `0x10835xxx` cluster). The module
calls it, so IDENT advertises `flags bit0 = 1` (compressed records accepted). A
second inflate/inflateEnd pair exists at `0x10681xxx` with no matching init in
that cluster; it is deliberately not used (disambiguated by expected address in
`build.py`).

### demux decision — named-file fallback (spec §7)

This build environment has **no ARM disassembler** (no `capstone`, no ARM target
in the available `objdump`/`llvm-objdump`), so a generic RNLink business demux
could **not** be verified for a safe displaced-prologue entry hook as the spec
requires. v1 therefore takes the spec §7 fallback:

* **Downlink = the named-file channel (IDENT bit1 = 0).** The byte-verified
  file-sink shim routes a completed file `glassly-cfw.gcf` (business 18) to
  `cfw_file_rx`. Body = blocks `[opt u8][len u16 LE][chunk]`, opt bits identical
  to the raw path (0x01 LENS, 0x80 RESET, 0x40 END, 0x10 CONTROL); one whole
  stream (RESET…END) per file, ≤ 48000 B; a control packet is a one-block file;
  `stream` byte = 0 in ACK/NACK/REPLY. QUERY is accepted on both paths.
* **Raw 0xF0 downlink (IDENT bit1 = 1) is NOT implemented in v1.** IDENT
  advertises `flags = 0x01` (zlib yes, raw-0xF0 no).
* **Uplink** = `rnlink_if_send_payload(0xF0, …)` on both paths — spec-faithful.

**What the phone side must know:** read IDENT bit1. With bit1 = 0 (this build)
send the downlink as the `glassly-cfw.gcf` file over business 18 with the
framing above, not as 0xF0 stream packets; replies still arrive on 0xF0. The
exact framing is in `docs/PROTOCOL.md` "Downlink paths". Wiring a verified raw
0xF0 demux (then advertising bit1 = 1) is deferred to a device build.

## Unverified (behind clear comments; not claimed to work on hardware)

Hardware result so far (2026-10-07): the patched AP boots, the OTA is
confirmed, and the launcher answers general status. The first image stored
every module callback pointer even and one byte high (the resolver re-added
the Thumb bit that an ARM ELF `STT_FUNC` value already carries), so the file
sink and the UI handler could never be entered; `build.py` now strips the bit
first and asserts the stored `cfw_file_rx` address. Whether the file channel
reaches `cfw_file_rx` is still open.

No RayNeo iO hardware was available; nothing here is hardware-validated. Two
channels must be kept distinct when reading this list:

* **File channel (v1 downlink, IDENT bit1 = 0):** the hook site is byte-verified,
  but that a completed `glassly-cfw.gcf` upload over business 18 actually reaches
  `cfw_file_rx` through the `[0x19750d2c + 0x18]` slot is unverified on device.
* **Raw 0xF0 channel (IDENT bit1 = 1):** not implemented; no demux hook exists in
  this build. Nothing about it is claimed.

These specific points rest on the prior firmware analysis rather than on a check
this build can perform, and are guarded in the source:

1. **App-pointer chain** (`ctx.c`): `*(0x19a1f954)` → `+0x3c` manager → `+0x10`
   `AppListView`, state tail at `+0xdc`. The `0x19a1f954` pointer *constant* is
   byte-verified; the `+0x3c`/`+0x10`/`+0xdc` offsets are from prior analysis and
   are only dereferenced after null checks (a wrong offset yields "no context",
   not a fault that bricks boot — but could still misbehave on device).
2. **UI message ABI** (`link.c`): `message_processor_register_c_handler` and
   `message_processor_send` signatures, and that the registered handler receives
   the `TIONativeMessage*`. The `TIONativeMessage`/`TIONativeFile` layouts are
   from the stock file bridge in the prior analysis.
3. **Crown/rotary input** (`input.c`): whether a full-screen page receives the
   crown as an LVGL rotary event (`LV_EVENT_ROTARY`, ~600 raw units/detent). The
   key click/long-press path uses the documented `LV_EVENT_KEY` codes 0x3a/0x3b.
4. **ResourcePool::GetFont(20)** is resolved-but-unused: the overload ABI (four
   same-named entries) could not be verified, so inline/cached text uses
   positioned `lv_label`s with the default LVGL font instead.
5. **Carousel item** ("Glassly" launcher entry) is **deferred**: the native menu
   method-replacement addresses require disassembly verification not possible
   here. v1 ships the phone-initiated page open only (PAGE_OPEN, gated on
   `paired() && isBussinessIdle()`).
