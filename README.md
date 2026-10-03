# Mocktail VR

Experimental VR support for [Mocktail](https://github.com/komaruworld/mocktail).
Linux x86_64, WiVRn or SteamVR/ALVR.

## Build

```bash
git clone --branch vr --recurse-submodules https://github.com/komaruworld/mocktail.git
cd mocktail
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTAIL_ENABLE_VR=ON
cmake --build build -j4
```

Build dependencies are listed in the [main README](https://github.com/komaruworld/mocktail#building).

## Run

Start WiVRn or SteamVR/ALVR and connect your headset first.

```bash
./build/mocktail --vr
```

The runtime is detected automatically. To pick one manually:

```bash
MOCKTAIL_VR_RUNTIME=wivrn ./build/mocktail --vr
# or
MOCKTAIL_VR_RUNTIME=alvr MOCKTAIL_GRAPHICS_BACKEND=direct-vulkan ./build/mocktail --vr
```

If you previously set `XR_RUNTIME_JSON`, unset it before switching runtimes.
Use `--no-vr` for desktop mode.

