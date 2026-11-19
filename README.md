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

## Install (macOS)

1. [Download this repo as a ZIP](https://github.com/nikhilshirodkar4198/XP11-realwind/archive/refs/heads/main.zip)
2. Extract it
3. Drag the `RealWind` folder into `X-Plane 11/Resources/plugins/`

Done. Launch X-Plane normally — plugins load automatically.

## Verify

Launch X-Plane 11 and open `Log.txt` in the X-Plane folder. You should see:
```
RealWind: loaded
RealWind: sfc 270/12kt(METAR)  10k 285/45kt  30k 310/88kt
```

## Build from source (optional)

Only needed if you want to modify the code:
```sh
SDK_PATH=$(xcrun --show-sdk-path)
clang++ -dynamiclib -undefined dynamic_lookup \
  -arch arm64 -arch x86_64 \
  -DAPL=1 -DIBM=0 -DLIN=0 -DXPLM200 -DXPLM210 \
  -std=c++11 -I SDK -isysroot "$SDK_PATH" -lcurl \
  -o RealWind.xpl RealWindPlugin.cpp
```

> **Note:** Leave X-Plane's built-in real weather **enabled**. The plugin re-injects METAR wind every 2 seconds to override X-Plane's wind values while leaving clouds, visibility, and pressure untouched.
