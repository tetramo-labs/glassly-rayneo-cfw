# SAFETY — read before flashing

**This firmware has never run on a RayNeo iO.** It is verified only at the
compile and unit-test level. Flashing it is experimental and can brick your
glasses. If a phone-side change can do what you need, do that instead.

## What is and is not verified

Verified on this machine:

* Every firmware address the module calls is resolved by name from the AP symbol
  table and checked byte-for-byte against the stock image (`patches/build.py`).
* Every patched byte is a verified in-place edit of known stock bytes, or the
  appended module; only `nuttx_ap.bin` changes, and it stays under the
  9,600,000-byte ceiling with the build-info footer relocated intact.
* The OTA archive repacks deterministically and the protocol codecs pass tests.

**Not** verified (no hardware was available):

* That the patched image boots, that the launcher file-receive hook delivers the
  `glassly-cfw.gcf` channel, that the UI message ABI and app-pointer chain are
  correct, that the LVGL page renders, or that gestures/crown forward. See the
  "Unverified" section of `docs/HOOKS.md`.
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
business 0xF0 / the named-file channel) plus gesture forwarding. No animation,
vectors, keyboard, ANCS, head gestures, or carousel item. It does not touch the
stock businesses, the OTA flow, audio, or the radio.
