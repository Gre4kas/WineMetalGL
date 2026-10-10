#!/bin/sh
# Stages a release: the core next to the patched Wine modules that load it,
# the generated documentation and the evidence behind it (docs/release.md).
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

# Build the core from this checkout, so the staged library is the commit
# recorded below.
"$project_root/scripts/build.sh" >/dev/null

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
mkdir -p "$output/artifacts/guest/i386" "$output/artifacts/guest/x86_64" "$output/artifacts/wine-driver" "$output/docs" \
    "$output/evidence/cts" "$output/evidence/games"

cp "$core" "$output/artifacts/wine-driver/libwinemetalgl.dylib"
cp "$wine_build/dlls/winemac.drv/winemac.so" "$output/artifacts/wine-driver/winemac.so"
cp "$wine_build/dlls/win32u/win32u.so" "$output/artifacts/wine-driver/win32u.so"
cp "$wine_build/dlls/opengl32/opengl32.so" "$output/artifacts/wine-driver/opengl32.so"
cp "$wine_build/dlls/opengl32/i386-windows/opengl32.dll" "$output/artifacts/guest/i386/opengl32.dll"
cp "$wine_build/dlls/opengl32/x86_64-windows/opengl32.dll" "$output/artifacts/guest/x86_64/opengl32.dll"
# Ad-hoc signatures: a runtime that requires signed code refuses unsigned
# libraries, and winemac.so would quietly fall back to Apple's OpenGL.
for library in "$output"/artifacts/wine-driver/*; do
    codesign --force --sign - "$library"
done
cp "$project_root/README.md" "$output/README.md"
cp "$project_root/docs/conformance.md" "$output/docs/conformance.md"
cp "$project_root/docs/api-coverage.json" "$output/docs/api-coverage.json"
cp "$project_root/docs/games.md" "$output/docs/games.md"
cp "$project_root/docs/release.md" "$output/docs/release.md"
# The evidence the generated documents were built from.
cp "$project_root"/tests/cts/baseline/*.txt "$output/evidence/cts/"
cp "$project_root/tests/cts/notsupported.json" "$output/evidence/cts/notsupported.json"
cp "$project_root/tests/games/corpus.json" "$project_root/tests/games/results.json" "$output/evidence/games/"
if test -f "$project_root/tests/games/perf.json"; then cp "$project_root/tests/games/perf.json" "$output/evidence/games/"; fi

commit=$(git -C "$project_root" rev-parse HEAD)
if ! git -C "$project_root" diff --quiet HEAD; then commit="$commit (uncommitted changes)"; fi
printf 'WineMetalGL staged from %s\nWine build: %s\nCommit: %s\n' \
    "$project_root" "$wine_build" "$commit" > "$output/STAGED-FROM.txt"

(
    cd "$output"
    find . -type f ! -name SHA256SUMS -print | sort | while IFS= read -r path; do
        shasum -a 256 "$path"
    done > SHA256SUMS
)

"$project_root/scripts/verify-release.sh" "$output"
