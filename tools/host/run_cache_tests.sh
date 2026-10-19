#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/rayneo-cache-tests}
mkdir -p "$OUT"
clang -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
  -Ipatches -Ipatches/generated -o "$OUT/cache_test" tools/host/cache_test.c
"$OUT/cache_test"
