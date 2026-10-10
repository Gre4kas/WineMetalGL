#!/bin/sh
# Installs the core next to a Wine build's winemac.so, which loads it from
# its own directory.
set -eu

if test "$#" -ne 1; then
    printf '%s\n' "usage: $0 /absolute/path/to/wine-build-or-install-unix-dir" >&2
    exit 2
fi

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=$1
source_library="$project_root/build/release/libwinemetalgl.dylib"
if test -d "$target/dlls/winemac.drv"; then
    destination_dir="$target/dlls/winemac.drv"
else
    destination_dir="$target"
fi

test -f "$destination_dir/winemac.so"
test -f "$source_library"
test "$(lipo -archs "$source_library")" = "x86_64"
test "$(otool -l "$source_library" | awk '/LC_BUILD_VERSION/{found=1} found && /minos/{print $2; exit}')" = "15.0"

install -m 755 "$source_library" "$destination_dir/libwinemetalgl.dylib"
codesign --force --sign - "$destination_dir/libwinemetalgl.dylib"

printf '%s\n' "Staged: $destination_dir/libwinemetalgl.dylib"
