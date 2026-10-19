#!/bin/sh
# Builds "WineMetalGL Settings.app" (src/settings-app): UpscaleGL's preset
# and logging, written to the settings file the core reads.
#
#   [PAYLOAD=DIR] scripts/build-settings-app.sh [OUTDIR]
#
# The app carries the install scripts (Contents/Resources/Installer) to
# install WineMetalGL into CrossOver bottles and Wines. PAYLOAD is a staged
# x86_64 release (scripts/stage-release.sh: its module directory) to carry
# with them; without it, the scripts download the latest release.
#
# A universal (arm64, x86_64) app for macOS 14 and later; its Liquid Glass
# needs the macOS 26 SDK (Xcode 26) to build, and macOS 26 to show. The
# icon is MetalSharp's Liquid Glass icon (src/settings-app/MetalSharp.icon),
# compiled by actool (full Xcode: ACTOOL, or xcrun's); without actool, a
# plain .icns of its artwork stands in. Signing is the caller's (the release
# workflow signs with the Developer ID and notarizes).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/settings-app}
SRC=$ROOT/src/settings-app
APP="$OUT/WineMetalGL Settings.app"
VERSION=$(sed -n 's/^project(WineMetalGL VERSION \([0-9][0-9.]*\) .*/\1/p' "$ROOT/CMakeLists.txt")
test -n "$VERSION" || { echo "no project() version in CMakeLists.txt" >&2; exit 1; }

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
    cp -R "$PAYLOAD/module" "$APP/Contents/Resources/Installer/"
fi

# The logo at the top of the window: the icon's artwork.
sips -Z 256 "$SRC/MetalSharp.icon/Assets/metalsharp-transparent.png" --out "$APP/Contents/Resources/MetalSharpLogo.png" >/dev/null

ACTOOL_BIN=${ACTOOL:-$(xcrun --find actool 2>/dev/null || true)}
if test -n "$ACTOOL_BIN" && "$ACTOOL_BIN" --version >/dev/null 2>&1; then
    "$ACTOOL_BIN" --compile "$OUT/obj" --platform macosx --minimum-deployment-target 14.0 --app-icon MetalSharp \
        --output-partial-info-plist "$OUT/obj/partial-Info.plist" "$SRC/MetalSharp.icon" >/dev/null
    test -s "$OUT/obj/Assets.car" || { echo "actool made no Assets.car" >&2; exit 1; }
    cp "$OUT/obj/Assets.car" "$APP/Contents/Resources/"
    cp "$OUT/obj/MetalSharp.icns" "$APP/Contents/Resources/"
else
    echo "no actool (full Xcode): a plain icon stands in for the Liquid Glass one" >&2
    set=$OUT/obj/MetalSharp.iconset
    mkdir -p "$set"
    for size in 16 32 128 256 512; do
        sips -Z $size "$SRC/MetalSharp.icon/Assets/metalsharp-transparent.png" --out "$set/icon_${size}x${size}.png" >/dev/null
        sips -Z $((size * 2)) "$SRC/MetalSharp.icon/Assets/metalsharp-transparent.png" --out "$set/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$APP/Contents/Resources/MetalSharp.icns"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>WineMetalGL Settings</string>
    <key>CFBundleExecutable</key><string>WineMetalGL Settings</string>
    <key>CFBundleIconFile</key><string>MetalSharp</string>
    <key>CFBundleIconName</key><string>MetalSharp</string>
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
