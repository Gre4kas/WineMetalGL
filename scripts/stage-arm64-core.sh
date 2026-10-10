#!/bin/sh
# Stages the arm64 core for a native arm64 Wine: the core, the driver ABI
# header and the Wine patches (docs/release.md). The x86_64 release
# (scripts/stage-release.sh) is the drop-in package for x86_64 Wine under
# Rosetta; the two cores are not interchangeable (winemac.so loads the core of
# its own architecture and falls back to Apple's OpenGL otherwise).
#
#   scripts/stage-arm64-core.sh /absolute/path/to/output
set -eu

if test "$#" -ne 1; then
    printf '%s\n' "usage: $0 /absolute/path/to/output" >&2
    exit 2
fi
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$1
build="$project_root/build/arm64-release"
core="$build/libwinemetalgl.dylib"

if test "$output" = /; then
    printf '%s\n' 'refusing to stage over /' >&2
    exit 1
fi
if test -e "$output"; then
    printf 'output already exists; refusing to replace it: %s\n' "$output" >&2
    exit 1
fi

cmake -S "$project_root" -B "$build" -G Ninja -DWMGL_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$build" --target winemetalgl_core >/dev/null

test "$(lipo -archs "$core")" = arm64 || { printf 'not an arm64 core: %s\n' "$core" >&2; exit 1; }
minos=$(otool -l "$core" | awk '/LC_BUILD_VERSION/{found=1} found && /minos/{print $2; exit}')
test "$minos" = 15.0 || { printf 'unexpected deployment target: %s\n' "$minos" >&2; exit 1; }
if otool -L "$core" | grep -q OpenGL.framework; then
    printf '%s\n' 'the core links OpenGL.framework' >&2
    exit 1
fi
if nm -gU "$core" | awk '{print $3}' | grep -v '^_wmgl_' | grep -q .; then
    printf '%s\n' 'the core exports symbols outside the driver ABI' >&2
    exit 1
fi

mkdir -p "$output/lib" "$output/include/winemetalgl" "$output/patches" "$output/docs" "$output/evidence/cts"
cp "$core" "$output/lib/libwinemetalgl.dylib"
codesign --force --sign - "$output/lib/libwinemetalgl.dylib"
cp "$project_root/include/winemetalgl/driver.h" "$output/include/winemetalgl/driver.h"
cp -R "$project_root/patches/wine" "$output/patches/wine"
cp "$project_root/README.md" "$output/README.md"
# LGPL-3.0-or-later: LICENSE, the GPL-3.0 it builds on and the third-party licenses.
cp "$project_root/LICENSE" "$output/LICENSE"
cp -R "$project_root/LICENSES" "$output/LICENSES"
cp "$project_root/docs/conformance.md" "$project_root/docs/unsupported.md" "$project_root/docs/building.md" \
    "$project_root/docs/release.md" "$output/docs/"
# The native KHR-GL30 to KHR-GL33 runs exercise this build of the core (tests/cts/native).
cp "$project_root"/tests/cts/baseline/gl3[0-3]-cfg*-native.txt "$output/evidence/cts/"
cat > "$output/ARM64.txt" <<'EOF'
WineMetalGL core for arm64 (native) Wine on macOS 15 or later.

lib/libwinemetalgl.dylib goes next to an arm64 winemac.so built from a Wine
11.17 tree with patches/wine applied (patches/wine/README.md); winemac.so
loads it from its own directory. Do not install it into an x86_64 Wine (use
the x86_64 package there): a core of the wrong architecture does not load, and
Wine silently falls back to Apple's OpenGL.

This core is the build the native KHR-GL30 to KHR-GL33 runs in evidence/cts
exercised (mustpass configurations 1 to 4). It has not yet run inside an arm64
Wine.
EOF

commit=$(git -C "$project_root" rev-parse HEAD)
if ! git -C "$project_root" diff --quiet HEAD; then commit="$commit (uncommitted changes)"; fi
printf 'WineMetalGL arm64 core staged from %s\nCommit: %s\n' "$project_root" "$commit" > "$output/STAGED-FROM.txt"
(
    cd "$output"
    find . -type f ! -name SHA256SUMS -print | sort | while IFS= read -r file; do
        shasum -a 256 "$file"
    done > SHA256SUMS
    shasum -a 256 -c SHA256SUMS >/dev/null
)
codesign --verify "$output/lib/libwinemetalgl.dylib"
printf '%s\n' 'WINEMETALGL_ARM64_CORE_STAGED' "stage=$output"
