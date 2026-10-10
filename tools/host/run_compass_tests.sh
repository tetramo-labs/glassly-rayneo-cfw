#!/bin/bash
# Host check of compass.c: integer atan2 accuracy, and headings from a captured session when given one.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/rayneo-compass-tests}
mkdir -p "$OUT"
clang -std=c11 -O1 -fsanitize=address,undefined -Wall -Wno-unused-function -Wno-unused-variable \
  -Ipatches -Ipatches/generated -o "$OUT/compass_test" tools/host/compass_test.c -lm
"$OUT/compass_test" atan2
if [ -n "$1" ]; then "$OUT/compass_test" "${2:-0}" < "$1"; fi
