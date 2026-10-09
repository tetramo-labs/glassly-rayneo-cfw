#!/usr/bin/env bash
#
# build_cfw.sh — one-shot build of the Glassly CFW OTA packages for RayNeo iO.
#
#   1. create a venv (./venv) and install the test/probe deps
#   2. download + verify the pinned stock OTA archive into ./cache
#   3. apply the committed patch set (patches/cfw_patches.json) to nuttx_ap.bin
#   4. verify the patched AP hash
#   5. (if clang is present) regenerate the patch set and check it still matches
#      the committed one — a reproducibility check; never required
#   6. repack BOTH the CFW and the stock OTA archives deterministically
#   7. write build/pins.json and print every pinned hash
#
# The patch set is committed as JSON and applied with pure-stdlib Python
# (patches/apply_patches.py), so the pinned output is reproduced regardless of
# whether a clang cross-compiler is installed. Running this twice produces
# byte-identical archives (the repack is deterministic).
#
# WARNING: this only BUILDS images. Flashing custom firmware onto the glasses
# is UNVERIFIED on hardware and can brick the device — read README.md/docs/SAFETY.md.
#
# Usage:
#   ./build_cfw.sh                  # full build
#   ./build_cfw.sh --skip-venv      # don't touch the venv
#   ./build_cfw.sh --force-download # re-download the stock archive
#   ./build_cfw.sh --update-patches # regenerate & OVERWRITE patches/cfw_patches.json (needs clang)
set -euo pipefail

# ---- pinned config ----------------------------------------------------------
STOCK_URL="https://github.com/Turbo1123/Turbo-IO/releases/download/stock-strix-1.0.4.12-archive/StrixOS-1.0.4.12-STOCK-PAYLOADS-REPACK.zip"  # stock payload archive
STOCK_ZIP="cache/StrixOS-1.0.4.12-STOCK-PAYLOADS-REPACK.zip"
STOCK_ZIP_SHA256="54eb155b1b9980c0334005c2051720879204b6b5448180ee594eb735b92e905a"
AP_STOCK_SHA256="53afdf5298815849eafca6f315a70606d2a605aff2e563f79050f797d615a988"
AP_CFW_SHA256="061443175bbe196496b28a7ab8b50b0c788d3a38a0b71f0a0af3b0681a88be18"
PATCH_JSON="patches/cfw_patches.json"
CFW_AP="build/nuttx_ap.cfw.bin"
CFW_ZIP="build/rayneo-io-glassly-cfw-1.0.4.12.ota.zip"
STOCK_OUT_ZIP="build/rayneo-io-stock-1.0.4.12.ota.zip"
REVISION="GLASSLYCFW-IO/4"

SKIP_VENV=0; FORCE_DOWNLOAD=0; UPDATE_PATCHES=0
for arg in "$@"; do
  case "$arg" in
    --skip-venv) SKIP_VENV=1 ;;
    --force-download) FORCE_DOWNLOAD=1 ;;
    --update-patches) UPDATE_PATCHES=1 ;;
    -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
    *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")"
say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  OK\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m   !\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

