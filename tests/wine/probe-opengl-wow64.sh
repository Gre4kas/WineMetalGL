#!/bin/bash
# Runs the Wine fixtures (tests/wine/fixtures.txt) on WineMetalGL in one fresh
# WoW64 prefix, as x86_64 and i386 guests.
#
#   WINEMETALGL_WINE_RUNTIME=/path/to/wine-install tests/wine/probe-opengl-wow64.sh [--all] [fixture...]
#
# The Wine installation must carry the patches in patches/wine and
# libwinemetalgl.dylib next to winemac.so (scripts/stage-wine.sh). --all also
# runs fixtures outside the gate scope (none since GL 4.6). Exit status is
# non-zero if any fixture misses a marker; a summary lists every fixture.
set -uo pipefail

PROJECT_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WINE_RUNTIME=${WINEMETALGL_WINE_RUNTIME:-}
if test -z "$WINE_RUNTIME"; then echo 'Set WINEMETALGL_WINE_RUNTIME to a compatible Wine installation.' >&2; exit 2; fi
# Wine's win32u/dwrite native modules dlopen FreeType by soname. Keep the
# host closure explicit and relocatable instead of depending on Homebrew's
# global search path.
WINE_HOST_LIB_DIR=${WINEMETALGL_HOST_LIB_DIR:-$WINE_RUNTIME/lib}
if test -d "$WINE_HOST_LIB_DIR"; then
    export DYLD_LIBRARY_PATH="$WINE_HOST_LIB_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
fi
WINE="$WINE_RUNTIME/bin/wine"
WINESERVER="$WINE_RUNTIME/bin/wineserver"
CC64=${CC64:-x86_64-w64-mingw32-gcc}
CC32=${CC32:-i686-w64-mingw32-gcc}
RUN_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/winemetalgl-wow64.XXXXXX")
PREFIX="$RUN_ROOT/prefix"
TIMEOUT_SECONDS=${WINEMETALGL_TIMEOUT_SECONDS:-300}
if command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT=(gtimeout --signal=TERM --kill-after=10s "${TIMEOUT_SECONDS}s")
elif command -v timeout >/dev/null 2>&1; then
    TIMEOUT=(timeout --signal=TERM --kill-after=10s "${TIMEOUT_SECONDS}s")
else
    TIMEOUT=()
fi

cleanup() {
    status=$?
    WINEPREFIX="$PREFIX" "$WINESERVER" -k 2>/dev/null || true
    WINEPREFIX="$PREFIX" "$WINESERVER" -w 2>/dev/null || true
    if test "${WINEMETALGL_KEEP_RUN:-0}" = 1; then
        echo "Retained probe run: $RUN_ROOT" >&2
    else
        rm -rf "$RUN_ROOT"
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

run_all=0
only=()
for arg in "$@"; do
    case "$arg" in
    --all) run_all=1 ;;
    *) only+=("$arg") ;;
    esac
done

core_dir="$WINE_RUNTIME/lib/wine/x86_64-unix"
# WINEMETALGL_MODULE=1: the Wine module (scripts/install-wine-module.sh)
# instead of the winemac integration (patches/wine).
module=${WINEMETALGL_MODULE:-0}
if test "$module" = 1; then inputs=("$core_dir/winemetalgl.so" "$core_dir/libwinemetalgl.dylib")
else inputs=("$core_dir/winemac.so" "$core_dir/libwinemetalgl.dylib"); fi
for required in "$WINE" "$WINESERVER" "${inputs[@]}"; do
    test -e "$required" || { echo "Missing WineMetalGL input: $required" >&2; exit 1; }
done

export WINEPREFIX="$PREFIX" WINEARCH=wow64 WINEDEBUG=${WINEDEBUG:--all}
unset WINEMETALGL
# A stock Wine offers to install Wine Mono and Gecko on first boot and waits
# on that dialog: the fixtures need neither.
WINEDLLOVERRIDES="mscoree,mshtml=${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}" \
    ${TIMEOUT[@]+"${TIMEOUT[@]}"} "$WINE" wineboot -u >"$RUN_ROOT/wineboot.log" 2>&1
if test "$module" = 1; then
    MODULE=${WINEMETALGL_MODULE_DIR:-$PROJECT_ROOT/build/wine-module}
    test -e "$MODULE/x86_64-windows/opengl32.dll" || { echo "Missing $MODULE/x86_64-windows/opengl32.dll" >&2; exit 1; }
    cp "$MODULE/x86_64-windows/opengl32.dll" "$PREFIX/drive_c/windows/system32/opengl32.dll"
    cp "$MODULE/i386-windows/opengl32.dll" "$PREFIX/drive_c/windows/syswow64/opengl32.dll"
    "$WINE" reg add 'HKCU\Software\Wine\DllOverrides' /v opengl32 /d native /f >>"$RUN_ROOT/wineboot.log" 2>&1
fi

failed=0
summary=()
while read -r source defines scope markers; do
    case "$source" in ''|'#'*) continue ;; esac
    if test "${#only[@]}" -gt 0 2>/dev/null; then
        printf '%s\n' "${only[@]}" | grep -qx "$source" || continue
    elif test "$scope" != gate && test "$run_all" = 0; then
        continue
    fi
    flags=()
    test "$defines" != - && flags=($defines)
    name="$source${defines#-}"
    name=${name//[^A-Za-z0-9_]/_}
    for bits in 64 32; do
        cc=$CC64
        test "$bits" = 32 && cc=$CC32
        exe="$RUN_ROOT/$name-$bits.exe"
        out="$exe.stdout"
        if ! "$cc" -O2 ${flags[@]+"${flags[@]}"} -I"$WINE_RUNTIME/include" "$PROJECT_ROOT/tests/wine/$source.c" -o "$exe" \
            -lopengl32 -luser32 -lgdi32 -ladvapi32 >"$exe.build.log" 2>&1; then
            summary+=("BUILD-FAIL $name ($bits-bit)")
            failed=1
            continue
        fi
        marker_file="$exe.marker"
        ${TIMEOUT[@]+"${TIMEOUT[@]}"} "$WINE" "$exe" "Z:$marker_file" >"$out" 2>&1
        rc=$?
        test -f "$marker_file" && cat "$marker_file" >>"$out"
        missing=()
        for marker in $markers; do
            grep -q "$marker" "$out" || missing+=("$marker")
        done
        if test "$source" = opengl_load_probe && test ! -f "$marker_file"; then missing+=("marker file"); fi
        if test "${#missing[@]}" -eq 0; then
            summary+=("PASS $name ($bits-bit)")
        else
            summary+=("FAIL $name ($bits-bit) rc=$rc missing: ${missing[*]}")
            failed=1
            tail -5 "$out" | sed 's/^/    /' >&2
        fi
    done
done <"$PROJECT_ROOT/tests/wine/fixtures.txt"

if test "${#summary[@]}" -eq 0; then
    echo 'no fixtures matched' >&2
    exit 1
fi
printf '%s\n' "${summary[@]}"
if test "$failed" = 0; then
    echo WINEMETALGL_WOW64_FIXTURES_OK
fi
exit "$failed"
