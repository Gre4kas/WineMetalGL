#!/bin/sh
# Installs the WineMetalGL Wine module (scripts/build-wine-module.sh) into a
# Wine 11.x installation, and optionally into a Wine prefix.
#
#   scripts/install-wine-module.sh WINE_INSTALL [WINEPREFIX]
#
# Into WINE_INSTALL, without replacing any of Wine's files:
#   lib/wine/x86_64-windows/winemetalgl.dll, lib/wine/i386-windows/winemetalgl.dll
#   lib/wine/x86_64-unix/winemetalgl.so, lib/wine/x86_64-unix/libwinemetalgl.dylib (the core)
# Into WINEPREFIX (each prefix that should use WineMetalGL): opengl32.dll,
# which forwards to winemetalgl.dll, in system32 and syswow64, and the DLL
# override opengl32=native. Removing the override (or the file) gives the
# prefix Wine's own OpenGL back.
#
# From a checkout, MODULE (default build/wine-module) and CORE (default
# build/release/libwinemetalgl.dylib) choose what is installed; from a release
# package (this script at its top), its module directory.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
WINE=${1:?usage: install-wine-module.sh WINE_INSTALL [WINEPREFIX]}
PREFIX=${2:-}
if test -d "$HERE/module"; then
    MODULE=${MODULE:-$HERE/module}
    CORE=${CORE:-$HERE/module/x86_64-unix/libwinemetalgl.dylib}
else
    ROOT=$(cd "$HERE/.." && pwd)
    MODULE=${MODULE:-$ROOT/build/wine-module}
    CORE=${CORE:-$ROOT/build/release/libwinemetalgl.dylib}
fi
LIB="$WINE/lib/wine"

for f in "$MODULE/x86_64-windows/winemetalgl.dll" "$MODULE/i386-windows/winemetalgl.dll" \
         "$MODULE/x86_64-unix/winemetalgl.so" "$CORE" "$LIB/x86_64-unix/ntdll.so"; do
    test -e "$f" || { echo "missing: $f" >&2; exit 1; }
done

cp "$MODULE/x86_64-windows/winemetalgl.dll" "$LIB/x86_64-windows/winemetalgl.dll"
cp "$MODULE/i386-windows/winemetalgl.dll" "$LIB/i386-windows/winemetalgl.dll"
cp "$MODULE/x86_64-unix/winemetalgl.so" "$LIB/x86_64-unix/winemetalgl.so"
if test "$CORE" != "$LIB/x86_64-unix/libwinemetalgl.dylib"; then cp "$CORE" "$LIB/x86_64-unix/libwinemetalgl.dylib"; fi
codesign -f -s - "$LIB/x86_64-unix/winemetalgl.so" "$LIB/x86_64-unix/libwinemetalgl.dylib" 2>/dev/null
echo "Installed winemetalgl into $LIB"

if test -n "$PREFIX"; then
    test -d "$PREFIX/drive_c/windows/system32" || { echo "not a Wine prefix: $PREFIX" >&2; exit 1; }
    cp "$MODULE/x86_64-windows/opengl32.dll" "$PREFIX/drive_c/windows/system32/opengl32.dll"
    if test -d "$PREFIX/drive_c/windows/syswow64"; then
        cp "$MODULE/i386-windows/opengl32.dll" "$PREFIX/drive_c/windows/syswow64/opengl32.dll"
    fi
    WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE/bin/wine" reg add 'HKCU\Software\Wine\DllOverrides' \
        /v opengl32 /d native /f >/dev/null
    echo "Installed opengl32.dll (opengl32=native) into $PREFIX"
fi
