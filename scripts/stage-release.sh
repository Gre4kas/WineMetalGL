#!/bin/sh
# Stages a release: the Wine module with the core, the generated documentation
# and the evidence behind it (docs/release.md).
#
#   scripts/stage-release.sh WINE_HEADERS OUTPUT
#
# WINE_HEADERS is a Wine 11.x installation or source tree, for the module's
# build (scripts/build-wine-module.sh).
#
# install-wine-module.sh
# module/
#   x86_64-unix/     winemetalgl.so, libwinemetalgl.dylib (lib/wine/x86_64-unix)
#   x86_64-windows/  winemetalgl.dll (lib/wine/x86_64-windows), opengl32.dll (prefix system32)
#   i386-windows/    winemetalgl.dll (lib/wine/i386-windows), opengl32.dll (prefix syswow64)
set -eu

if test "$#" -ne 2; then
    printf '%s\n' "usage: $0 /absolute/path/to/wine-headers /absolute/path/to/output" >&2
    exit 2
fi

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wine_headers=$1
output=$2
core="$project_root/build/release/libwinemetalgl.dylib"
module="$project_root/build/wine-module"

if test "$output" = /; then
    printf '%s\n' 'refusing to stage over /' >&2
    exit 1
fi
if test -e "$output"; then
    printf 'output already exists; refusing to replace it: %s\n' "$output" >&2
    exit 1
fi

# Build the core and the module from this checkout, so the staged libraries
# are the commit recorded below.
"$project_root/scripts/build.sh" >/dev/null
"$project_root/scripts/build-wine-module.sh" "$wine_headers" "$module" >/dev/null

mkdir -p "$output/module/x86_64-unix" "$output/module/x86_64-windows" "$output/module/i386-windows" "$output/docs" \
    "$output/evidence/cts" "$output/evidence/games"
cp "$core" "$output/module/x86_64-unix/libwinemetalgl.dylib"
cp "$module/x86_64-unix/winemetalgl.so" "$output/module/x86_64-unix/winemetalgl.so"
for arch in x86_64 i386; do
    cp "$module/$arch-windows/winemetalgl.dll" "$module/$arch-windows/opengl32.dll" "$output/module/$arch-windows/"
done
# Ad-hoc signatures: a runtime that requires signed code refuses unsigned
# libraries.
for library in "$output"/module/x86_64-unix/*; do
    codesign --force --sign - "$library"
done
cp "$project_root/scripts/install-wine-module.sh" "$output/install-wine-module.sh"
cp "$project_root/README.md" "$output/README.md"
# LGPL-3.0-or-later: LICENSE, the GPL-3.0 it builds on and the third-party licenses.
cp "$project_root/LICENSE" "$output/LICENSE"
cp -R "$project_root/LICENSES" "$output/LICENSES"
cp "$project_root/docs/conformance.md" "$output/docs/conformance.md"
cp "$project_root/docs/api-coverage.json" "$output/docs/api-coverage.json"
cp "$project_root/docs/games.md" "$output/docs/games.md"
cp "$project_root/docs/release.md" "$output/docs/release.md"
for doc in unsupported testing building performance; do
    cp "$project_root/docs/$doc.md" "$output/docs/$doc.md"
done
# The evidence the generated documents were built from.
cp "$project_root"/tests/cts/baseline/*.txt "$output/evidence/cts/"
# Later runs of the baselines' non-Pass cases, which the totals include.
if [ -d "$project_root/tests/cts/recheck" ]; then
    mkdir -p "$output/evidence/cts/recheck"
    cp "$project_root"/tests/cts/recheck/*.txt "$output/evidence/cts/recheck/"
fi
cp "$project_root/tests/cts/notsupported.json" "$output/evidence/cts/notsupported.json"
cp "$project_root/tests/cts/known-failures.json" "$output/evidence/cts/known-failures.json"
cp "$project_root/tests/games/corpus.json" "$project_root/tests/games/results.json" "$output/evidence/games/"
if test -f "$project_root/tests/games/perf.json"; then cp "$project_root/tests/games/perf.json" "$output/evidence/games/"; fi

commit=$(git -C "$project_root" rev-parse HEAD)
if ! git -C "$project_root" diff --quiet HEAD; then commit="$commit (uncommitted changes)"; fi
printf 'WineMetalGL staged from %s\nWine headers: %s\nCommit: %s\n' \
    "$project_root" "$wine_headers" "$commit" > "$output/STAGED-FROM.txt"

(
    cd "$output"
    find . -type f ! -name SHA256SUMS -print | sort | while IFS= read -r path; do
        shasum -a 256 "$path"
    done > SHA256SUMS
)

"$project_root/scripts/verify-release.sh" "$output"
