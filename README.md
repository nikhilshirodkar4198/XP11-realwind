# XP11-RealWind

Injects real-world METAR wind into X-Plane 11 via the [aviationweather.gov](https://aviationweather.gov) API. No API key required.

## How it works

Every 5 minutes the plugin reads the aircraft's lat/lon, queries the nearest METAR station within a 1° bounding box, and writes the surface wind direction and speed to X-Plane's weather datarefs.

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

> **Note:** Disable X-Plane's built-in real weather ("Download real weather" in Weather settings) to prevent it from overriding the injected values.
