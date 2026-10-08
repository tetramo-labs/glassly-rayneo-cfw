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
| `0x106d9d1a` | `0x549d1a` | 10 | `00 24 1f 48 20 4b 84 61 03 60` | `bl cfw_register_shim` + 3×`nop` | Inside `LinkMonitor::registerLinkCallback`, just before the launcher registers its business-15 callback table at `0x19750d2c`. The shim calls `cfw_boot_register` (which registers the module's own table for business 0xF0 with `rnlink_if_register_callback`) and then reproduces the displaced `movs r4,#0` and the two stores unchanged, so slot `[+0x18]` stays NULL as in stock. |
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
calls it, so IDENT advertises `flags bit0 = 1`. A second inflate/inflateEnd pair
at `0x10681xxx` has no matching init and is deliberately not used.

### Downlink — RNLink business table (verified by disassembly, 2026-10-07)

`rnlink_if_register_callback(cbs, sub, business)` (`0x107763dd`) copies a
32-byte callback table into a 24-entry registry at `0x1987c780` (entry 0x2c
bytes: table copy, `sub` @0x20, `cbs*` @0x24, business byte @0x28). Slot 0 is
`data(bus, data, len)`, called by `rnlink_if_on_data_recv` for every entry whose
business byte matches, with no business filtering upstream except the handshake
(0x10) and 0x11. The launcher registers tables for 15 and 18 from
`LinkMonitor::registerLinkCallback`; the module registers `{cfw_rx_data, 0…}`
for 0xF0 from the shim in that same routine. The AA55 messageNumber is not
passed to the callback, hence the `stream` byte in every packet
(`docs/PROTOCOL.md`).

`cfw_rx_data` (RNLink thread) deep-copies the payload and hands it to the LVGL
thread with `message_processor_send`: module ids must be 1..15 (`getModuleName`
table: 1 LAUNCHER … 15 RNLINK_CHANNEL); 3 PHONE and 5 MUSIC have no stock
handler, and a taken id is refused with -3 without side effects, so the module
registers 5, then 3, lazily on the first frame. The queued record copies
`{id, data, bytes, reserved, context, mode}` and the C handler is called as
`handler(msg, user)` from `message_processor_handle`, an LVGL timer created by
`message_processor_init` — the LVGL thread. LVGL is built without OS locking,
so timers are only ever created from that handler.

The stock file transfer is not usable as a downlink: business-18 payloads reach
`onBigCoreFileRecvCallback`, which only logs, and the phone's INFO is never
acknowledged (observed on hardware on stock and revision 1 alike).

## Unverified (behind clear comments; not claimed to work on hardware)

Hardware result so far (2026-10-07): the patched AP boots, the OTA is
confirmed, and the launcher answers general status. Revision 1 never answered
IDENT: its callback pointers were even (fixed in `build.py`), it used an
invalid message-processor module id, and its uplink was only drained once a
page existed. Revision 2 has not been flashed yet.

1. **App-pointer chain** (`ctx.c`): `*(0x19a1f954)` → `+0x3c` manager → `+0x10`
   `AppListView`, state tail at `+0xdc`. The `0x19a1f954` pointer *constant* is
   byte-verified; the `+0x3c`/`+0x10`/`+0xdc` offsets are from prior analysis and
   are only dereferenced after null checks (a wrong offset yields "no context").
2. **Lazy handler registration** races the LVGL thread's handler lookup (an
   unordered_map insert from the RNLink thread). It happens once per boot, on the
   first business-0xF0 frame; a boot-time registration needs the AppListView,
   which does not exist yet when the shim runs.
3. **Crown/rotary input** (`input.c`): whether a full-screen page receives the
   crown as an LVGL rotary event (`LV_EVENT_ROTARY`, ~600 raw units/detent). The
   key click/long-press path uses the documented `LV_EVENT_KEY` codes 0x3a/0x3b.
4. **ResourcePool::GetFont(20)** is resolved-but-unused: the overload ABI (four
   same-named entries) could not be verified, so inline/cached text uses
   positioned `lv_label`s with the default LVGL font instead.
5. **Carousel item** ("Glassly" launcher entry) is **deferred**: the native menu
   method-replacement addresses need disassembly verification. Pages open only
   on PAGE_OPEN, gated on `paired() && isBussinessIdle()`.
