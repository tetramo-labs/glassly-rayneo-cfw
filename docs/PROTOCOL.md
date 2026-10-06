# PROTOCOL — the Glassly CFW link, firmware view

This is the firmware side of the Glassly CFW link (business `0xF0`). It mirrors
the phone-side record/message/asset formats so the same encoders work for RayNeo
iO and for the Even Realities G2 CFW. Panel is 540×180, single display, 1-bit
(the firmware thresholds 4bpp gray `>= 8` to on; the L8 canvas writes 255/0).

## Layers

```
AA55 frame  (phone BLE transport)  -> payload after the business byte
  stream packets   [opt u8][chunk]            (3.1)   opt: 0x01 LENS, 0x80 RESET, 0x40 END, 0x10 CONTROL
    control        [0x10][ctrl u8]            (3.2)   1 QUERY 2 PAGE_OPEN 3 PAGE_CLOSE 4 KEEPALIVE
    record stream  [flags][len u16 LE][crc u16 LE][body]   (3.3 L1)
      flags: 0x01 LENS, 0x04 COMPRESSED (zlib SYNC_FLUSH, body ends 00 00 FF FF), 0x08 RESET_CONTEXT
      crc = CRC-16/CCITT-FALSE(poly 0x1021, init 0xFFFF) of the DECODED message
      messages (L2): [mode][epoch u16][request u16] body
        0x41 RESET  [41][epoch][request][00][cap]
        0x37 PUT    PUT_ASSET  [00][id u32][ver u32][kind u8][total u32][offset u32][len u16][data]
                    PUT_OBJECT [01][id u32][ver u32][shape record]
        0x38 SHOW   [38][epoch][request][flags][bg&0x0F][puts u8][puts entries][count u16][count×(id,ver)][ops]
        0x39 HIDE   [39][epoch][request][flags][bg]
```

Shape records (coords signed px on 540×180, clipped): `RECT(2)`/`RECT_FILL(3)`
20 B, `IMAGE(14)` 12 B, `TEXT_CACHED(16)` 16 B, `TEXT_INLINE(17)` 13+len. Image
asset = `[w u8][h u8][4bpp RLE]`; RLE tokens `[cnt<<4|c]`, `[c][cnt u8]`,
`[c][00][cnt u16]`. `ops` are always empty on RayNeo; no warm-up SHOW/TWEEN is
sent — the first cache message after RESET is a real PUT or SHOW.

## Replies (glasses → phone, business 0xF0, ≤192 B, one at a time ≥100 ms apart)

```
ACK   [01][stream][ordinal u16][lens=1][size u16][crc u16]
NACK  [03][stream][ordinal u16][lens][size u16][crc u16]
REPLY [05][stream][ordinal u16][lens][mode][request u16][status u8][epoch u16][revision u32]   (before the ACK)
GESTURE [07][eventType u8][source u8][tick u32 LE]
IDENT [11][ver u8][w u16 LE][h u16 LE][maxChunk u16 LE][storeKiB u16 LE][revision u16 LE][flags u8]"GLASSLYCFW-IO/1"
PAGE  [13][state u8][reason u8]
```

* `stream` = the **low byte of the RESET packet's 16-bit AA55 messageNumber**
  (per the finished phone implementation). In the v1 named-file downlink there is
  no messageNumber, so `stream` is 0; this is wired for the 0xF0 stream path.
* `ordinal` = per-stream message counter (reset at RESET).
* IDENT: `ver=1`, `w=540`, `h=180`, `maxChunk=240`, `storeKiB=64`, `revision=1`,
  `flags = 0x01`. Flag bits: **bit0** = zlib COMPRESSED records accepted (the
  stock inflater is used); **bit1** = raw stream packets accepted on business
  0xF0 (downlink). v1 advertises `0x01`: zlib yes, raw-0xF0 downlink **no** — the
  phone must use the named-file downlink below.
* REPLY `status`: 0 APPLIED, 1 STALE (epoch mismatch), 2 MISSING (unknown id/ver),
  3 CAPACITY, 4 REFUSED.
* GESTURE `eventType`: 0 single_tap, 1 swipe_up, 2 swipe_down, 3 double_tap,
  11 tap_then_hold; `source` 0 touchpad/crown, 1 button.
* PAGE `state` 1 open/0 closed; `reason` 0 ok, 1 launcher busy, 2 not paired,
  3 lease expired, 4 user backed out.

## Control / session rules the firmware enforces

* **QUERY → IDENT** is answerable without the page open and without
  `isBussinessIdle` (the phone classifies as stock after 2 s of silence).
* Cache messages (RESET/PUT/SHOW/HIDE) are **NACKed while the page is closed**.
* **PAGE_OPEN** gates on `paired() && isBussinessIdle()`. Failure replies
  `PAGE [0][reason]` (reason 1/2 are retried by the phone at 2/5/10 s); success
  replies `PAGE [1][0]`, arms the 120 s lease, and reports display activity.
* **KEEPALIVE** renews the 120 s lease (`PAGE [1][0]`). The lease expiring, a
  PAGE_CLOSE, or a >2 s long press close the page (`PAGE [0][reason]`).
* With IDENT `flags bit0 = 0` the phone would send every record uncompressed with
  RESET_CONTEXT only on the first; this build advertises bit0 = 1.

## Downlink paths

There are two downlink paths. Which one a firmware accepts is announced by IDENT
bit1; the uplink (replies) is always business 0xF0.

### Raw path (IDENT bit1 = 1) — NOT in v1

Stream packets `[opt u8][chunk]` as the payload of business-0xF0 AA55 frames,
one packet per frame; stream id = messageNumber of the RESET frame, consecutive
messageNumbers per stream; `stream` byte in ACK/NACK/REPLY = low byte of that
messageNumber. v1 does not implement this path (no verified business demux).

### Named-file path (IDENT bit1 = 0) — v1, exactly as the phone sends it

* The phone uploads a file named **`glassly-cfw.gcf`** over **business 18**
  (the stock file transfer). The firmware's file-receive hook delivers it once
  it is complete.
* File body = a sequence of blocks **`[opt u8][len u16 LE][chunk]`**. `opt`
  bits are identical to the raw path: `0x01 LENS` (always set), `0x80 RESET`,
  `0x40 END`, `0x10 CONTROL`. `chunk` is the stream chunk (≤ maxChunk) or, for
  CONTROL, the single control byte.
* **One whole stream (RESET … END) per file**, file ≤ **48000 bytes**. The
  firmware rejects larger files.
* A **control packet is a one-block file**: `[0x10][01 00][ctrl]`.
* There is no messageNumber on this path, so the **`stream` byte is 0** in
  ACK/NACK/REPLY.
* **QUERY is accepted on both paths** (as a one-block file here) and answered
  with IDENT without the page being open.

Replies for both paths: business 0xF0 via `rnlink_if_send_payload`, ≤ 192 B,
one at a time ≥ 100 ms apart. See `docs/HOOKS.md` "demux decision".
