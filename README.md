# Mocktail VR

Experimental VR support for [Mocktail](https://github.com/komaruworld/mocktail).
Linux x86_64, WiVRn or SteamVR/ALVR.

[![CI](https://github.com/komaruworld/mocktail/actions/workflows/ci.yml/badge.svg)](https://github.com/komaruworld/mocktail/actions/workflows/ci.yml)
[![Stars](https://img.shields.io/github/stars/komaruworld/mocktail?style=flat&logo=github)](https://github.com/komaruworld/mocktail/stargazers)
[![Downloads](https://img.shields.io/github/downloads/komaruworld/mocktail/total?logo=github)](https://github.com/komaruworld/mocktail/releases/latest)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Discord](https://img.shields.io/discord/1543955607869063269?label=discord)](https://discord.gg/rhgpfcmFSD)

Mocktail runs the Android `x86_64` Roblox client on Linux and has experimental
`aarch64` builds for the Android `arm64-v8a` client. It also runs in a Linux
userspace hosted by FreeBSD's Linuxulator. Mocktail provides the Android ABI
and JNI pieces the client expects, then connects them to SDL3 and Vulkan or
OpenGL on the Linux side.

Mocktail is an independent community project. It is not affiliated with Roblox
Corporation or VinegarHQ and does not distribute the Roblox client.

## VR build and launch

VR device profiles cover the exact x86-64 Roblox builds 2998, 3092, and 3222.
The 3222 profile includes its changed controller-pointer and camera layout.

### Build

```bash
git clone --branch vr --recurse-submodules https://github.com/komaruworld/mocktail.git
cd mocktail
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTAIL_ENABLE_VR=ON
cmake --build build -j4
```

Build dependencies are listed in the [main README](https://github.com/komaruworld/mocktail#building).

### Run

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

## How it works

```
Roblox APK -> signature and ABI checks -> Bionic + JNI -> SDL3 + Vulkan/OpenGL
```

The APK is checked before any native code is loaded. It is downloaded on first
launch and is not bundled with Mocktail. The last working copy is kept in case
an update fails.

## Install with Flatpak

Install the stable release from Flathub:

```bash
flatpak install flathub space.bigrat.mocktail
flatpak run space.bigrat.mocktail
```

Nightly builds are produced automatically from the latest `main` branch:

```bash
flatpak install --user https://mocktail.bigrat.space/mocktail.flatpakref
flatpak run space.bigrat.mocktail
```

## Install from the AUR

Arch Linux users can install either the pinned source release (`mocktail`) or
the current development version (`mocktail-git`) with an AUR helper:

```bash
paru -S mocktail-git
# or
yay -S mocktail-git
```

Use `mocktail` instead of `mocktail-git` to build the pinned release from
source, or install the stable prebuilt package with `paru -S mocktail-bin` or
`yay -S mocktail-bin`.

## Install with DNF

Fedora 44 users can install either the stable `mocktail` package or the
`mocktail-nightly` package built from `main`:

```bash
sudo curl -fsSL https://mocktail.bigrat.space/rpm/mocktail.repo \
  -o /etc/yum.repos.d/mocktail.repo
sudo dnf install mocktail
# or
sudo dnf install mocktail-nightly
```

## Install with APT

Ubuntu 26.04 users can install either the stable `mocktail` package or the
`mocktail-nightly` package built from `main`:

```bash
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL https://mocktail.bigrat.space/mocktail-packages.gpg \
  -o /etc/apt/keyrings/mocktail.gpg
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/mocktail.gpg] https://mocktail.bigrat.space/apt mocktail main" | \
  sudo tee /etc/apt/sources.list.d/mocktail.list >/dev/null
sudo apt update
sudo apt install mocktail
# or
sudo apt install mocktail-nightly
```

## Direct downloads

Direct AppImage, DEB, and RPM downloads are available from the
[Website](https://mocktail.bigrat.space/) and
[GitHub Releases](https://github.com/komaruworld/mocktail/releases).

<details>
<summary>Screenshots</summary>

![Roblox home in Mocktail](assets/screenshots/flatpak-home.png)

![Roblox gameplay in Mocktail](assets/screenshots/flatpak-gameplay-tower.png)

![Roblox experience in Mocktail](assets/screenshots/flatpak-gameplay-lobby.png)

</details>

## FFlag overrides

Put a JSON object in `$XDG_CONFIG_HOME/mocktail/fflags.json` (usually
`~/.config/mocktail/fflags.json`). Mocktail applies it on the next launch.

PC profiles use the legacy Charts page by default to avoid the blank screen
caused by `Color3` errors in the newer SDUI page. An explicit
`FFlagLuaAppChartsAppPage` override takes precedence over this default.

```json
{
  "FFlagExample": "True",
  "DFIntExample": "120"
}
```
## FAQ

To see the most Frequently Asked Question go check [FAQ.md](FAQ.md)


## Building

Linux `x86_64` is supported. Linux `aarch64` builds are available, with Roblox
runtime validation still experimental. FreeBSD 15.1 Linuxulator support is
experimental and has been tested with an `x86_64` Fedora 44 userspace. On
FreeBSD, Mocktail runs inside Linuxulator; it is not a native FreeBSD binary.
Building requires CMake 3.20+, Git, pkg-config, LLD, binutils, a C++17
compiler, SDL 3.4+, SDL3_ttf,
Vulkan, EGL, libplacebo, fontconfig, libcurl, OpenSSL, libelf, libyaml, minizip,
Capstone 5, utf8proc, nlohmann/json, GTK4, libadwaita 1.6+, and WebKitGTK 6.0.
Optional: libsecret.

See the [FreeBSD Guide](packaging/freebsd/README.md) for setup and launch instructions.

<details>
<summary>Ubuntu 26.04+</summary>

```bash
sudo apt update
sudo apt install build-essential cmake git ninja-build pkg-config lld \
  libsdl3-dev libsdl3-ttf-dev libcurl4-openssl-dev libssl-dev \
  nlohmann-json3-dev libyaml-dev libelf-dev libminizip-dev \
  libcapstone-dev libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev libsecret-1-dev \
  libutf8proc-dev libfontconfig1-dev libegl-dev libvulkan-dev \
  libplacebo-dev libpng-dev zlib1g-dev
```
</details>

<details>
<summary>Arch Linux</summary>

```bash
sudo pacman -S --needed base-devel cmake git ninja pkgconf lld sdl3 sdl3_ttf \
  curl openssl nlohmann-json libyaml libelf minizip capstone gtk4 \
  libadwaita libsecret webkitgtk-6.0 libutf8proc fontconfig libglvnd \
  libplacebo vulkan-headers vulkan-icd-loader zlib
```
</details>

<details>
<summary>Fedora 44+</summary>

```bash
sudo dnf install gcc-c++ cmake git ninja-build pkgconf-pkg-config lld \
  SDL3-devel SDL3_ttf-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel elfutils-libelf-devel minizip-ng-compat-devel \
  capstone-devel gtk4-devel libadwaita-devel webkitgtk6.0-devel libsecret-devel \
  utf8proc-devel fontconfig-devel libglvnd-devel vulkan-headers \
  vulkan-loader-devel libplacebo-devel zlib-ng-compat-devel
```
</details>

```bash
git clone --recurse-submodules https://github.com/komaruworld/mocktail.git
cd mocktail
make build
./build/mocktail
```

## Known limitations

Mocktail does not currently work with `hardened_malloc`. Using
`hardened_malloc` may cause Mocktail to fail to start or crash during runtime.
Run Mocktail without `hardened_malloc` enabled.

## License

[Apache License 2.0](LICENSE). Third-party components keep their own licenses.

## Support

You can support the project by giving it a star or with cryptocurrency:

- USDT (TON): `UQCi6Yzcc9cOctoij6n_r1K90-OdVxAT0D_xo2UzGKkQaJDY`
- USDT (TRC20): `TNPMG9Vig2xiuo2r1QqnXRChPH7Vu28Jmx`
