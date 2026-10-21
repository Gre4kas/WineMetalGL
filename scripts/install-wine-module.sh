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
# package (this script at its top), its module directory; and otherwise (the
# script on its own) the release downloaded from GitHub (fetch_release).
set -eu

# The release to install when neither a release package (module/ next to this
# script) nor a checkout's build is at hand (a script downloaded on its own,
# or WineMetalGL Settings without the release inside it): downloaded from
# GitHub, checked against the release's SHA256SUMS.txt, and kept in
# WINEMETALGL_CACHE (default ~/Library/Caches/WineMetalGL/releases).
# WINEMETALGL_VERSION picks a version (default: the latest release).
fetch_release() {
    repo=metalsharp/WineMetalGL
    version=${WINEMETALGL_VERSION:-}
    if test -z "$version"; then
        latest=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest") ||
            { echo "cannot reach GitHub to find the latest WineMetalGL release" >&2; exit 1; }
        version=${latest##*/winemetalgl-}
        case "$version" in [0-9]*.[0-9]*) ;; *) echo "no WineMetalGL release found ($latest)" >&2; exit 1 ;; esac
    fi
    name=winemetalgl-$version-x86_64
    cache=${WINEMETALGL_CACHE:-$HOME/Library/Caches/WineMetalGL/releases}/$version
    if ! test -f "$cache/$name/module/x86_64-unix/libwinemetalgl.dylib"; then
        echo "Downloading WineMetalGL $version"
        tmp=$(mktemp -d "${TMPDIR:-/tmp}/winemetalgl.XXXXXX")
        url=https://github.com/$repo/releases/download/winemetalgl-$version
        curl -fsSL -o "$tmp/$name.tar.gz" "$url/$name.tar.gz" &&
            curl -fsSL -o "$tmp/SHA256SUMS.txt" "$url/SHA256SUMS.txt" ||
            { rm -rf "$tmp"; echo "cannot download WineMetalGL $version from $url" >&2; exit 1; }
        expected=$(awk -v f="$name.tar.gz" '$2 == f { print $1 }' "$tmp/SHA256SUMS.txt")
        actual=$(shasum -a 256 "$tmp/$name.tar.gz" | awk '{ print $1 }')
        if test -z "$expected" || test "$expected" != "$actual"; then
            rm -rf "$tmp"
            echo "$name.tar.gz does not match the release's SHA256SUMS.txt; not installed" >&2
            exit 1
        fi
        mkdir -p "$cache"
        tar -xzf "$tmp/$name.tar.gz" -C "$cache" || { rm -rf "$tmp"; echo "cannot extract $name.tar.gz" >&2; exit 1; }
        rm -rf "$tmp"
        test -f "$cache/$name/module/x86_64-unix/libwinemetalgl.dylib" ||
            { echo "$name.tar.gz has no module/ directory" >&2; exit 1; }
    fi
    echo "Using WineMetalGL $version ($cache/$name)"
    MODULE=${MODULE:-$cache/$name/module}
    CORE=${CORE:-$MODULE/x86_64-unix/libwinemetalgl.dylib}
}

HERE=$(cd "$(dirname "$0")" && pwd)
WINE=${1:?usage: install-wine-module.sh WINE_INSTALL [WINEPREFIX]}
WINE=${WINE%/}
PREFIX=${2:-}
ROOT=$(cd "$HERE/.." && pwd)
if test -d "$HERE/module"; then
    MODULE=${MODULE:-$HERE/module}
    CORE=${CORE:-$HERE/module/x86_64-unix/libwinemetalgl.dylib}
elif test -n "${MODULE:-}" || { test -d "$ROOT/build/wine-module" && test -f "$ROOT/CMakeLists.txt"; }; then
    MODULE=${MODULE:-$ROOT/build/wine-module}
    CORE=${CORE:-$ROOT/build/release/libwinemetalgl.dylib}
else
    fetch_release
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
# The x87 acceleration helper (WINEMETALGL_X87), when the module has one.
if test -e "$MODULE/x86_64-unix/x87sidecar"; then
    install_file "$MODULE/x86_64-unix/x87sidecar" "$LIB/x86_64-unix/x87sidecar"
else
    echo "This module has no x87sidecar: x87 acceleration (WINEMETALGL_X87) stays off."
fi
# Copies keep a valid signature (a release's Developer ID one); others are
# signed ad hoc.
for f in "$LIB/x86_64-unix/winemetalgl.so" "$LIB/x86_64-unix/libwinemetalgl.dylib" "$LIB/x86_64-unix/x87sidecar"; do
    test -e "$f" || continue
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
