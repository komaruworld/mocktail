<p align="center">
  <img src="https://raw.githubusercontent.com/komaruworld/mocktail/refs/heads/main/packaging/icons/hicolor/192x192/apps/space.bigrat.mocktail.png" alt="Mocktail logo" width="128" height="128" />
</p>

<h1 align="center">Mocktail</h1>

<p align="center">
  <a href="https://github.com/komaruworld/mocktail/actions/workflows/ci.yml"><img src="https://github.com/komaruworld/mocktail/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/komaruworld/mocktail/stargazers"><img src="https://img.shields.io/github/stars/komaruworld/mocktail?style=flat&logo=github" alt="Stars" /></a>
  <a href="https://github.com/komaruworld/mocktail/releases/latest"><img src="https://img.shields.io/github/downloads/komaruworld/mocktail/total?logo=github" alt="Downloads" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License" /></a>
</p>

<p align="center">
  Run the Android <code>x86_64</code> Roblox client on Linux.
</p>

Mocktail hosts a Linux userspace — via FreeBSD's Linuxulator when needed — and provides the Android ABI and JNI pieces the Roblox client expects, then connects them to SDL3 and Vulkan or OpenGL on the Linux side.

> Mocktail is an independent community project. It is **not** affiliated with Roblox Corporation or VinegarHQ, and it does not distribute the Roblox client.

---

## Table of contents

- [How it works](#how-it-works)
- [Installation](#installation)
  - [Flatpak](#flatpak)
  - [AUR (Arch Linux)](#aur-arch-linux)
  - [DNF (Fedora)](#dnf-fedora)
  - [APT (Ubuntu)](#apt-ubuntu)
  - [Direct downloads](#direct-downloads)
- [Screenshots](#screenshots)
- [FFlag overrides](#fflag-overrides)
- [Building from source](#building-from-source)
  - [Dependencies](#dependencies)
  - [Build steps](#build-steps)
- [License](#license)
- [Support](#support)

---

## How it works

```
Roblox APK -> signature and ABI checks -> Bionic + JNI -> SDL3 + Vulkan/OpenGL
```

The APK is downloaded on first launch and is checked before any native code runs — it is not bundled with Mocktail. If an update fails, Mocktail falls back to the last known-good copy.

---

## Installation

### Flatpak

Stable release from Flathub:

```bash
flatpak install flathub space.bigrat.mocktail
flatpak run space.bigrat.mocktail
```

Nightly build, produced automatically from the latest `main` branch:

```bash
flatpak install --user https://mocktail.bigrat.space/mocktail.flatpakref
flatpak run space.bigrat.mocktail
```

### AUR (Arch Linux)

| Package | Description |
|---|---|
| `mocktail-bin` | Stable, prebuilt |
| `mocktail` | Stable, built from source |
| `mocktail-git` | Development version, built from `main` |

```bash
paru -S mocktail-git   # or: yay -S mocktail-git
```

### DNF (Fedora)

Fedora 44+:

```bash
sudo curl -fsSL https://mocktail.bigrat.space/rpm/mocktail.repo \
  -o /etc/yum.repos.d/mocktail.repo

sudo dnf install mocktail            # stable
# or
sudo dnf install mocktail-nightly    # built from main
```

### APT (Ubuntu)

Ubuntu 26.04+:

```bash
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL https://mocktail.bigrat.space/mocktail-packages.gpg \
  -o /etc/apt/keyrings/mocktail.gpg

echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/mocktail.gpg] https://mocktail.bigrat.space/apt mocktail main" \
  | sudo tee /etc/apt/sources.list.d/mocktail.list >/dev/null

sudo apt update
sudo apt install mocktail            # stable
# or
sudo apt install mocktail-nightly    # built from main
```

### Direct downloads

AppImage, DEB, and RPM builds are available from the [website](https://mocktail.bigrat.space/) and from [GitHub Releases](https://github.com/komaruworld/mocktail/releases).

---

## Screenshots

<details>
<summary>Click to expand</summary>

| Home | Gameplay | Lobby |
|---|---|---|
| ![Roblox home in Mocktail](assets/screenshots/flatpak-home.png) | ![Roblox gameplay in Mocktail](assets/screenshots/flatpak-gameplay-tower.png) | ![Roblox experience in Mocktail](assets/screenshots/flatpak-gameplay-lobby.png) |

</details>

---

## FFlag overrides

Put a JSON object in `$XDG_CONFIG_HOME/mocktail/fflags.json` (usually `~/.config/mocktail/fflags.json`). Mocktail applies it on the next launch.

```json
{
  "FFlagExample": "True",
  "DFIntExample": "120"
}
```

---

## Building from source

**Supported platforms:** Linux `x86_64`.

Experimental FreeBSD 15.1 support runs Mocktail inside Linuxulator (not as a native FreeBSD binary), tested with an `x86_64` Fedora 44 userspace. See the [FreeBSD guide](packaging/freebsd/README.md) for setup and launch instructions.

### Dependencies

CMake 3.20+, Git, pkg-config, LLD, binutils, a C++17 compiler, SDL 3.4+, SDL3_ttf, Vulkan, EGL, libplacebo, fontconfig, libcurl, OpenSSL, libelf, libyaml, minizip, Capstone 5, utf8proc, nlohmann/json, GTK4, libadwaita 1.6+, WebKitGTK 6.0.

<details>
<summary>Ubuntu 26.04+</summary>

```bash
sudo apt update
sudo apt install build-essential cmake git ninja-build pkg-config lld \
  libsdl3-dev libsdl3-ttf-dev libcurl4-openssl-dev libssl-dev \
  nlohmann-json3-dev libyaml-dev libelf-dev libminizip-dev \
  libcapstone-dev libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev \
  libutf8proc-dev libfontconfig1-dev libegl-dev libvulkan-dev \
  libplacebo-dev zlib1g-dev
```
</details>

<details>
<summary>Arch Linux</summary>

```bash
sudo pacman -S --needed base-devel cmake git ninja pkgconf lld sdl3 sdl3_ttf \
  curl openssl nlohmann-json libyaml libelf minizip capstone gtk4 \
  libadwaita webkitgtk-6.0 libutf8proc fontconfig libglvnd \
  libplacebo vulkan-headers vulkan-icd-loader zlib
```
</details>

<details>
<summary>Fedora 44+</summary>

```bash
sudo dnf install gcc-c++ cmake git ninja-build pkgconf-pkg-config lld \
  SDL3-devel SDL3_ttf-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel elfutils-libelf-devel minizip-ng-compat-devel \
  capstone-devel gtk4-devel libadwaita-devel webkitgtk6.0-devel \
  utf8proc-devel fontconfig-devel libglvnd-devel vulkan-headers \
  vulkan-loader-devel libplacebo-devel zlib-ng-compat-devel
```
</details>

### Build steps

```bash
git clone --recurse-submodules https://github.com/komaruworld/mocktail.git
cd mocktail
make build
./build/mocktail
```

---

## License

[Apache License 2.0](LICENSE). Third-party components keep their own licenses.

---

## Support

Give the project a star, or support it with crypto:

- **USDT (TON):** `UQCi6Yzcc9cOctoij6n_r1K90-OdVxAT0D_xo2UzGKkQaJDY`
- **USDT (TRC20):** `TNPMG9Vig2xiuo2r1QqnXRChPH7Vu28Jmx`
