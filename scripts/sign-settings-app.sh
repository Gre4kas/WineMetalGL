#!/bin/sh
# Signs "WineMetalGL Settings.app" (scripts/build-settings-app.sh) for
# release: first every Mach-O and DLL among the game fixes it carries
# (Contents/Resources/GamePatches), so the copies the app installs into a
# Wine keep a valid signature, then the app.
#
#   [WINEMETALGL_SIGN_IDENTITY=...] scripts/sign-settings-app.sh APP
#
# WINEMETALGL_SIGN_IDENTITY is a Developer ID Application identity (the
# release workflow's), used with the hardened runtime and a secure
# timestamp; without it, everything is signed ad hoc. The release libraries
# the app carries (Contents/Resources/Installer/module) keep the signatures
# scripts/stage-release.sh gave them, and are only checked. The DLLs are PE
# files: theirs is a generic signature, which codesign keeps in their
# extended attributes. Notarizing is the caller's.
set -eu

APP=${1:?usage: sign-settings-app.sh APP}
APP=${APP%/}
test -x "$APP/Contents/MacOS/WineMetalGL Settings" || { echo "not WineMetalGL Settings.app: $APP" >&2; exit 1; }
identity=${WINEMETALGL_SIGN_IDENTITY:-}
if test -n "$identity"; then
    set -- --force --options runtime --timestamp --sign "$identity"
else
    set -- --force --sign -
fi

for f in "$APP/Contents/Resources/Installer/module/x86_64-unix/libwinemetalgl.dylib" \
         "$APP/Contents/Resources/Installer/module/x86_64-unix/winemetalgl.so" \
         "$APP/Contents/Resources/Installer/module/x86_64-unix/x87sidecar"; do
    test -e "$f" || continue
    codesign --verify --strict "$f" || { echo "the carried release is not signed: $f" >&2; exit 1; }
done

count=0
find "$APP/Contents/Resources/GamePatches" -type f > "${TMPDIR:-/tmp}/wmgl-fixes.$$"
while IFS= read -r f; do
    case "$f" in
    *.dll) ;;
    *) file -b "$f" | grep -q '^Mach-O' || continue ;;
    esac
    xattr -c "$f"
    codesign "$@" "$f"
    codesign --verify --strict "$f"
    count=$((count + 1))
done < "${TMPDIR:-/tmp}/wmgl-fixes.$$"
rm -f "${TMPDIR:-/tmp}/wmgl-fixes.$$"
test "$count" -gt 0 || { echo "no game fixes to sign in $APP" >&2; exit 1; }
echo "Signed $count game fix files"

codesign "$@" "$APP"
codesign --verify --strict --deep "$APP"
echo "Signed $APP"
