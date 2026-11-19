# XP11-RealWind

Injects real-world winds aloft into X-Plane 11 via [Open-Meteo](https://open-meteo.com). No API key required. Works alongside X-Plane live weather — clouds, visibility, and pressure are untouched.

## Wind layers injected

| Layer | Pressure | Altitude |
|-------|----------|----------|
| 0 | 1000 hPa | ~364 ft |
| 1 | 925 hPa | ~2,500 ft |
| 2 | 850 hPa | ~5,000 ft |
| 3 | 700 hPa | ~10,000 ft |
| 4 | 500 hPa | ~18,000 ft |
| 5 | 300 hPa | ~30,000 ft |
| 6 | 200 hPa | ~39,000 ft |
| 7 | 150 hPa | ~44,600 ft |
| 8 | 100 hPa | ~53,000 ft |
| 9 | 70 hPa | ~60,000 ft |

## How it works

Every 5 minutes the plugin reads the aircraft's lat/lon, queries Open-Meteo for hourly wind forecasts at 7 pressure levels, and re-injects all 7 layers into X-Plane's wind datarefs every 2 seconds to override the sim's own weather updates.

Log output (X-Plane Log.txt): `RealWind: 270 deg @ 15 kts`

## Install (macOS / Linux)

**Prerequisites:** Xcode Command Line Tools + CMake
```sh
xcode-select --install   # macOS only, skip if already installed
brew install cmake       # or download from cmake.org
```

**Clone and install in one step:**
```sh
git clone https://github.com/nikhilshirodkar4198/XP11-realwind.git
cd XP11-realwind
./install.sh
```

The script builds the plugin and auto-detects your X-Plane 11 installation. No SDK download, no extra libraries.

## Install (Windows)

Windows requires the [X-Plane SDK](https://developer.x-plane.com/sdk/) for the XPLM linker stub:
```sh
mkdir build && cd build
cmake .. -DXPLANE_SDK=C:/path/to/SDK
cmake --build . --config Release
```
Copy `build/RealWind.xpl` → `X-Plane 11/Resources/plugins/RealWind/64/win.xpl`

> **Note:** Leave X-Plane's built-in real weather **enabled**. The plugin re-injects METAR wind every 2 seconds to override X-Plane's wind values while leaving clouds, visibility, and pressure untouched.
