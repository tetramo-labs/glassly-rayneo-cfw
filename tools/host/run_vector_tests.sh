#!/bin/bash
# Host check of vector.c (paths, rotation) against the g2flash vector-cases
# pixel probes, on the RayNeo canvas. Needs clang and bun.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/rayneo-vector-tests}
mkdir -p "$OUT"
clang -std=c11 -O1 -fsanitize=address,undefined -Wall -Wno-unused-function -Wno-unused-variable \
  -Ipatches -Ipatches/generated -o "$OUT/vector_test" tools/host/vector_test.c
hex() { (cd tools/g2suite && bun -e "import {svgPath} from './vector-protocol'; console.log(Buffer.from(svgPath(process.argv[1])).toString('hex'))" "$1"); }
RING="M80 80H240V240H80Z M120 120H200V200H120Z"
OPP="M80 80H240V240H80Z M120 120V200H200V120Z"
CURVE="M100 230C100 30 300 30 300 230Q200 350 100 230Z"
fail=0
run() { local name=$1; shift; echo "== $name"; VECTOR_PGM="$OUT/$name.pgm" "$OUT/vector_test" "$@" || fail=1; }
run fill-evenodd-hole           path 1 256 0 0 0 0 0 "$(hex "$RING")"  90,90,15 160,160,0
run fill-nonzero-same-winding   path 0 256 0 0 0 0 0 "$(hex "$RING")"  160,160,15
run fill-nonzero-opposite       path 0 256 0 0 0 0 0 "$(hex "$OPP")"   160,160,0
run fill-quadratic-cubic        path 0 256 0 0 0 0 0 "$(hex "$CURVE")" 200,170,15 200,30,0
# path tween target: translated to (300,180) at 2x and rotated 90 deg around (300,180)
run path-move-scale-rotate      path 0 512 300 180 $((90*256)) 300 180 "$(hex "M0 0H60V30H0Z")" 270,200,15 330,190,0
run rotation-initial            shape 3 1 100 100 80 40 0 0 0 0 0 140 120      105,120,15 140,85,0
run rotation-quarter-turn       shape 3 1 100 100 80 40 0 0 0 0 $((90*256)) 140 120   105,120,0 140,85,15
run rotation-ring-outline       shape 2 3 100 100 80 40 0 0 0 0 $((45*256)) 140 120   140,120,0
run rotation-circle-outline     shape 4 2 140 120 30 0 0 0 0 0 $((30*256)) 140 120    140,120,0 140,91,15
run rotation-line               shape 1 2 100 120 180 120 0 0 0 0 $((90*256)) 140 120 139,100,15 137,100,0 100,120,0
exit $fail
