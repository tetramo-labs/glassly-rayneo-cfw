#!/bin/bash
# Host check of scene.c's cached-image and builtin-font blits (pixel order, odd widths, texture options).
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/rayneo-image-tests}
mkdir -p "$OUT"
clang -std=c11 -O1 -fsanitize=address,undefined -Wall -Wno-unused-function -Wno-unused-variable \
  -Ipatches -Ipatches/generated -o "$OUT/image_test" tools/host/image_test.c
"$OUT/image_test"
