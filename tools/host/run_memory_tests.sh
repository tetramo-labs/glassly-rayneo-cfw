#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/rayneo-memory-tests}
mkdir -p "$OUT"
clang -std=c11 -O2 -fno-builtin -fsanitize=address,undefined -Wall -Wextra -Wno-unused-function \
  -Ipatches -Ipatches/generated -o "$OUT/memory_test" tools/host/memory_test.c
"$OUT/memory_test"
