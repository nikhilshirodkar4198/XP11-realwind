# XP11-RealWind

Injects real-world wind into X-Plane 11 at every altitude from the ground to FL600. Works alongside X-Plane live weather — clouds and visibility are untouched.

**Sources:** METAR (surface, nearest airport) · HRRR 1h (CONUS aloft) · ICON 3h (global aloft)

## Install

1. [Download ZIP](https://github.com/nikhilshirodkar4198/XP11-realwind/archive/refs/heads/main.zip)
2. Extract
3. Drag the `RealWind` folder into `X-Plane 11/Resources/plugins/`

Launch X-Plane. Check `Log.txt` for `RealWind: loaded` to confirm.

## Build from source

```sh
SDK_PATH=$(xcrun --show-sdk-path)
clang++ -dynamiclib -undefined dynamic_lookup \
  -arch arm64 -arch x86_64 \
  -DAPL=1 -DIBM=0 -DLIN=0 -DXPLM200 -DXPLM210 \
  -std=c++11 -I SDK -isysroot "$SDK_PATH" -lcurl \
  -o RealWind/64/mac.xpl RealWindPlugin.cpp
```
Requires Xcode CLT and the [X-Plane SDK](https://developer.x-plane.com/sdk/) in `SDK/`.
