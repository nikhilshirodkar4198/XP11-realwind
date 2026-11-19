# XP11-RealWind

Injects real-world wind into X-Plane 11 at every altitude from the ground to FL600. Works with or without X-Plane live weather — clouds and visibility are untouched.

**Sources:** METAR (surface, nearest airport) · HRRR 1h (CONUS aloft) · ICON 3h (global aloft)

## How it works

All 12 levels (surface to 70 hPa) are fetched every 5 minutes. X-Plane 11 has 3 wind layers, so every 2 seconds the plugin sets:

- **Layer 0** — surface wind (METAR below 5,000 ft AGL, model 10 m otherwise) at ground elevation
- **Layers 1–2** — the two fetched levels bracketing the aircraft's altitude

X-Plane interpolates between them, so the wind at the aircraft always comes from real data. METAR gusts drive surface turbulence.

## Install

1. [Download ZIP](https://github.com/nikhilshirodkar4198/XP11-realwind/archive/refs/heads/main.zip)
2. Extract
3. Drag the `RealWind` folder into `X-Plane 11/Resources/plugins/`

The folder contains `64/win.xpl` (Windows) and `64/mac.xpl` (macOS); X-Plane picks the right one.

> **Note:** `mac.xpl` predates the 3-layer fix and won't load in X-Plane 11 — rebuild it with the command below.

Launch X-Plane. Check `Log.txt` for `RealWind: loaded` to confirm.

## Build from source

Both builds need the [X-Plane SDK](https://developer.x-plane.com/sdk/).

**Windows** (Visual Studio 2022 Build Tools, uses WinHTTP — no extra libraries):

```bat
build_win.bat C:\path\to\XPSDK
```

**macOS** (Xcode CLT, SDK headers in `SDK/`):

```sh
SDK_PATH=$(xcrun --show-sdk-path)
clang++ -dynamiclib -undefined dynamic_lookup \
  -arch arm64 -arch x86_64 \
  -DAPL=1 -DIBM=0 -DLIN=0 -DXPLM200 -DXPLM210 \
  -std=c++11 -I SDK -isysroot "$SDK_PATH" -lcurl \
  -o RealWind/64/mac.xpl RealWindPlugin.cpp
```
