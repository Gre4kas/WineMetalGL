#!/bin/sh
# Imports legacy-display-modes.reg into a Wine prefix or a CrossOver bottle,
# then stops its wineserver so the next program sees the new mode list.
#
#   ./apply-display-modes.sh WINEPREFIX [WINE]
#   ./apply-display-modes.sh "CrossOver bottle name or path"
#
# WINE is the wine binary for a prefix (default: wine on PATH, or
# MetalSharp's when the prefix is under ~/.metalsharp).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
TARGET=${1:?usage: apply-display-modes.sh WINEPREFIX [WINE] | BOTTLE}
REG="$HERE/legacy-display-modes.reg"
BOTTLES="$HOME/Library/Application Support/CrossOver/Bottles"

case "$TARGET" in
*/*) ;;
*) test -d "$BOTTLES/$TARGET" && TARGET="$BOTTLES/$TARGET" ;;
esac
test -d "$TARGET" || { echo "no such prefix or bottle: $TARGET" >&2; exit 1; }
TARGET=$(cd "$TARGET" && pwd)

if test -f "$TARGET/cxbottle.conf"; then
    CX=
    for app in /Applications/CrossOver.app "$HOME/Applications/CrossOver.app"; do
        test -d "$app" && { CX="$app/Contents/SharedSupport/CrossOver"; break; }
    done
    test -n "$CX" || { echo "CrossOver.app not found" >&2; exit 1; }
    BOTTLE=$(basename "$TARGET")
    "$CX/bin/wine" --bottle "$BOTTLE" regedit /S "$REG"
    if test -x "$CX/bin/wineserver"; then
        WINEPREFIX="$TARGET" "$CX/bin/wineserver" -k || true
    else
        echo "Quit the bottle's programs (or restart CrossOver) for the new modes to apply."
    fi
    echo "Applied to CrossOver bottle $BOTTLE"
    exit 0
fi

WINE=${2:-}
if test -z "$WINE"; then
    case "$TARGET" in
    "$HOME/.metalsharp/"*) WINE="$HOME/.metalsharp/runtime/wine/bin/wine" ;;
    *) WINE=$(command -v wine || true) ;;
    esac
fi
test -x "$WINE" || { echo "no wine binary; pass it: apply-display-modes.sh WINEPREFIX WINE" >&2; exit 1; }
WINEPREFIX="$TARGET" "$WINE" regedit /S "$REG"
WINEPREFIX="$TARGET" "$(dirname "$WINE")/wineserver" -k || true
echo "Applied to $TARGET"
