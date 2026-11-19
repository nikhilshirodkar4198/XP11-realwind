#!/bin/bash
set -e

PLUGIN="$(dirname "$0")/RealWind.xpl"

if [ ! -f "$PLUGIN" ]; then
    echo "RealWind.xpl not found. Try re-downloading the repo."
    exit 1
fi

# Find X-Plane 11
XP=""
for p in \
    "/Applications/X-Plane 11" \
    "$HOME/Desktop/X-Plane 11" \
    "$HOME/X-Plane 11" \
    "$HOME/Library/Application Support/Steam/steamapps/common/X-Plane 11"; do
    if [ -d "$p" ]; then XP="$p"; break; fi
done

if [ -z "$XP" ]; then
    echo "X-Plane 11 not found in common locations."
    echo "Copy manually:"
    echo "  mkdir -p \"X-Plane 11/Resources/plugins/RealWind/64\""
    echo "  cp RealWind.xpl \"X-Plane 11/Resources/plugins/RealWind/64/mac.xpl\""
    exit 1
fi

DEST="$XP/Resources/plugins/RealWind/64"
mkdir -p "$DEST"
cp "$PLUGIN" "$DEST/mac.xpl"
echo "Installed → $DEST/mac.xpl"
echo "Launch X-Plane and check Log.txt for: RealWind: loaded"
