#!/bin/sh
# Makes the disk image WineMetalGL Settings is released in: the app and an
# alias to /Applications to drag it onto, compressed, and signed.
#
#   [WINEMETALGL_SIGN_IDENTITY=...] scripts/build-settings-dmg.sh APP DMG
#
# APP is signed already (scripts/sign-settings-app.sh; notarized and stapled
# in a release). The image is signed with WINEMETALGL_SIGN_IDENTITY (a
# Developer ID Application identity, with a secure timestamp), or ad hoc.
# Notarizing and stapling it are the caller's (the release workflow's).
set -eu

APP=${1:?usage: build-settings-dmg.sh APP DMG}
DMG=${2:?usage: build-settings-dmg.sh APP DMG}
APP=${APP%/}
test -d "$APP/Contents" || { echo "not an app: $APP" >&2; exit 1; }
codesign --verify --strict --deep "$APP" || { echo "sign the app first: $APP" >&2; exit 1; }

stage=$(mktemp -d "${TMPDIR:-/tmp}/wmgl-dmg.XXXXXX")
trap 'rm -rf "$stage"' EXIT
ditto "$APP" "$stage/WineMetalGL Settings.app"
ln -s /Applications "$stage/Applications"
rm -f "$DMG"
hdiutil create -quiet -volname "WineMetalGL Settings" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$DMG"
if test -n "${WINEMETALGL_SIGN_IDENTITY:-}"; then
    codesign --force --timestamp --sign "$WINEMETALGL_SIGN_IDENTITY" "$DMG"
else
    codesign --force --sign - "$DMG"
fi
codesign --verify --strict "$DMG"
echo "Built: $DMG"
