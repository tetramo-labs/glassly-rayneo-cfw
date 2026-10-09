# HOOKS — every patched address and why

The Glassly CFW module is appended after the stock `nuttx_ap.bin`, 32-byte
aligned, absolutely linked at `module_va = 0x10a972c0`, with the 8-byte
build-info footer (`1d3457be 00001930`) relocated to the new end. The patched AP
is 9,473,400 bytes (stock 9,466,560; ceiling 9,600,000; partition capacity
`0x9f0000`). Every firmware address the module calls is resolved by name from the
AP symbol table and verified byte-for-byte against the stock image at build time
(`patches/build.py`); the resolved set is written to `patches/generated/fw_syms.h`.

The machine-readable list is `patches/hooks.json`. Only `nuttx_ap.bin` changes.

## Verified patch sites (byte-for-byte against stock)

| Site (VA) | File off | Len | Stock bytes | New bytes | Purpose |
|---|---|---|---|---|---|
| `0x106d9d1a` | `0x549d1a` | 10 | `00 24 1f 48 20 4b 84 61 03 60` | `bl cfw_register_shim` + 3×`nop` | Inside `LinkMonitor::registerLinkCallback`, just before the launcher registers its business-15 callback table at `0x19750d2c`. The shim calls `cfw_boot_register` (which registers the module's own table for business 0xF0 with `rnlink_if_register_callback`) and then reproduces the displaced `movs r4,#0` and the two stores unchanged, so slot `[+0x18]` stays NULL as in stock. |
| `0x10a972b8` | `0x9072b8` | 8 | `1d 34 57 be 00 00 19 30` | `00`×8 | Zero the old footer slot (relocated to the new end). |
| `0x10a972c0` | `0x9072c0` | 6840 | *(append)* | module blob + 4-align + footer | The CFW module + relocated footer. |

The build additionally asserts, and fails on any mismatch, the constants that
pin the mechanism against a rebase: `[0x106d9d9c] = 0x19750d2c` (the launcher's
business-15 callback table), `[0x106d9da0] = 0x106d97f1` (its stock slot-0
callback, preserved), and the RNLink registry literal
`[0x10776460] = [0x107763d8] = 0x1987c780` (loaded by
`rnlink_if_register_callback` and `rnlink_if_on_data_recv`), which is emitted
to the C as `FWA_rnlink_registry`.

### State pointer — the module's own RNLink registry entry (verified 2026-10-08)

The module has no `.data`/`.bss`, so its one pointer to the heap context needs
a writable word the stock image owns. Since revision 3 that word is the `sub`
field (+0x20) of the module's own entry in the RNLink callback registry: the
registrar copies the 32-byte table into a free 0x2c-byte entry, stores `sub`
at +0x20, the table pointer at +0x24 and the business byte at +0x28. Every
dispatcher (`rnlink_if_on_data_recv`, `_on_conn_state`, `_on_bond_state`,
`_on_multipoint_conn_state`, `_on_device_info`, `_on_send_done`,
`_on_file_recv`) keys on +0x24 and calls a slot only when it is non-NULL; the
`sub` word is read only by the registrar's duplicate check and by
`rnlink_if_unregister_callback` as a match key. `ctx.c` finds the entry whose
+0x24 is `&cfw_rn_cbs` and keeps `ctx*` at its +0x20. Because the stored
pointer defeats the duplicate check, `cfw_boot_register` skips registration
when the entry already exists (`LinkMonitor::doStartMonitoring` can call
`registerLinkCallback` more than once).

Revisions 1–2 kept the pointer in a 32-byte tail added to the launcher's
`AppListView` (allocation 0xdc→0xfc at `0x1079fe2c`, reached through
`*(0x19a1f954)` → `+0x3c` → `+0x10`). Disassembly shows `Launcher::Launcher`
does store `this` at `0x19a1f954` and `ViewManager::ensureAppListView` keeps the
view at `+0x10`, but the view is created lazily from
`ViewManager::onEnterState`/`eventHandler`, so no context existed until the
app list had been shown. That hook is gone.

## Verified firmware symbols (37)

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

Hardware result so far (2026-10-09): the patched AP boots, the OTA is
confirmed, and the launcher answers general status. Revision 1 never answered
IDENT: its callback pointers were even (fixed in `build.py`), it used an
invalid message-processor module id, and its uplink was only drained once a
page existed. Revision 2 (all of that fixed) also stayed silent; its remaining
unverified dependency was the AppListView state pointer, which disassembly
later showed is created lazily. Revision 3 has not been flashed yet; its DIAG
reply reports the registry entry, context, module id and return codes.

1. **Whether the shim runs at all.** `registerLinkCallback` is reached (the
   launcher's business-15 table answers on hardware), but no on-device trace of
   the module's code has been observed yet. The next flash either logs
   `Glassly CFW detected` plus a DIAG, or shows the hook is not executing.
2. **Lazy handler registration** races the LVGL thread's handler lookup (an
   unordered_map insert from the RNLink thread). It happens once per boot, on the
   first business-0xF0 frame; `message_processor_init` ordering relative to the
   shim is unknown, so registration is not attempted at boot.
3. **Crown/rotary input** (`input.c`): whether a full-screen page receives the
   crown as an LVGL rotary event (`LV_EVENT_ROTARY`, ~600 raw units/detent). The
   key click/long-press path uses the documented `LV_EVENT_KEY` codes 0x3a/0x3b.
4. **ResourcePool::GetFont(20)** is resolved-but-unused: the overload ABI (four
   same-named entries) could not be verified, so inline/cached text uses
   positioned `lv_label`s with the default LVGL font instead.
5. **Carousel item** ("Glassly" launcher entry) is **deferred**: the native menu
   method-replacement addresses need disassembly verification. Pages open only
   on PAGE_OPEN, gated on `paired() && isBussinessIdle()`.
