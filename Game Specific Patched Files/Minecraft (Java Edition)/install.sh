#!/bin/sh
# Builds the SDL3 glue and installs it, with the arm64 core, into a folder
# that a Minecraft launcher points LWJGL at (README.md). Idempotent: run it
# again after rebuilding the core.
#
#   install.sh [/path/to/output] [/path/to/libwinemetalgl.dylib]
#
# The output holds libwmglhost.dylib (every gl* name, for
# -Dorg.lwjgl.opengl.libname), libSDL3.dylib (the SDL_GL_* glue, for
# -Dorg.lwjgl.sdl.libname), libSDL3-real.dylib (LWJGL's own SDL3, which the
# glue re-exports) and libwinemetalgl.dylib. Needs SDL3's headers
# (brew install sdl3) and Minecraft's LWJGL libraries.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-"$HOME/.local/share/winemetalgl/minecraft"}
core=${2:-$root/build/arm64-release/libwinemetalgl.dylib}
generated="$root/build/arm64-release/generated/gl_entry.gen.cpp"
sdl_include=${SDL3_INCLUDE:-$(brew --prefix sdl3 2>/dev/null)/include}
test -e "$core" -a -e "$generated" || {
    printf '%s\n' "build the arm64 core first: scripts/stage-arm64-core.sh (or build/arm64-release)" >&2
    exit 1
}
test -e "$sdl_include/SDL3/SDL.h" || { printf '%s\n' "no SDL3 headers: brew install sdl3" >&2; exit 1; }
jar=${LWJGL_SDL_JAR:-$(find "$HOME/Library/Application Support/minecraft/libraries/org/lwjgl/lwjgl-sdl" \
    "$HOME/Library/Application Support/PrismLauncher/libraries/org/lwjgl/lwjgl-sdl" \
    -name 'lwjgl-sdl-*-natives-macos-arm64.jar' 2>/dev/null | sort | tail -n 1)}
test -n "$jar" || { printf '%s\n' "no lwjgl-sdl natives-macos-arm64 jar; set LWJGL_SDL_JAR" >&2; exit 1; }
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$root/Game Specific Patched Files/The Elder Scrolls III Morrowind (OpenMW)/gen_stubs.py" "$generated" "$work"
unzip -p "$jar" macos/arm64/org/lwjgl/sdl/libSDL3.dylib > "$out/libSDL3-real.dylib"
install_name_tool -id @loader_path/libSDL3-real.dylib "$out/libSDL3-real.dylib" 2>/dev/null
clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -o "$out/libwmglhost.dylib" \
    "$work/procs.c" "$work/stubs.S" \
    -install_name @loader_path/libwmglhost.dylib \
    -compatibility_version 1.0.0 -current_version 1.0.0
clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -fobjc-arc -Wall -o "$out/libSDL3.dylib" \
    -I"$root/include" -I"$sdl_include" -I"$work" "$here/glue.m" "$out/libwmglhost.dylib" \
    -framework Cocoa -framework QuartzCore -framework Metal \
    -Wl,-reexport_library,"$out/libSDL3-real.dylib" \
    -install_name @loader_path/libSDL3.dylib
cp "$core" "$out/libwinemetalgl.dylib"
for f in "$out"/*.dylib; do codesign --force --sign - "$f" 2>&1 | grep -v "replacing existing signature" || true; done
printf 'installed into %s\n' "$out"
printf '%s\n' "JVM arguments:" \
    "-Dorg.lwjgl.sdl.libname=$out/libSDL3.dylib" \
    "-Dorg.lwjgl.opengl.libname=$out/libwmglhost.dylib"
