#!/bin/sh
# Checks a staged release (scripts/stage-release.sh).
set -eu

if test "$#" -ne 1; then
    printf '%s\n' "usage: $0 /absolute/path/to/staged-release" >&2
    exit 2
fi

stage=$1
core="$stage/artifacts/wine-driver/libwinemetalgl.dylib"

for path in \
    "$core" \
    "$stage/artifacts/guest/i386/opengl32.dll" \
    "$stage/artifacts/guest/x86_64/opengl32.dll" \
    "$stage/artifacts/wine-driver/winemac.so" \
    "$stage/artifacts/wine-driver/win32u.so" \
    "$stage/artifacts/wine-driver/opengl32.so" \
    "$stage/docs/api-coverage.json" \
    "$stage/docs/conformance.md" \
    "$stage/docs/games.md" \
    "$stage/docs/release.md" \
    "$stage/docs/unsupported.md" \
    "$stage/docs/testing.md" \
    "$stage/docs/building.md" \
    "$stage/docs/performance.md" \
    "$stage/evidence/cts/gl33-cfg1-native.txt" \
    "$stage/evidence/cts/gl33-cfg1-wine.txt" \
    "$stage/evidence/games/results.json" \
    "$stage/SHA256SUMS"; do
    test -f "$path" || { printf 'missing staged artifact: %s\n' "$path" >&2; exit 1; }
done

for path in \
    "$core" \
    "$stage/artifacts/wine-driver/winemac.so" \
    "$stage/artifacts/wine-driver/win32u.so" \
    "$stage/artifacts/wine-driver/opengl32.so"; do
    arch=$(lipo -archs "$path")
    test "$arch" = x86_64 || { printf 'unexpected Mach-O architecture %s: %s\n' "$arch" "$path" >&2; exit 1; }
    codesign --verify "$path" 2>/dev/null || { printf 'not validly signed: %s\n' "$path" >&2; exit 1; }
done

minos=$(otool -l "$core" | awk '/LC_BUILD_VERSION/{found=1} found && /minos/{print $2; exit}')
test "$minos" = 15.0 || { printf 'unexpected core deployment target: %s\n' "$minos" >&2; exit 1; }
if otool -L "$core" | grep -q OpenGL.framework; then
    printf '%s\n' 'the core links OpenGL.framework' >&2
    exit 1
fi
nm -gU "$core" | awk '{print $3}' | grep -v '^_wmgl_' | grep -q . && {
    printf '%s\n' 'the core exports symbols outside the driver ABI' >&2
    exit 1
}

file "$stage/artifacts/guest/i386/opengl32.dll" | grep -q 'Intel 80386'
file "$stage/artifacts/guest/x86_64/opengl32.dll" | grep -q 'x86-64'

python3 -m json.tool "$stage/docs/api-coverage.json" >/dev/null
python3 -m json.tool "$stage/evidence/games/results.json" >/dev/null
(
    cd "$stage"
    shasum -a 256 -c SHA256SUMS >/dev/null
)

printf '%s\n' 'WINEMETALGL_RELEASE_AUDIT_OK'
printf 'stage=%s\n' "$stage"
printf 'core_arch=%s\n' "$(lipo -archs "$core")"
printf 'core_minos=%s\n' "$minos"
