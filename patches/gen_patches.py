#!/usr/bin/env python3
"""
Generate the committed Glassly CFW patch set (patches/cfw_patches.json) from the
stock RayNeo iO `nuttx_ap.bin`. This is the clang half of the build: it resolves
and verifies every firmware address, compiles the injected C module (build.py ->
clang), lays it out, and writes the offset/old/new patch list plus base and
output SHA-256s.

The resulting JSON is committed and is the source of truth; apply_patches.py
replays it with no compiler at all, so the exact reviewed firmware can be rebuilt
on any machine. A different clang version may produce different bytes — that is
why the JSON, not the compiler, is the committed artifact.

Usage:
  gen_patches.py              # stock AP from the cached OTA archive -> patches/cfw_patches.json
  gen_patches.py <base_ap>    # explicit stock AP
  gen_patches.py <base_ap> <out.json>
"""
import sys, os, json, hashlib, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402

REPO = os.path.dirname(HERE)
CACHE_ZIP = os.path.join(REPO, "cache", "StrixOS-1.0.4.12-STOCK-PAYLOADS-REPACK.zip")
DEFAULT_OUT = os.path.join(HERE, "cfw_patches.json")


def stock_ap():
    with zipfile.ZipFile(CACHE_ZIP) as z:
        return z.read("nuttx_ap.bin")


def main(argv):
    base_ap = open(argv[1], "rb").read() if (len(argv) >= 2 and argv[1]) else stock_ap()
    out_path = argv[2] if (len(argv) >= 3 and argv[2]) else DEFAULT_OUT

    print("resolving firmware symbols and compiling the injected module (build.py):")
    patched, ops, info = build.build_ap(base_ap)

    spec = {
        "base": "nuttx_ap.bin",
        "base_sha256": hashlib.sha256(base_ap).hexdigest(),
        "output_sha256": hashlib.sha256(patched).hexdigest(),
        "module_va": hex(info["module_va"]),
        "module_bytes": info["blob_len"],
        "patches": ops,
    }
    with open(out_path, "w") as f:
        json.dump(spec, f, indent=2)
        f.write("\n")

    total = sum(len(bytes.fromhex(op["new"])) for op in ops)
    print(f"wrote {out_path}: {len(ops)} patches, {total} bytes changed/appended")
    print(f"  module  {info['blob_len']} B @ {info['module_va']:#x}")
    print(f"  base    sha256 {spec['base_sha256']}")
    print(f"  output  sha256 {spec['output_sha256']}")
    print(f"  symbols verified: {len(info['resolved'])}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except build.BuildError as e:
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
