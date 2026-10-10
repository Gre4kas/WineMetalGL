#!/bin/sh
# Builds glcts natively for this Mac against the WineMetalGL core, so the
# KHR-GL33 suites can run without Wine (see README.md).
#
#   tests/cts/native/build.sh <VK-GL-CTS checkout> [build dir]
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cts=${1:?usage: build.sh <VK-GL-CTS checkout> [build dir]}
build=${2:-$cts/build-wmgl-native}

# The core for the build machine's architecture.
cmake -S "$root" -B "$root/build/host" -G Ninja -DWMGL_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$root/build/host" --target winemetalgl_core

mkdir -p "$cts/targets/wmgl"
cp "$root/tests/cts/native/wmgl.cmake" "$cts/targets/wmgl/wmgl.cmake"
cmake -S "$cts" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=wmgl \
    -DWMGL_SOURCE_DIR="$root" -DWMGL_LIBRARY="$root/build/host/libwinemetalgl.dylib" \
    -DCMAKE_BUILD_RPATH="$root/build/host"
cmake --build "$build" --target glcts
printf '%s\n' "glcts: $build/external/openglcts/modules/glcts"
