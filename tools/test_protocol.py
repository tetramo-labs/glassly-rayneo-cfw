#!/usr/bin/env python3
"""
Protocol unit tests for the Glassly CFW link (business 0xF0), runnable with
`python3 -m pytest tools/`. Pure stdlib + pytest.

Covered:
  * CRC-16/XMODEM (AA55 frame CRC) + the spec frame test vector
  * AA55 transport frame encode/decode
  * CRC-16/CCITT-FALSE (record decoded-CRC)
  * 4bpp RLE round trip (Python encoder <-> a port of the firmware decoder)
  * the record accumulator + stream reassembly, and the RESET/PUT/SHOW object
    cache, against a Python model of the firmware state machine
"""
import struct
import pytest


# ------------------------------- CRCs ---------------------------------------
def crc16_xmodem(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


# --------------------------- AA55 transport frame ---------------------------
def build_frame(msg_num, business, payload, packet_type=0):
    # frame = AA 55 [len BE = total-6] [msgNum BE] [packetType] [business] [payload] [CRC BE]
    body = struct.pack(">H", msg_num) + bytes([packet_type, business]) + bytes(payload)
    total = 2 + 2 + len(body) + 2
    frame = b"\xAA\x55" + struct.pack(">H", total - 6) + body
    frame += struct.pack(">H", crc16_xmodem(frame[4:]))
    return frame


def parse_frame(frame):
    assert frame[0] == 0xAA and frame[1] == 0x55
    length = struct.unpack(">H", frame[2:4])[0]
    assert length == len(frame) - 6
    assert crc16_xmodem(frame[4:-2]) == struct.unpack(">H", frame[-2:])[0]
    msg_num = struct.unpack(">H", frame[4:6])[0]
    packet_type = frame[6]
    business = frame[7]
    payload = frame[8:-2]
    return msg_num, packet_type, business, bytes(payload)


def test_crc16_xmodem_check_value():
    assert crc16_xmodem(b"123456789") == 0x31C3


def test_crc16_ccitt_false_check_value():
    assert crc16_ccitt_false(b"123456789") == 0x29B1


def test_frame_spec_vector():
    # Spec §2.2: AA 55 00 07 12 34 00 2A 01 02 FE 7E D3
    frame = build_frame(0x1234, 0x2A, b"\x01\x02\xFE")
    assert frame == bytes.fromhex("aa5500071234002a0102fe7ed3")


def test_frame_roundtrip():
    frame = build_frame(0xBEEF, 0xF0, bytes(range(20)))
    msg, ptype, biz, payload = parse_frame(frame)
    assert (msg, ptype, biz, payload) == (0xBEEF, 0, 0xF0, bytes(range(20)))


# ------------------------------- 4bpp RLE -----------------------------------
def rle_encode(pixels):
    """pixels: list of 4-bit values in wire (row-major, nibble) order.
    Emits tokens matching the firmware decoder: [cnt<<4|c] (1..15),
    [c][cnt] (c high nibble 0, 1..255), [c][0][cnt lo][cnt hi] (>=256)."""
    out = bytearray()
    i = 0
    n = len(pixels)
    while i < n:
        c = pixels[i]
        run = 1
        while i + run < n and pixels[i + run] == c and run < 0xFFFF:
            run += 1
        i += run
        while run:
            if run <= 15:
                out.append((run << 4) | c); run = 0
            elif run <= 255:
                out.append(c); out.append(run); run = 0
            else:
                take = min(run, 0xFFFF)
                out.append(c); out.append(0)
                out.append(take & 0xFF); out.append((take >> 8) & 0xFF)
                run -= take
    return bytes(out)


def rle_decode(tokens, rowbytes, rows):
    """Port of the firmware rle_feed/rle_emit into a rowbytes*rows 4bpp buffer."""
    buf = bytearray(rowbytes * rows)
    left = rowbytes * rows * 2
    r = 0
    bpos = 0
    hi = True

    def emit(v, cnt):
        nonlocal r, bpos, hi, left
        assert cnt <= left, "RLE overrun"
        left -= cnt
        while cnt:
            assert r < rows, "row overrun"
            idx = r * rowbytes + bpos
            if not hi:
                buf[idx] = (buf[idx] & 0xF0) | v
                bpos += 1; hi = True; cnt -= 1
            else:
                buf[idx] = (buf[idx] & 0x0F) | (v << 4)
                hi = False; cnt -= 1
            if hi and bpos >= rowbytes:
                bpos = 0; r += 1

    st = 0
    color = 0
    cnt = 0
    for b in tokens:
        if st == 0:
            color = b & 0x0F
            c = b >> 4
            if c:
                emit(color, c)
            else:
                st = 1
        elif st == 1:
            if b:
                emit(color, b); st = 0
            else:
                st = 2
        elif st == 2:
            cnt = b; st = 3
        else:
            cnt |= b << 8
            assert cnt != 0
            emit(color, cnt); st = 0
    return bytes(buf)


def pixels_to_4bpp(pixels, rowbytes, rows):
    buf = bytearray(rowbytes * rows)
    for idx, v in enumerate(pixels):
        r, bpos, hi = idx // (rowbytes * 2), (idx % (rowbytes * 2)) // 2, (idx % 2 == 0)
        byte = r * rowbytes + bpos
        if hi:
            buf[byte] = (buf[byte] & 0x0F) | (v << 4)
        else:
            buf[byte] = (buf[byte] & 0xF0) | v
    return bytes(buf)


@pytest.mark.parametrize("w,h", [(8, 4), (16, 16), (7, 3), (255, 2)])
def test_rle_roundtrip(w, h):
    import random
    random.seed(w * 100 + h)
    rowbytes = (w + 1) // 2
    total_nibbles = rowbytes * h * 2
    # a run-friendly pattern plus random noise
    pixels = []
    while len(pixels) < total_nibbles:
        v = random.randint(0, 15)
        pixels.extend([v] * random.randint(1, 40))
    pixels = pixels[:total_nibbles]
    tokens = rle_encode(pixels)
    decoded = rle_decode(tokens, rowbytes, h)
    assert decoded == pixels_to_4bpp(pixels, rowbytes, h)


def test_rle_long_run():
    pixels = [0xA] * 1000
    rowbytes = len(pixels) // 2
    tokens = rle_encode(pixels)
    assert rle_decode(tokens, rowbytes, 1) == pixels_to_4bpp(pixels, rowbytes, 1)


# ------------------- record + message state-machine model -------------------
MSG_RESET, MSG_PUT, MSG_SHOW, MSG_HIDE = 41, 37, 38, 39
PUT_ASSET, PUT_OBJECT = 0x00, 0x01
ST_APPLIED, ST_STALE, ST_MISSING, ST_CAPACITY, ST_REFUSED = 0, 1, 2, 3, 4
REC_LENS, REC_COMPRESSED, REC_RESET_CTX = 0x01, 0x04, 0x08
PKT_LENS, PKT_CONTROL, PKT_END, PKT_RESET = 0x01, 0x10, 0x40, 0x80


class FirmwareModel:
    """Mirror of link.c + scene.c: record reassembly + object cache, no zlib."""

    def __init__(self, page_open=True):
        self.buf = bytearray()
        self.page_open = page_open
        self.epoch = None
        self.objects = {}        # id -> (version, type)
        self.active = []
        self.replies = []        # (kind, ...)
        self.stream_id = 0       # echoed in ACK/NACK/REPLY

    def feed_packet(self, payload):
        """One business-0xF0 frame payload: [opt][stream][chunk]."""
        assert len(payload) >= 2
        self.feed_block(payload[0], payload[2:], stream=payload[1])

    def feed_block(self, opt, chunk, stream=0):
        if opt & PKT_CONTROL:
            return
        if opt & PKT_RESET:
            self.buf = bytearray()
            self.stream_id = stream
        self.buf += chunk
        while len(self.buf) >= 5:
            rlen = self.buf[1] | (self.buf[2] << 8)
            total = 5 + rlen
            if len(self.buf) < total:
                break
            self._record(bytes(self.buf[:total]))
            del self.buf[:total]
        if opt & PKT_END:
            self.buf = bytearray()

    def _record(self, wire):
        flags = wire[0]
        rlen = wire[1] | (wire[2] << 8)
        crc = wire[3] | (wire[4] << 8)
        body = wire[5:5 + rlen]
        if not (flags & REC_LENS):
            self.replies.append(("NACK",)); return
        if crc16_ccitt_false(body) != crc:
            self.replies.append(("NACK",)); return
        if not self.page_open:
            self.replies.append(("NACK",)); return
        self._message(body)

    def _message(self, msg):
        mode = msg[0]
        epoch = msg[1] | (msg[2] << 8)
        request = msg[3] | (msg[4] << 8)
        body = msg[5:]
        if mode == MSG_RESET:
            self.epoch = epoch
            self.objects.clear(); self.active = []
            self.replies.append(("REPLY", mode, request, ST_APPLIED)); self.replies.append(("ACK",))
            return
        if epoch != self.epoch:
            self.replies.append(("REPLY", mode, request, ST_STALE)); self.replies.append(("ACK",)); return
        if mode == MSG_PUT:
            kind = body[0]
            if kind == PUT_OBJECT:
                oid = struct.unpack("<I", body[1:5])[0]
                ver = struct.unpack("<I", body[5:9])[0]
                shape = body[9]
                self.objects[oid] = (ver, shape)
                self.replies.append(("REPLY", mode, request, ST_APPLIED)); self.replies.append(("ACK",))
            else:
                self.replies.append(("REPLY", mode, request, ST_APPLIED)); self.replies.append(("ACK",))
            return
        if mode == MSG_HIDE:
            self.active = []
            self.replies.append(("REPLY", mode, request, ST_APPLIED)); self.replies.append(("ACK",)); return
        if mode == MSG_SHOW:
            puts = body[2]
            off = 3
            for _ in range(puts):
                elen = body[off] | (body[off + 1] << 8); off += 2
                entry = body[off:off + elen]; off += elen
                if entry[0] == PUT_OBJECT:
                    oid = struct.unpack("<I", entry[1:5])[0]
                    ver = struct.unpack("<I", entry[5:9])[0]
                    self.objects[oid] = (ver, entry[9])
            count = body[off] | (body[off + 1] << 8); off += 2
            new_active = []
            status = ST_APPLIED
            for _ in range(count):
                oid = struct.unpack("<I", body[off:off + 4])[0]
                ver = struct.unpack("<I", body[off + 4:off + 8])[0]
                off += 8
                if oid not in self.objects or self.objects[oid][0] != ver:
                    status = ST_MISSING; break
                new_active.append(oid)
            if status == ST_APPLIED:
                self.active = new_active
            self.replies.append(("REPLY", mode, request, status)); self.replies.append(("ACK",))


def make_record(flags, body):
    return bytes([flags]) + struct.pack("<H", len(body)) + struct.pack("<H", crc16_ccitt_false(body)) + body


def msg(mode, epoch, request, body=b""):
    return bytes([mode]) + struct.pack("<HH", epoch, request) + body


def rect_object(oid, ver, gray=0xF):
    rec = bytes([2, 1, gray, 1]) + struct.pack("<8h", 0, 0, 100, 40, 0, 0, 0, 0)
    return bytes([PUT_OBJECT]) + struct.pack("<II", oid, ver) + rec


def test_reset_put_show_applies():
    fw = FirmwareModel(page_open=True)
    epoch = 0x1234
    recs = make_record(REC_LENS | REC_RESET_CTX, msg(MSG_RESET, epoch, 1, b"\x00\xf0"))
    recs += make_record(REC_LENS, msg(MSG_PUT, epoch, 2, rect_object(0x11, 1)))
    show_body = bytes([0x01, 0x00, 0]) + struct.pack("<H", 1) + struct.pack("<II", 0x11, 1)
    recs += make_record(REC_LENS, msg(MSG_SHOW, epoch, 3, show_body))
    # deliver split across two blocks to exercise reassembly
    mid = len(recs) // 2
    fw.feed_block(PKT_LENS | PKT_RESET, recs[:mid])
    fw.feed_block(PKT_LENS | PKT_END, recs[mid:])
    assert fw.active == [0x11]
    assert ("NACK",) not in fw.replies
    replies = [r for r in fw.replies if r[0] == "REPLY"]
    assert all(r[3] == ST_APPLIED for r in replies)


def test_show_missing_when_version_stale():
    fw = FirmwareModel(page_open=True)
    epoch = 7
    fw.feed_block(PKT_LENS | PKT_RESET, make_record(REC_LENS, msg(MSG_RESET, epoch, 1, b"\x00\xf0")))
    fw.feed_block(PKT_LENS, make_record(REC_LENS, msg(MSG_PUT, epoch, 2, rect_object(0x22, 1))))
    show_body = bytes([0x01, 0x00, 0]) + struct.pack("<H", 1) + struct.pack("<II", 0x22, 99)
    fw.feed_block(PKT_LENS | PKT_END, make_record(REC_LENS, msg(MSG_SHOW, epoch, 3, show_body)))
    assert fw.active == []
    assert ("REPLY", MSG_SHOW, 3, ST_MISSING) in fw.replies


def test_cache_message_nacked_when_page_closed():
    fw = FirmwareModel(page_open=False)
    fw.feed_block(PKT_LENS | PKT_RESET | PKT_END, make_record(REC_LENS, msg(MSG_RESET, 1, 1, b"\x00\xf0")))
    assert ("NACK",) in fw.replies
    assert fw.epoch is None


def test_bad_record_crc_nacks():
    fw = FirmwareModel(page_open=True)
    body = msg(MSG_RESET, 1, 1, b"\x00\xf0")
    wire = bytes([REC_LENS]) + struct.pack("<H", len(body)) + struct.pack("<H", 0xDEAD) + body
    fw.feed_block(PKT_LENS | PKT_RESET | PKT_END, wire)
    assert fw.replies == [("NACK",)]


def test_stale_epoch_rejected():
    fw = FirmwareModel(page_open=True)
    fw.feed_block(PKT_LENS | PKT_RESET, make_record(REC_LENS, msg(MSG_RESET, 5, 1, b"\x00\xf0")))
    fw.feed_block(PKT_LENS | PKT_END, make_record(REC_LENS, msg(MSG_PUT, 6, 2, rect_object(1, 1))))
    assert ("REPLY", MSG_PUT, 2, ST_STALE) in fw.replies


if __name__ == "__main__":
    import sys
    sys.exit(pytest.main([__file__, "-v"]))
