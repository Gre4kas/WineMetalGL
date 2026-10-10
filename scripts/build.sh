#!/bin/sh
# Builds the x86_64 core (libwinemetalgl.dylib) that Wine's winemac.drv loads.
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# The presets are read from the current directory.
cd "$project_root"

cmake --preset release
cmake --build --preset release

library="$project_root/build/release/libwinemetalgl.dylib"
test -f "$library"
test "$(lipo -archs "$library")" = "x86_64"
if otool -L "$library" | grep -q OpenGL.framework; then
    printf '%s\n' "$library links OpenGL.framework" >&2
    exit 1
fi

printf '%s\n' "Built x86_64 core: $library"
