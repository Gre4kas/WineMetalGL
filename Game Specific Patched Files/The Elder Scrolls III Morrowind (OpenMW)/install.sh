#!/bin/sh
# Builds the glue and installs it, with the arm64 core, into an OpenMW.app
# (macOS arm64 release or nightly). Idempotent: run it again after
# rebuilding the core.
#
#   install.sh /path/to/OpenMW.app [/path/to/libwinemetalgl.dylib]
#
# libwmglhost.dylib replaces OpenGL.framework; libSDL2-2.0.0.dylib becomes
# the SDL_GL_* glue over the real SDL, renamed libSDL2-real.dylib.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
test "$#" -ge 1 || { printf '%s\n' "usage: $0 /path/to/OpenMW.app [core]" >&2; exit 2; }
app="$1/Contents"
fw="$app/Frameworks"
core=${2:-$root/build/arm64-release/libwinemetalgl.dylib}
generated="$root/build/arm64-release/generated/gl_entry.gen.cpp"
test -e "$core" -a -e "$generated" || {
    printf '%s\n' "build the arm64 core first: scripts/stage-arm64-core.sh (or build/arm64-release)" >&2
    exit 1
}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$here/gen_stubs.py" "$generated" "$work"
if test ! -e "$fw/libSDL2-real.dylib"; then
    cp "$fw/libSDL2-2.0.0.dylib" "$fw/libSDL2-real.dylib"
    install_name_tool -id @executable_path/../Frameworks/libSDL2-real.dylib "$fw/libSDL2-real.dylib" 2>/dev/null
fi
for f in "$app/MacOS/openmw" "$fw"/libosg*.dylib; do
    if otool -L "$f" | grep -q OpenGL.framework; then
        install_name_tool -change /System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL \
            @executable_path/../Frameworks/libwmglhost.dylib "$f" 2>/dev/null
    fi
done
clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -o "$fw/libwmglhost.dylib" \
    "$work/procs.c" "$work/stubs.S" \
    -install_name @executable_path/../Frameworks/libwmglhost.dylib \
    -compatibility_version 1.0.0 -current_version 1.0.0
clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -fobjc-arc -Wall -o "$fw/libSDL2-2.0.0.dylib" \
    -I"$root/include" -I"$work" "$here/glue.m" "$fw/libwmglhost.dylib" \
    -framework Cocoa -framework QuartzCore -framework Metal \
    -Wl,-reexport_library,"$fw/libSDL2-real.dylib" \
    -install_name @executable_path/../Frameworks/libSDL2-2.0.0.dylib \
    -compatibility_version 3201.0.0 -current_version 3201.10.0
cp "$core" "$fw/libwinemetalgl.dylib"
codesign --force --deep --sign - "$1" 2>&1 | grep -v "replacing existing signature" || true
printf 'installed into %s\n' "$1"