sha256() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else die "no sha256 tool (need shasum or sha256sum)"; fi
}
verify() { local got; got="$(sha256 "$1")"; [ "$got" = "$2" ] || die "$3 hash mismatch
    expected $2
    got      $got"; }
download() {
  if command -v curl >/dev/null 2>&1; then curl -fL --retry 3 --progress-bar -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then wget -O "$2" "$1"
  else die "no downloader (need curl or wget)"; fi
}
clang_ok() {
  command -v clang >/dev/null 2>&1 || return 1
  local t; t="$(mktemp -d)"; printf 'int f(int x){return x+1;}\n' > "$t/p.c"
  local rc=0
  clang --target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mthumb -ffreestanding -c "$t/p.c" -o "$t/p.o" >/dev/null 2>&1 || rc=1
  rm -rf "$t"; return $rc
}

PY=python3

# ---- 1. venv ----------------------------------------------------------------
if [ "$SKIP_VENV" -eq 1 ]; then
  say "skipping venv setup (--skip-venv)"
else
  command -v python3 >/dev/null 2>&1 || die "python3 not found"
  [ -d venv ] || { say "creating virtualenv (./venv)"; python3 -m venv venv; }
  say "installing deps (bleak, pytest)"
  ./venv/bin/python -m pip install --quiet --upgrade pip
  ./venv/bin/python -m pip install --quiet -r requirements.txt
  PY=./venv/bin/python
  ok "venv ready — run tests with: $PY -m pytest tools/"
fi

mkdir -p cache build

# ---- 2. download + verify stock archive -------------------------------------
if [ "$FORCE_DOWNLOAD" -eq 0 ] && [ -f "$STOCK_ZIP" ] && [ "$(sha256 "$STOCK_ZIP")" = "$STOCK_ZIP_SHA256" ]; then
  say "stock archive present and verified ($STOCK_ZIP)"
else
  say "downloading stock payload archive"
  download "$STOCK_URL" "$STOCK_ZIP"
  verify "$STOCK_ZIP" "$STOCK_ZIP_SHA256" "stock archive"
fi
ok "stock archive verified"

# ---- verify the archive's members (manifest Size/Md5, AP sha) ---------------
"$PY" tools/ota_pack.py verify "$STOCK_ZIP"

# ---- 5. optional reproducibility check with clang ---------------------------
[ -f "$PATCH_JSON" ] || [ "$UPDATE_PATCHES" -eq 1 ] || die "committed $PATCH_JSON missing and --update-patches not given"
if [ "$UPDATE_PATCHES" -eq 1 ]; then
  clang_ok || die "--update-patches needs a working clang thumbv8m cross-compiler"
  say "regenerating committed patch set with clang"
  "$PY" patches/gen_patches.py
  ok "patch set regenerated — review & commit $PATCH_JSON"
elif clang_ok; then
  say "clang present — regenerating to a temp file to check reproducibility"
  TMP="$(mktemp)"
  if "$PY" patches/gen_patches.py "" "$TMP" >/dev/null 2>&1; then
    if diff -q "$PATCH_JSON" "$TMP" >/dev/null 2>&1; then ok "clang reproduces the committed patch set exactly"
    else warn "clang output differs (likely a clang-version difference) — using the committed $PATCH_JSON"; fi
  else warn "clang regeneration failed — using the committed $PATCH_JSON"; fi
  rm -f "$TMP"
else
  warn "no clang cross-compiler — skipping regeneration (committed $PATCH_JSON is applied as-is)"
fi

# ---- 3/4. apply committed patch set + verify the patched AP -----------------
say "applying committed patch set -> $CFW_AP"
"$PY" patches/apply_patches.py          # extracts the stock AP from the cached archive
verify "$CFW_AP" "$AP_CFW_SHA256" "patched AP"
ok "patched AP verified ($AP_CFW_SHA256)"

# ---- 6. repack both OTA archives deterministically --------------------------
say "repacking CFW OTA archive"
"$PY" tools/ota_pack.py repack-cfw "$STOCK_ZIP" "$CFW_AP" "$CFW_ZIP"
say "repacking stock OTA archive"
"$PY" tools/ota_pack.py repack-stock "$STOCK_ZIP" "$STOCK_OUT_ZIP"

# ---- 7. pins.json -----------------------------------------------------------
say "writing build/pins.json"
"$PY" - "$CFW_ZIP" "$STOCK_OUT_ZIP" "$CFW_AP" "$REVISION" <<'PYEOF'
import sys, os, json, hashlib
cfw_zip, stock_zip, cfw_ap, revision = sys.argv[1:5]
def h(p): b=open(p,"rb").read(); return len(b), hashlib.sha256(b).hexdigest()
def md5(p): return hashlib.md5(open(p,"rb").read()).hexdigest()
cs, csh = h(cfw_zip); ss, ssh = h(stock_zip); aps, apsh = h(cfw_ap)
pins = {
  "stock": {"file": os.path.basename(stock_zip), "size": ss, "sha256": ssh},
  "cfw":   {"file": os.path.basename(cfw_zip),   "size": cs, "sha256": csh},
  "ap_sha256": apsh, "ap_md5": md5(cfw_ap), "revision": revision,
}
json.dump(pins, open("build/pins.json","w"), indent=2); open("build/pins.json","a").write("\n")
print(json.dumps(pins, indent=2))
PYEOF

echo
ok "done — OTA packages in build/"
echo "    CFW:   $CFW_ZIP"
echo "    stock: $STOCK_OUT_ZIP"
echo "    pins:  build/pins.json"
echo "    Flash via the Glassly App or the SDK Mac harness, then run tools/suite; see docs/SAFETY.md."
