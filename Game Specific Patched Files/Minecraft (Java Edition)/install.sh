#!/bin/sh
# Builds the SDL3 glue and installs it, with the arm64 core, into a folder
# that a Minecraft launcher points LWJGL at (README.md). Idempotent: run it
# again after rebuilding the core.
#
#   install.sh [/path/to/output] [/path/to/libwinemetalgl.dylib]
#
# The output holds libwmglhost.dylib (every gl* name, for
# -Dorg.lwjgl.opengl.libname), libwinemetalgl.dylib, and the glue for each
# windowing library found in Minecraft's LWJGL libraries:
# - 26.x: libSDL3.dylib (the SDL_GL_* glue, for -Dorg.lwjgl.sdl.libname)
#   over libSDL3-real.dylib, LWJGL's own SDL3. Needs SDL3's headers
#   (brew install sdl3).
# - 1.21.x and older: libglfw.dylib (the GLFW context glue, for
#   -Dorg.lwjgl.glfw.libname) over libglfw-real.dylib, LWJGL's own GLFW.
#   Needs GLFW's headers (brew install glfw).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-"$HOME/.local/share/winemetalgl/minecraft"}
core=${2:-$root/build/arm64-release/libwinemetalgl.dylib}
generated="$root/build/arm64-release/generated/gl_entry.gen.cpp"
sdl_include=${SDL3_INCLUDE:-$(brew --prefix sdl3 2>/dev/null)/include}
glfw_include=${GLFW_INCLUDE:-$(brew --prefix glfw 2>/dev/null)/include}
libraries="$HOME/Library/Application Support/minecraft/libraries/org/lwjgl
$HOME/Library/Application Support/PrismLauncher/libraries/org/lwjgl"
test -e "$core" -a -e "$generated" || {
    printf '%s\n' "build the arm64 core first: scripts/stage-arm64-core.sh (or build/arm64-release)" >&2
    exit 1
}
# The newest natives jar of an LWJGL module, from either launcher.
natives() {
    printf '%s\n' "$libraries" | while IFS= read -r dir; do
        find "$dir" -name "$1-*natives-macos-arm64*.jar" 2>/dev/null
    done | sort | tail -n 1
}
sdl_jar=${LWJGL_SDL_JAR:-$(natives lwjgl-sdl)}
glfw_jar=${LWJGL_GLFW_JAR:-$(natives lwjgl-glfw)}
test -n "$sdl_jar$glfw_jar" || { printf '%s\n' "no LWJGL SDL3 or GLFW natives; set LWJGL_SDL_JAR or LWJGL_GLFW_JAR" >&2; exit 1; }
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$root/Game Specific Patched Files/The Elder Scrolls III Morrowind (OpenMW)/gen_stubs.py" "$generated" "$work"
clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -o "$out/libwmglhost.dylib" \
    "$work/procs.c" "$work/stubs.S" \
    -install_name @loader_path/libwmglhost.dylib \
    -compatibility_version 1.0.0 -current_version 1.0.0
args=""
if test -n "$sdl_jar" && test -e "$sdl_include/SDL3/SDL.h"; then
    unzip -p "$sdl_jar" macos/arm64/org/lwjgl/sdl/libSDL3.dylib > "$out/libSDL3-real.dylib"
    install_name_tool -id @loader_path/libSDL3-real.dylib "$out/libSDL3-real.dylib" 2>/dev/null
    clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -fobjc-arc -Wall -o "$out/libSDL3.dylib" \
        -I"$root/include" -I"$sdl_include" -I"$work" "$here/glue.m" "$out/libwmglhost.dylib" \
        -framework Cocoa -framework QuartzCore -framework Metal \
        -Wl,-reexport_library,"$out/libSDL3-real.dylib" \
        -install_name @loader_path/libSDL3.dylib
    args="$args
26.x: -Dorg.lwjgl.sdl.libname=$out/libSDL3.dylib -Dorg.lwjgl.opengl.libname=$out/libwmglhost.dylib"
elif test -n "$sdl_jar"; then
    printf '%s\n' "skipping the SDL3 glue: no SDL3 headers (brew install sdl3)" >&2
fi
if test -n "$glfw_jar" && test -e "$glfw_include/GLFW/glfw3.h"; then
    unzip -p "$glfw_jar" macos/arm64/org/lwjgl/glfw/libglfw.dylib > "$out/libglfw-real.dylib"
    install_name_tool -id @loader_path/libglfw-real.dylib "$out/libglfw-real.dylib" 2>/dev/null
    clang -dynamiclib -arch arm64 -mmacosx-version-min=15.0 -O2 -fobjc-arc -Wall -o "$out/libglfw.dylib" \
        -I"$root/include" -I"$glfw_include" -I"$work" "$here/glue_glfw.m" "$out/libwmglhost.dylib" \
        -framework Cocoa -framework QuartzCore -framework Metal \
        -Wl,-reexport_library,"$out/libglfw-real.dylib" \
        -install_name @loader_path/libglfw.dylib
    args="$args
1.21.x: -Dorg.lwjgl.glfw.libname=$out/libglfw.dylib -Dorg.lwjgl.opengl.libname=$out/libwmglhost.dylib"
elif test -n "$glfw_jar"; then
    printf '%s\n' "skipping the GLFW glue: no GLFW headers (brew install glfw)" >&2
fi
cp "$core" "$out/libwinemetalgl.dylib"
for f in "$out"/*.dylib; do codesign --force --sign - "$f" 2>&1 | grep -v "replacing existing signature" || true; done
printf 'installed into %s\n' "$out"
printf 'JVM arguments:%s\n' "$args"
