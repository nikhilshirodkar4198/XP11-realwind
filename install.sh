#!/bin/bash
set -e

# Check for cmake
if ! command -v cmake &>/dev/null; then
    echo "cmake not found. Install it with: brew install cmake"
    exit 1
fi

# Build
echo "Building RealWind..."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" 2>&1
make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cd ..

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
    echo ""
    echo "Build succeeded but X-Plane 11 not found in common locations."
    echo "Copy manually:"
    echo "  mkdir -p \"X-Plane 11/Resources/plugins/RealWind/64\""
    echo "  cp build/RealWind.xpl \"X-Plane 11/Resources/plugins/RealWind/64/mac.xpl\""
    exit 0
fi

DEST="$XP/Resources/plugins/RealWind/64"
mkdir -p "$DEST"
cp build/RealWind.xpl "$DEST/mac.xpl"
echo ""
echo "Installed → $DEST/mac.xpl"
echo "Launch X-Plane and check Log.txt for: RealWind: loaded"
