#!/bin/bash
set -eu
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/rayneo-head-tests}
mkdir -p "$OUT"
clang -std=c11 -O1 -fsanitize=address,undefined -Wall -Wno-unused-function \
  -Ipatches -Ipatches/generated -o "$OUT/head_test" tools/host/head_test.c
"$OUT/head_test"
