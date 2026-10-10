#!/bin/sh
# Stages a release: the core next to the patched Wine modules that load it,
# plus the generated documentation.
#
# artifacts/
#   wine-driver/   libwinemetalgl.dylib, winemac.so, win32u.so, opengl32.so
#                  (install into lib/wine/x86_64-unix; winemac.so loads the
#                  core from its own directory)
#   guest/i386/    opengl32.dll   (lib/wine/i386-windows)
#   guest/x86_64/  opengl32.dll   (lib/wine/x86_64-windows)
set -eu

if test "$#" -ne 2; then
    printf '%s\n' "usage: $0 /absolute/path/to/wine-build /absolute/path/to/output" >&2
    exit 2
fi

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wine_build=$1
output=$2
core="$project_root/build/release/libwinemetalgl.dylib"

for path in \
    "$core" \
    "$wine_build/dlls/opengl32/i386-windows/opengl32.dll" \
    "$wine_build/dlls/opengl32/x86_64-windows/opengl32.dll" \
    "$wine_build/dlls/opengl32/opengl32.so" \
    "$wine_build/dlls/winemac.drv/winemac.so" \
    "$wine_build/dlls/win32u/win32u.so"; do
    test -f "$path" || { printf 'missing build artifact: %s\n' "$path" >&2; exit 1; }
done

if test "$output" = /; then
    printf '%s\n' 'refusing to stage over /' >&2
    exit 1
fi
if test -e "$output"; then
    printf 'output already exists; refusing to replace it: %s\n' "$output" >&2
    exit 1
fi
mkdir -p "$output/artifacts/guest/i386" "$output/artifacts/guest/x86_64" "$output/artifacts/wine-driver" "$output/docs"

cp "$core" "$output/artifacts/wine-driver/libwinemetalgl.dylib"
cp "$wine_build/dlls/winemac.drv/winemac.so" "$output/artifacts/wine-driver/winemac.so"
cp "$wine_build/dlls/win32u/win32u.so" "$output/artifacts/wine-driver/win32u.so"
cp "$wine_build/dlls/opengl32/opengl32.so" "$output/artifacts/wine-driver/opengl32.so"
cp "$wine_build/dlls/opengl32/i386-windows/opengl32.dll" "$output/artifacts/guest/i386/opengl32.dll"
cp "$wine_build/dlls/opengl32/x86_64-windows/opengl32.dll" "$output/artifacts/guest/x86_64/opengl32.dll"
cp "$project_root/README.md" "$output/README.md"
cp "$project_root/docs/conformance.md" "$output/docs/conformance.md"
cp "$project_root/docs/api-coverage.json" "$output/docs/api-coverage.json"

printf 'WineMetalGL staged from %s\nWine build: %s\nCommit: %s\n' \
    "$project_root" "$wine_build" "$(git -C "$project_root" rev-parse HEAD)" > "$output/STAGED-FROM.txt"

(
    cd "$output"
    find . -type f ! -name SHA256SUMS -print | sort | while IFS= read -r path; do
        shasum -a 256 "$path"
    done > SHA256SUMS
)

"$project_root/scripts/verify-release.sh" "$output"
