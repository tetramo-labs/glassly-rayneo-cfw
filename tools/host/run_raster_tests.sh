#!/bin/bash
# Host check of raster.c (the direct-framebuffer family) against pixel expectations.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/rayneo-raster-tests}
mkdir -p "$OUT"
clang -std=c11 -O1 -fsanitize=address,undefined -Wall -Wno-unused-function -Wno-unused-variable \
  -Ipatches -Ipatches/generated -o "$OUT/raster_test" tools/host/raster_test.c
"$OUT/raster_test"
