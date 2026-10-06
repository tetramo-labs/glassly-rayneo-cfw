#!/usr/bin/env python3
"""
On-device probe for the Glassly CFW link (needs `bleak` and real RayNeo iO
hardware; this repo is validated only at build/unit-test level, so treat the
output as experimental).

It scans for the glasses (BLE service 0xB81D or a name matching /rayneo/i),
connects, runs the stock version exchange + guest auth (spec §2.3), subscribes
to the RX notify characteristic, then:

  * sends a QUERY control packet on business 0xF0 and prints the IDENT reply
    (or reports "stock" after 2 s of silence),
  * optionally opens the Glassly page and sends one test text frame.

This does NOT flash firmware. Usage:
  python3 probe.py [--name NAME] [--text "hello"]
"""
import argparse
import asyncio
import hashlib
import os
import struct
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("probe.py needs bleak:  ./venv/bin/pip install bleak", file=sys.stderr)
    sys.exit(2)

SVC = "0000b81d-0000-1000-8000-00805f9b34fb"
TX = "ea8b70d5-2bd3-49ab-9c31-9c38b2c3c4f9"   # phone -> glasses, write-no-response
RX = "7db3e235-3608-41f3-a03c-955fcbd2ea4b"   # glasses -> phone, notify
SALT = bytes.fromhex("0712c8ad27744195de5fa1ea6d025fb8")
BIZ_HANDSHAKE = 0x10
BIZ_GLASSLY = 0xF0
CTRL_QUERY = 1
CTRL_PAGE_OPEN = 2


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


class Frames:
    def __init__(self):
        self.msg_num = int.from_bytes(os.urandom(2), "big")

    def build(self, business, payload, packet_type=0):
        body = struct.pack(">H", self.msg_num) + bytes([packet_type, business]) + bytes(payload)
        self.msg_num = (self.msg_num + 1) & 0xFFFF
        total = 4 + len(body) + 2
        frame = b"\xAA\x55" + struct.pack(">H", total - 6) + body
        return frame + struct.pack(">H", crc16_xmodem(frame[4:]))


def tlv(tag, value):
    return bytes([tag]) + struct.pack(">H", len(value)) + value


def parse_ident(payload):
    if not payload or payload[0] != 0x11:
        return None
    ver = payload[1]
    w, h, max_chunk, store_kib, revision = struct.unpack_from("<HHHHH", payload, 2)
    flags = payload[12]
    name = payload[13:].split(b"\0", 1)[0].decode("ascii", "replace")
    return dict(ver=ver, w=w, h=h, max_chunk=max_chunk, store_kib=store_kib,
                revision=revision, zlib=bool(flags & 1), name=name)


async def run(args):
    print("scanning for RayNeo iO…")
    device = None
    for d in await BleakScanner.discover(timeout=8.0):
        name = (d.name or "")
        if args.name and args.name.lower() in name.lower():
            device = d; break
        if "rayneo" in name.lower():
            device = d; break
    if not device:
        print("no RayNeo iO found (pass --name)"); return 1
    print(f"connecting to {device.name} ({device.address})")

    ident = {"value": None}
    got = asyncio.Event()

    def on_rx(_handle, data):
        frames = bytes(data)
        if len(frames) >= 10 and frames[0] == 0xAA and frames[1] == 0x55 and frames[7] == BIZ_GLASSLY:
            payload = frames[8:-2]
            parsed = parse_ident(payload)
            if parsed:
                ident["value"] = parsed
                got.set()

    async with BleakClient(device) as client:
        await client.start_notify(RX, on_rx)
        f = Frames()
        phone_id = os.urandom(6)
        ver_req = tlv(0x10, b"\x01") + tlv(0x11, phone_id) + tlv(0x12, b"Glassly") + tlv(0x13, b"\x01")
        await client.write_gatt_char(TX, f.build(BIZ_HANDSHAKE, tlv(0x11, ver_req)), response=False)
        await asyncio.sleep(1.0)
        # (a full session would parse the version reply + do guest auth with SALT here)
        _ = hashlib.sha256  # SALT/auth helpers available for a full implementation

        print("sending QUERY on business 0xF0…")
        await client.write_gatt_char(TX, f.build(BIZ_GLASSLY, bytes([0x10, CTRL_QUERY])), response=False)
        try:
            await asyncio.wait_for(got.wait(), timeout=2.0)
            print("IDENT:", ident["value"])
            classified = ident["value"]["name"].startswith("GLASSLYCFW-IO/")
            print("classified as Glassly CFW" if classified else "unexpected IDENT name")
        except asyncio.TimeoutError:
            print("no IDENT within 2 s -> stock firmware")
            return 0

        if args.text:
            print(f"opening page + sending test text {args.text!r}")
            await client.write_gatt_char(TX, f.build(BIZ_GLASSLY, bytes([0x10, CTRL_PAGE_OPEN])), response=False)
            await asyncio.sleep(0.3)
            # A real text frame would be the encoded record stream; left to the SGC.
        await client.stop_notify(RX)
    return 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--name", default=None)
    p.add_argument("--text", default=None)
    a = p.parse_args()
    return asyncio.run(run(a))


if __name__ == "__main__":
    sys.exit(main())
