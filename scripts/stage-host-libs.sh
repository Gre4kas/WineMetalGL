#!/bin/sh
set -eu

# Stage the x86_64 host-library closure Wine's native font path dlopens.
# No ARM64/universal artifacts are accepted.
if test "$#" -ne 1; then
    printf '%s\n' "usage: $0 /absolute/path/to/runtime" >&2
    exit 2
fi
# WINEMETALGL_HOST_LIB_DIR names the directory holding the x86_64 libraries;
# without it, the runtime's own lib directory is only checked.
runtime=$1
source_dir=${WINEMETALGL_HOST_LIB_DIR:-$runtime/lib}
destination="$runtime/lib"
mkdir -p "$destination"
same=0
if test "$(CDPATH= cd -- "$source_dir" && pwd -P)" = "$(CDPATH= cd -- "$destination" && pwd -P)"; then same=1; fi
for name in libfreetype.6.dylib libpng16.16.dylib libz.dylib libbz2.1.0.dylib libbrotlidec.1.dylib libbrotlicommon.1.dylib; do
    test -f "$source_dir/$name" || { echo "Missing host dependency: $source_dir/$name" >&2; exit 1; }
    test "$(lipo -archs "$source_dir/$name")" = x86_64 || { echo "Non-x86_64 host dependency: $name" >&2; exit 1; }
    test "$same" = 1 || install -m 755 "$source_dir/$name" "$destination/$name"
done
ln -sfn libfreetype.6.dylib "$destination/libfreetype.dylib"
ln -sfn libpng16.16.dylib "$destination/libpng16.dylib"
printf '%s\n' "Staged x86_64 host font-library closure in $destination"
