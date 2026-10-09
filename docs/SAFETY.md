# SAFETY — read before flashing

**Hardware status (2026-10-09):** revisions 1 and 2 flashed over the Glassly
App's OTA flow, booted, reported `OtaValidationConfirmed` and reconnected; the
launcher kept answering general-status requests (business 15, the same RNLink
registry path the module uses). Neither answered the phone's QUERY. Revision 3
removes the two remaining unverified links in that chain (the launcher-view
state pointer and the LVGL hop before IDENT) and adds a DIAG reply; it has not
been flashed. Flashing is still experimental and can brick your glasses. If a
phone-side change can do what you need, do that instead.

## What is and is not verified

Verified on this machine:

* Every firmware address the module calls is resolved by name from the AP symbol
  table and checked byte-for-byte against the stock image (`patches/build.py`).
* Every patched byte is a verified in-place edit of known stock bytes, or the
  appended module; only `nuttx_ap.bin` changes, and it stays under the
  9,600,000-byte ceiling with the build-info footer relocated intact.
* The OTA archive repacks deterministically and the protocol codecs pass tests.

**Not** verified (no hardware was available):

* That business-0xF0 frames reach the registered table on the RNLink thread,
  that the app-pointer chain is correct, that the LVGL page renders, or that
  gestures/crown forward. See the "Unverified" section of `docs/HOOKS.md`.
* There is **no verified recovery path**. If the glasses do not boot after
  flashing, they may not be able to accept another OTA.

## If you flash anyway

* Charge the glasses to **≥ 50 %** first. The OTA flow also refuses below 50 %.
* **Never interrupt** the flash: keep the phone next to the glasses, do not let
  either sleep, do not close the app, do not power anything off until the glasses
  reboot and reconnect and report `OtaValidationConfirmed` + `idle`.
* Keep the matching stock OTA package (`rayneo-io-stock-1.0.4.12.ota.zip`,
  produced by the same build) so you can attempt to reflash stock. This is *not*
  a guaranteed recovery — it only works if the device still accepts an OTA.
* The firmware changes behavior only after the launcher is up and a Glassly
  message arrives; it allocates no state and runs no code on the boot path beyond
  the one enlarged allocation and the file-sink callback install. That is a
  deliberate risk-reduction choice, not a safety guarantee.

## Scope

v1 adds one thing: the Glassly page (a 540×180 overlay driven by the phone over
business 0xF0) plus gesture forwarding. No animation,
vectors, keyboard, ANCS, head gestures, or carousel item. It does not touch the
stock businesses, the OTA flow, audio, or the radio.
