#!/usr/bin/env python3
"""
Apply the committed Glassly CFW patch set (patches/cfw_patches.json) to the stock
RayNeo iO `nuttx_ap.bin`. This is the compiler-free half of the build: no clang,
no C sources, nothing but the Python standard library, so it runs anywhere.

The patch JSON is produced by gen_patches.py (which needs clang) and committed to
git, so the exact reviewed image can be rebuilt on any machine without
reproducing clang's byte-for-byte output.

Usage:
  apply_patches.py                      # apply to the cached stock AP -> build/nuttx_ap.cfw.bin
  apply_patches.py --verify             # apply + check hashes + idempotency round-trip, no output
  apply_patches.py <base_ap> <out_ap>   # apply to an explicit stock AP

By default the stock AP is extracted from the cached OTA archive
(cache/StrixOS-1.0.4.12-STOCK-PAYLOADS-REPACK.zip). The patch JSON schema:

  {"base": "nuttx_ap.bin", "base_sha256": "...", "output_sha256": "...",
   "patches": [{"offset": int, "old": "hex", "new": "hex", "desc": "..."}, ...]}

Each patch writes `new` at `offset` after checking the stock bytes there equal
`old` (a sanity + double-apply guard). A patch with empty `old` appends/extends at
the end of the file (offset must equal the current length).
"""
import sys, os, json, hashlib, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PATCH_JSON = os.path.join(HERE, "cfw_patches.json")
CACHE_ZIP = os.path.join(REPO, "cache", "StrixOS-1.0.4.12-STOCK-PAYLOADS-REPACK.zip")
AP_MEMBER = "nuttx_ap.bin"


class ApplyError(Exception):
    pass


def _sha256(b):
    return hashlib.sha256(b).hexdigest()


def stock_ap_from_cache():
    if not os.path.exists(CACHE_ZIP):
        raise ApplyError(f"cached stock archive not found: {CACHE_ZIP}\n"
                         "    run ./build_cfw.sh once to download it, or pass an explicit base AP")
    with zipfile.ZipFile(CACHE_ZIP) as z:
        return z.read(AP_MEMBER)


def apply_ops(img, ops):
    buf = bytearray(img)
    for i, op in enumerate(ops):
        off = op["offset"]
        old = bytes.fromhex(op.get("old", ""))
        new = bytes.fromhex(op["new"])
        tag = f"patch #{i} @ {off:#x}" + (f" ({op['desc']})" if op.get("desc") else "")
        if old:
            cur = bytes(buf[off:off + len(old)])
            if cur == new and cur != old:
                continue  # already applied (idempotent)
            if cur != old:
                raise ApplyError(f"{tag}: expected {old.hex()} found {cur.hex()} "
                                 "(input is not the stock base, or partially patched)")
            buf[off:off + len(new)] = new
        else:
            end = off + len(new)
            if off <= len(buf) and bytes(buf[off:end]) == new and end <= len(buf):
                continue  # already applied
            if off != len(buf):
                raise ApplyError(f"{tag}: append offset {off:#x} != length {len(buf):#x}")
            buf.extend(new)
    return bytes(buf)


def load_spec():
    return json.load(open(PATCH_JSON))


def apply(base, spec):
    if spec.get("base_sha256") and _sha256(base) != spec["base_sha256"]:
        raise ApplyError(f"base image hash mismatch\n    expected {spec['base_sha256']}\n    got      {_sha256(base)}")
    out = apply_ops(base, spec["patches"])
    if spec.get("output_sha256") and _sha256(out) != spec["output_sha256"]:
        raise ApplyError(f"patched image hash mismatch\n    expected {spec['output_sha256']}\n    got      {_sha256(out)}")
    return out


def main(argv):
    verify = "--verify" in argv
    argv = [a for a in argv if a != "--verify"]
    try:
        spec = load_spec()
        if len(argv) >= 3:
            base = open(argv[1], "rb").read()
            out_path = argv[2]
        else:
            base = stock_ap_from_cache()
            out_path = os.path.join(REPO, "build", "nuttx_ap.cfw.bin")
        print(f"  base verified ({_sha256(base)[:16]}…)")
        out = apply(base, spec)
        if verify:
            # Idempotency round-trip: replaying onto the already-patched image must
            # be a no-op and reproduce the same bytes.
            again = apply_ops(out, spec["patches"])
            if again != out:
                raise ApplyError("idempotency round-trip failed (re-apply changed the image)")
            print(f"  applied {len(spec['patches'])} patches, output sha256 verified")
            print(f"  idempotent re-apply reproduces the image ({len(out)} bytes)")
            print("  VERIFY OK")
            return 0
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        open(out_path, "wb").write(out)
        print(f"  applied {len(spec['patches'])} patches -> {out_path} ({len(out)} bytes)")
        return 0
    except (ApplyError, FileNotFoundError, KeyError, zipfile.BadZipFile) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
