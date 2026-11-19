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

## How it works

Every 5 minutes the plugin reads the aircraft's lat/lon, queries Open-Meteo for hourly wind forecasts at 7 pressure levels, and re-injects all 7 layers into X-Plane's wind datarefs every 2 seconds to override the sim's own weather updates.

Log output (X-Plane Log.txt): `RealWind: 270 deg @ 15 kts`

## Dependencies

- [X-Plane SDK](https://developer.x-plane.com/sdk/) (headers only — place in `SDK/` or pass `-DXPLANE_SDK=...`)
- libcurl (`brew install curl` / `apt install libcurl4-openssl-dev` / vcpkg on Windows)
- CMake 3.15+

## Build

```sh
mkdir build && cd build
cmake .. -DXPLANE_SDK=/path/to/XPSDK
cmake --build . --config Release
```

Output: `RealWind.xpl`

## Install

Copy `RealWind.xpl` to:

```
X-Plane 11/Resources/plugins/RealWind/64/win.xpl   # Windows
X-Plane 11/Resources/plugins/RealWind/64/lin.xpl   # Linux
X-Plane 11/Resources/plugins/RealWind/64/mac.xpl   # macOS
```

> **Note:** Leave X-Plane's built-in real weather **enabled**. The plugin re-injects METAR wind every 2 seconds to override X-Plane's wind values while leaving clouds, visibility, and pressure untouched.
