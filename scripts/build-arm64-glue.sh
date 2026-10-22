#!/bin/sh
# Builds the native glue that runs games on the arm64 core outside Wine
# (Game Specific Patched Files: Minecraft (Java Edition), The Elder Scrolls
# III Morrowind (OpenMW)), prebuilt for WineMetalGL Settings to install:
#
#   scripts/build-arm64-glue.sh /absolute/path/to/output
#
#   minecraft/libwmglhost.dylib   every gl* name, for -Dorg.lwjgl.opengl.libname
#   minecraft/libSDL3.dylib       the SDL3 glue (Minecraft 26.x), re-exporting
#                                 libSDL3-real.dylib beside it
#   minecraft/libglfw.dylib       the GLFW glue (Minecraft 1.21.x), re-exporting
#                                 libglfw-real.dylib beside it
#   openmw/libwmglhost.dylib      every gl* name, for OpenMW.app's Frameworks
#   openmw/libSDL2-2.0.0.dylib    the SDL2 glue, re-exporting libSDL2-real.dylib
#
# The real libraries are the game's own, put beside the glue when it is
# installed (LWJGL's natives; OpenMW.app's SDL2): the glue links against
# stand-ins, Homebrew's SDL2, SDL3 and GLFW (brew install sdl2 sdl3 glfw)
# under those names. Needs the arm64 build (build/arm64-release, from
# scripts/stage-arm64-core.sh) for the names of the core's GL functions.
set -eu

test "$#" -eq 1 || { printf '%s\n' "usage: $0 /absolute/path/to/output" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$1
generated="$root/build/arm64-release/generated/gl_entry.gen.cpp"
fixes="$root/Game Specific Patched Files"
test -e "$generated" || { printf '%s\n' "build the arm64 core first (scripts/stage-arm64-core.sh)" >&2; exit 1; }
for formula in sdl2 sdl3 glfw; do
    prefix=$(brew --prefix "$formula" 2>/dev/null) && test -d "$prefix/include" ||
        { printf '%s\n' "no $formula: brew install sdl2 sdl3 glfw" >&2; exit 1; }
done
sdl2=$(brew --prefix sdl2) sdl3=$(brew --prefix sdl3) glfw=$(brew --prefix glfw)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$fixes/The Elder Scrolls III Morrowind (OpenMW)/gen_stubs.py" "$generated" "$work"
# A stand-in for a game's library: Homebrew's, renamed to where the game's goes.
standin() {
    cp "$1" "$work/$2"
    chmod u+w "$work/$2"
    xcrun install_name_tool -id "$3" "$work/$2" 2>/dev/null
}
standin "$sdl3/lib/libSDL3.dylib" libSDL3-real.dylib @loader_path/libSDL3-real.dylib
standin "$glfw/lib/libglfw.dylib" libglfw-real.dylib @loader_path/libglfw-real.dylib
standin "$sdl2/lib/libSDL2-2.0.0.dylib" libSDL2-real.dylib @executable_path/../Frameworks/libSDL2-real.dylib

# Apple's clang (xcrun): another first in PATH (the release's llvm-mingw, for
# the Wine module) has no macOS SDK ("ld: library 'System' not found").
cc() { xcrun clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 "$@"; }
glue() { cc -fobjc-arc -Wall -I"$root/include" -I"$work" -framework Cocoa -framework QuartzCore -framework Metal "$@"; }

mc="$out/minecraft"
mkdir -p "$mc"
cc -o "$mc/libwmglhost.dylib" "$work/procs.c" "$work/stubs.S" -install_name @loader_path/libwmglhost.dylib \
    -compatibility_version 1.0.0 -current_version 1.0.0
glue -o "$mc/libSDL3.dylib" -I"$sdl3/include" "$fixes/Minecraft (Java Edition)/glue.m" "$mc/libwmglhost.dylib" \
    -Wl,-reexport_library,"$work/libSDL3-real.dylib" -install_name @loader_path/libSDL3.dylib
glue -o "$mc/libglfw.dylib" -I"$glfw/include" "$fixes/Minecraft (Java Edition)/glue_glfw.m" "$mc/libwmglhost.dylib" \
    -Wl,-reexport_library,"$work/libglfw-real.dylib" -install_name @loader_path/libglfw.dylib

mw="$out/openmw"
mkdir -p "$mw"
cc -o "$mw/libwmglhost.dylib" "$work/procs.c" "$work/stubs.S" -install_name @executable_path/../Frameworks/libwmglhost.dylib \
    -compatibility_version 1.0.0 -current_version 1.0.0
glue -o "$mw/libSDL2-2.0.0.dylib" "$fixes/The Elder Scrolls III Morrowind (OpenMW)/glue.m" "$mw/libwmglhost.dylib" \
    -Wl,-reexport_library,"$work/libSDL2-real.dylib" -install_name @executable_path/../Frameworks/libSDL2-2.0.0.dylib \
    -compatibility_version 3201.0.0 -current_version 3201.10.0

# WINEMETALGL_SIGN_IDENTITY: a Developer ID signature (stage-release.sh); else ad hoc.
for f in "$mc"/*.dylib "$mw"/*.dylib; do
    if test -n "${WINEMETALGL_SIGN_IDENTITY:-}"; then
        codesign --force --options runtime --timestamp --sign "$WINEMETALGL_SIGN_IDENTITY" "$f"
    else
        codesign --force --sign - "$f"
    fi
done
printf 'Built the arm64 glue: %s\n' "$out"
