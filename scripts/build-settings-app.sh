#!/bin/sh
# Builds "WineMetalGL Settings.app" (src/settings-app): UpscaleGL's preset,
# x87 acceleration and logging, written to the settings file the core reads.
#
#   [PAYLOAD=DIR] [ARM64_CORE=DIR] scripts/build-settings-app.sh [OUTDIR]
#
# The app carries the install scripts (Contents/Resources/Installer) to
# install WineMetalGL into CrossOver bottles and Wines. PAYLOAD is a staged
# x86_64 release (scripts/stage-release.sh), carried whole as Installer: its
# module, docs, evidence and licenses. Without it, the scripts download the
# latest release. ARM64_CORE is a staged arm64 core
# (scripts/stage-arm64-core.sh), carried as Contents/Resources/arm64-core. With
# both, as in a release, the app's disk image is a whole WineMetalGL runtime.
#
# The app also carries the game fixes it applies where their games are
# installed (Contents/Resources/GamePatches, from Game Specific Patched
# Files; src/settings-app/GamePatches.swift): Quake 4's, SpaceEngine's and
# Wolfenstein: The New Order's. SOURCE-SHA256 there lists the fixed files
# as the repository has them, before scripts/sign-settings-app.sh signs them.
# The arm64 fixes (Minecraft, OpenMW: src/settings-app/Arm64Fixes.swift)
# install the glue the staged arm64 core carries (ARM64_CORE/glue,
# scripts/build-arm64-glue.sh).
#
# A universal (arm64, x86_64) app for macOS 14 and later; its Liquid Glass
# needs the macOS 26 SDK (Xcode 26) to build, and macOS 26 to show (before
# it, the app simulates it; WINEMETALGL_SIMULATED_GLASS=1 shows that). The
# icon is WineMetalGL's Liquid Glass icon (src/settings-app/WineMetalGL.icon),
# compiled by actool (full Xcode: ACTOOL, or xcrun's); without actool, a
# plain .icns stands in: src/settings-app/WineMetalGL-plain.png, the artwork
# on the icon's dark gradient (readable on a light Dock). Signing is the caller's
# (scripts/sign-settings-app.sh; the release workflow signs with the
# Developer ID, then notarizes, and scripts/build-settings-dmg.sh makes the
# disk image). APP_VERSION replaces the version (to try an update).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/settings-app}
SRC=$ROOT/src/settings-app
APP="$OUT/WineMetalGL Settings.app"
VERSION=$(sed -n 's/^project(WineMetalGL VERSION \([0-9][0-9.]*\) .*/\1/p' "$ROOT/CMakeLists.txt")
test -n "$VERSION" || { echo "no project() version in CMakeLists.txt" >&2; exit 1; }
VERSION=${APP_VERSION:-$VERSION}

rm -rf "$APP" "$OUT/obj"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$OUT/obj"

for arch in arm64 x86_64; do
    xcrun swiftc -O -parse-as-library -target "$arch-apple-macos14.0" "$SRC"/*.swift -o "$OUT/obj/settings-$arch"
done
lipo -create "$OUT/obj/settings-arm64" "$OUT/obj/settings-x86_64" -output "$APP/Contents/MacOS/WineMetalGL Settings"

mkdir -p "$APP/Contents/Resources/Installer"
cp "$ROOT/scripts/install-wine-module.sh" "$ROOT/scripts/install-crossover.sh" "$APP/Contents/Resources/Installer/"
if test -n "${PAYLOAD:-}"; then
    test -f "$PAYLOAD/module/x86_64-unix/libwinemetalgl.dylib" || { echo "no staged release in $PAYLOAD" >&2; exit 1; }
    cp -R "$PAYLOAD"/. "$APP/Contents/Resources/Installer/"
fi
if test -n "${ARM64_CORE:-}"; then
    test -f "$ARM64_CORE/lib/libwinemetalgl.dylib" || { echo "no staged arm64 core in $ARM64_CORE" >&2; exit 1; }
    mkdir -p "$APP/Contents/Resources/arm64-core"
    cp -R "$ARM64_CORE"/. "$APP/Contents/Resources/arm64-core/"
fi

FIXES="$ROOT/Game Specific Patched Files"
PATCHES="$APP/Contents/Resources/GamePatches"
mkdir -p "$PATCHES/Quake 4" "$PATCHES/SpaceEngine" "$PATCHES/Wolfenstein The New Order"
cp "$FIXES/Quake 4/README.md" "$FIXES/Quake 4/legacy-display-modes.reg" "$FIXES/Quake 4/apply-display-modes.sh" \
    "$FIXES/Quake 4/autoexec.cfg" "$PATCHES/Quake 4/"
cp "$FIXES/SpaceEngine/README.md" "$FIXES/SpaceEngine/winemac.so" "$PATCHES/SpaceEngine/"
cp "$FIXES/Wolfenstein The New Order/README.md" "$FIXES/Wolfenstein The New Order/xaudio2_7.dll" \
    "$PATCHES/Wolfenstein The New Order/"
(cd "$FIXES" && shasum -a 256 SpaceEngine/winemac.so "Wolfenstein The New Order/xaudio2_7.dll") > "$PATCHES/SOURCE-SHA256"

# The logo at the top of the window: the icon's artwork.
sips -Z 256 "$SRC/WineMetalGL.icon/Assets/winemetalgl-transparent.png" --out "$APP/Contents/Resources/WineMetalGLLogo.png" >/dev/null

ACTOOL_BIN=${ACTOOL:-$(xcrun --find actool 2>/dev/null || true)}
if test -n "$ACTOOL_BIN" && "$ACTOOL_BIN" --version >/dev/null 2>&1; then
    "$ACTOOL_BIN" --compile "$OUT/obj" --platform macosx --minimum-deployment-target 14.0 --app-icon WineMetalGL \
        --output-partial-info-plist "$OUT/obj/partial-Info.plist" "$SRC/WineMetalGL.icon" >/dev/null
    test -s "$OUT/obj/Assets.car" || { echo "actool made no Assets.car" >&2; exit 1; }
    cp "$OUT/obj/Assets.car" "$APP/Contents/Resources/"
    cp "$OUT/obj/WineMetalGL.icns" "$APP/Contents/Resources/"
else
    echo "no actool (full Xcode): a plain icon stands in for the Liquid Glass one" >&2
    set=$OUT/obj/WineMetalGL.iconset
    mkdir -p "$set"
    for size in 16 32 128 256 512; do
        sips -Z $size "$SRC/WineMetalGL-plain.png" --out "$set/icon_${size}x${size}.png" >/dev/null
        sips -Z $((size * 2)) "$SRC/WineMetalGL-plain.png" --out "$set/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$APP/Contents/Resources/WineMetalGL.icns"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>WineMetalGL Settings</string>
    <key>CFBundleExecutable</key><string>WineMetalGL Settings</string>
    <key>CFBundleIconFile</key><string>WineMetalGL</string>
    <key>CFBundleIconName</key><string>WineMetalGL</string>
    <key>CFBundleIdentifier</key><string>com.metalsharp.winemetalgl.settings</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>WineMetalGL Settings</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
rm -rf "$OUT/obj"
echo "Built: $APP"
