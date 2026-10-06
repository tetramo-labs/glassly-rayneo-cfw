#!/usr/bin/env python3
"""
Deterministic (un)packer for the RayNeo iO Strix OS 1.0.4.12 OTA archive.

The OTA archive is a zip of `OtaFileInfo.json` (a 14-row manifest) plus 14
payload files. Only `nuttx_ap.bin` differs between the stock firmware and the
Glassly CFW, so repacking the CFW archive means: swap in the patched AP, rewrite
that one manifest row's `Size`/`Md5`, and re-emit every other member byte for
byte, in the original order.

"Deterministic" means the same inputs always produce the same output bytes: all
members are stored uncompressed (ZIP_STORED, so no zlib-version dependence),
timestamps are fixed to the zip epoch (1980-01-01), and external attributes are
zeroed. This lets the Glassly App pin a SHA-256 for both the stock and CFW
archives it bundles.

Stdlib only.

Usage:
  ota_pack.py verify       <ota.zip>
  ota_pack.py repack-stock <in.zip> <out.zip>
  ota_pack.py repack-cfw   <in.zip> <patched_ap.bin> <out.zip>

`verify` checks every member against its manifest Size/Md5 and prints hashes.
`repack-stock` re-emits the archive deterministically (unchanged content).
`repack-cfw` additionally replaces nuttx_ap.bin with <patched_ap.bin>.
All three print the output/AP SHA-256 and the AP MD5.
"""
import sys, os, json, hashlib, zipfile, io, struct

AP_NAME = "nuttx_ap.bin"
MANIFEST = "OtaFileInfo.json"
ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)


def _md5(b):
    return hashlib.md5(b).hexdigest()


def _sha256(b):
    return hashlib.sha256(b).hexdigest()


class OtaError(Exception):
    pass


def read_archive(path):
    """Return (order, members, manifest) where `order` is the member name list in
    archive order, `members` maps name -> bytes, and `manifest` is the parsed
    OtaFileInfo.json list."""
    order = []
    members = {}
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            order.append(info.filename)
            members[info.filename] = z.read(info.filename)
    if MANIFEST not in members:
        raise OtaError(f"{path}: missing {MANIFEST}")
    manifest = json.loads(members[MANIFEST])
    return order, members, manifest


def manifest_row(manifest, name):
    for row in manifest:
        if row.get("Name") == name:
            return row
    raise OtaError(f"manifest has no row for {name}")


def verify_members(order, members, manifest):
    """Check each payload's bytes against its manifest Size/Md5. Raises OtaError on
    the first mismatch. OtaFileInfo.json itself has no manifest row."""
    names = [n for n in order if n != MANIFEST]
    rows = {row["Name"] for row in manifest}
    if set(names) != rows:
        raise OtaError(f"payload set {sorted(names)} != manifest set {sorted(rows)}")
    for name in names:
        row = manifest_row(manifest, name)
        b = members[name]
        if len(b) != row["Size"]:
            raise OtaError(f"{name}: size {len(b)} != manifest {row['Size']}")
        got = _md5(b)
        if got != row["Md5"]:
            raise OtaError(f"{name}: md5 {got} != manifest {row['Md5']}")


def serialize_manifest(manifest):
    """Byte image of the manifest. Matches the upstream convention used by the
    stock repack tool: UTF-8, 2-space indent, trailing newline."""
    return (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def write_archive(order, members, out_path):
    """Write `members` to `out_path` in `order`, deterministically: stored (no
    compression), fixed epoch timestamp, zero external attributes."""
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_STORED) as z:
        for name in order:
            info = zipfile.ZipInfo(filename=name, date_time=ZIP_EPOCH)
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0
            info.create_system = 0
            z.writestr(info, members[name])


def repack(in_path, out_path, patched_ap=None):
    """Repack `in_path` to `out_path`. If `patched_ap` (bytes) is given, swap it in
    for nuttx_ap.bin and rewrite that manifest row's Size/Md5. Returns a dict of
    output/AP hashes."""
    order, members, manifest = read_archive(in_path)
    verify_members(order, members, manifest)  # refuse to repack a tampered input

    if patched_ap is not None:
        members[AP_NAME] = patched_ap
        row = manifest_row(manifest, AP_NAME)
        row["Size"] = len(patched_ap)
        row["Md5"] = _md5(patched_ap)
        members[MANIFEST] = serialize_manifest(manifest)
    else:
        # Re-emit the manifest through the same serializer so stock and CFW
        # archives are byte-identical except for the AP row.
        members[MANIFEST] = serialize_manifest(manifest)

    write_archive(order, members, out_path)

    out_bytes = open(out_path, "rb").read()
    ap = members[AP_NAME]
    return {
        "file": os.path.basename(out_path),
        "size": len(out_bytes),
        "sha256": _sha256(out_bytes),
        "ap_sha256": _sha256(ap),
        "ap_md5": _md5(ap),
    }


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd = argv[1]
    try:
        if cmd == "verify":
            order, members, manifest = read_archive(argv[2])
            verify_members(order, members, manifest)
            ap = members[AP_NAME]
            print(f"  ok: {len(order)} members, {len(manifest)} manifest rows, all Size/Md5 verified")
            print(f"  {AP_NAME}: {len(ap)} bytes sha256 {_sha256(ap)} md5 {_md5(ap)}")
            return 0
        if cmd == "repack-stock":
            r = repack(argv[2], argv[3], patched_ap=None)
        elif cmd == "repack-cfw":
            ap = open(argv[3], "rb").read()
            r = repack(argv[2], argv[4], patched_ap=ap)
        else:
            print(f"unknown command: {cmd}", file=sys.stderr)
            return 2
    except (IndexError,) as e:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    except (OtaError, FileNotFoundError, zipfile.BadZipFile) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"  wrote {r['file']} ({r['size']} bytes)")
    print(f"  zip  sha256 {r['sha256']}")
    print(f"  ap   sha256 {r['ap_sha256']}")
    print(f"  ap   md5    {r['ap_md5']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
