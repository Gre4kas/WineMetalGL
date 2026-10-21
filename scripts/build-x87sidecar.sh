#!/bin/sh
# Builds x87sidecar (github.com/athei/x87sidecar, MIT; LICENSES/x87sidecar-MIT.txt),
# the native helper that makes Rosetta translate x87 code into fast code, at
# third_party/x87sidecar/REVISION with WineMetalGL's serve-pid.patch, into
# OUTDIR/x86_64-unix/x87sidecar, next to winemetalgl.so (src/wine/unix/unix_x87.c).
#
#   scripts/build-x87sidecar.sh [OUTDIR]
#
# Needs git, CMake 3.27 or later, and the network the first time; the build
# is kept in build/x87sidecar/ for the revision and patch it was made from.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/wine-module}
REVISION=$(cat "$ROOT/third_party/x87sidecar/REVISION")
PATCH=$ROOT/third_party/x87sidecar/serve-pid.patch
KEY=$REVISION-$(shasum -a 256 "$PATCH" | cut -c1-12)
WORK=$ROOT/build/x87sidecar/$KEY
BIN=$WORK/build/bin/x87sidecar

if ! test -x "$BIN"; then
    rm -rf "$WORK"
    mkdir -p "$WORK"
    git -C "$WORK" init -q src
    git -C "$WORK/src" fetch -q --depth 1 https://github.com/athei/x87sidecar.git "$REVISION"
    git -C "$WORK/src" checkout -q FETCH_HEAD
    git -C "$WORK/src" apply "$PATCH"
    cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$WORK/build" --target x87sidecar >/dev/null
fi
mkdir -p "$OUT/x86_64-unix"
cp "$BIN" "$OUT/x86_64-unix/x87sidecar"
echo "Built: $OUT/x86_64-unix/x87sidecar (x87sidecar $REVISION with serve-pid.patch)"
