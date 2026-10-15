#!/bin/sh
# Installs the WineMetalGL Wine module (scripts/build-wine-module.sh) into a
# Wine 11.x installation, and optionally into a Wine prefix.
#
#   scripts/install-wine-module.sh WINE_INSTALL [WINEPREFIX]
#
# WINE_INSTALL is the directory with bin/wine and lib/wine, or a Wine app
# (WineHQ's "Wine Stable.app", whose Wine is in Contents/Resources/wine). For
# CrossOver, use install-crossover.sh.
#
# Into WINE_INSTALL, without replacing any of Wine's files:
#   lib/wine/x86_64-windows/winemetalgl.dll, lib/wine/i386-windows/winemetalgl.dll
#   lib/wine/x86_64-unix/winemetalgl.so, lib/wine/x86_64-unix/libwinemetalgl.dylib (the core)
# Into WINEPREFIX (each prefix that should use WineMetalGL): opengl32.dll,
# which forwards to winemetalgl.dll, and winemetalgl.dll itself, in system32
# and syswow64, and the DLL override opengl32=native. Removing the override (or the file) gives the
# prefix Wine's own OpenGL back. A WINEPREFIX that does not exist yet is
# created (wineboot).
#
# From a checkout, MODULE (default build/wine-module) and CORE (default
# build/release/libwinemetalgl.dylib) choose what is installed; from a release
# package (this script at its top), its module directory.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
WINE=${1:?usage: install-wine-module.sh WINE_INSTALL [WINEPREFIX]}
WINE=${WINE%/}
PREFIX=${2:-}
if test -d "$HERE/module"; then
    MODULE=${MODULE:-$HERE/module}
    CORE=${CORE:-$HERE/module/x86_64-unix/libwinemetalgl.dylib}
else
    ROOT=$(cd "$HERE/.." && pwd)
    MODULE=${MODULE:-$ROOT/build/wine-module}
    CORE=${CORE:-$ROOT/build/release/libwinemetalgl.dylib}
fi
case "$WINE" in
*CrossOver.app*) echo "This is CrossOver; use install-crossover.sh BOTTLE instead." >&2; exit 1 ;;
esac
test -d "$WINE/Contents/Resources/wine/lib/wine" && WINE="$WINE/Contents/Resources/wine"
LIB="$WINE/lib/wine"
test -x "$WINE/bin/wine" || { echo "not a Wine installation (no bin/wine): $WINE" >&2; exit 1; }

for f in "$MODULE/x86_64-windows/winemetalgl.dll" "$MODULE/i386-windows/winemetalgl.dll" \
         "$MODULE/x86_64-unix/winemetalgl.so" "$CORE" "$LIB/x86_64-unix/ntdll.so"; do
    test -e "$f" || { echo "missing: $f" >&2; exit 1; }
done

# macOS keeps a signed app from being changed unless the program changing it
# (the terminal) has App Management permission.
install_file() {
    if ! out=$(cp "$1" "$2" 2>&1); then
        echo "$out" >&2
        case "$out" in
        *"Operation not permitted"*)
            echo "macOS did not let this terminal change $WINE. Allow it in System Settings >" >&2
            echo "Privacy & Security > App Management (then reopen it), or install into a copy of Wine outside the app." >&2 ;;
        esac
        exit 1
    fi
}
install_file "$MODULE/x86_64-windows/winemetalgl.dll" "$LIB/x86_64-windows/winemetalgl.dll"
install_file "$MODULE/i386-windows/winemetalgl.dll" "$LIB/i386-windows/winemetalgl.dll"
install_file "$MODULE/x86_64-unix/winemetalgl.so" "$LIB/x86_64-unix/winemetalgl.so"
if test "$CORE" != "$LIB/x86_64-unix/libwinemetalgl.dylib"; then install_file "$CORE" "$LIB/x86_64-unix/libwinemetalgl.dylib"; fi
# Copies keep a valid signature (a release's Developer ID one); others are
# signed ad hoc.
for f in "$LIB/x86_64-unix/winemetalgl.so" "$LIB/x86_64-unix/libwinemetalgl.dylib"; do
    codesign --verify "$f" 2>/dev/null && continue
    if ! out=$(codesign -f -s - "$f" 2>&1); then
        echo "$out" >&2
        echo "codesign failed: $f" >&2
        exit 1
    fi
done
echo "Installed winemetalgl into $LIB"

if test -z "$PREFIX"; then
    echo "No prefix given: run again with one (install-wine-module.sh WINE_INSTALL WINEPREFIX) for its programs to use WineMetalGL."
    exit 0
fi
if ! test -e "$PREFIX"; then
    echo "Creating the Wine prefix $PREFIX"
    if ! out=$(WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE/bin/wine" wineboot -i 2>&1); then
        echo "$out" >&2
        echo "wineboot failed: $PREFIX" >&2
        exit 1
    fi
fi
test -d "$PREFIX/drive_c/windows/system32" || { echo "not a Wine prefix: $PREFIX" >&2; exit 1; }
# winemetalgl.dll goes next to the forwarder too: a Wine whose loader
# does not search lib/wine/<arch>-windows for it still finds it in the prefix.
cp "$MODULE/x86_64-windows/opengl32.dll" "$PREFIX/drive_c/windows/system32/opengl32.dll"
cp "$MODULE/x86_64-windows/winemetalgl.dll" "$PREFIX/drive_c/windows/system32/winemetalgl.dll"
if test -d "$PREFIX/drive_c/windows/syswow64"; then
    cp "$MODULE/i386-windows/opengl32.dll" "$PREFIX/drive_c/windows/syswow64/opengl32.dll"
    cp "$MODULE/i386-windows/winemetalgl.dll" "$PREFIX/drive_c/windows/syswow64/winemetalgl.dll"
fi
if ! out=$(WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE/bin/wine" reg add 'HKCU\Software\Wine\DllOverrides' \
    /v opengl32 /d native /f 2>&1); then
    echo "$out" >&2
    echo "could not set opengl32=native in $PREFIX" >&2
    exit 1
fi
echo "Installed opengl32.dll (opengl32=native) into $PREFIX"
