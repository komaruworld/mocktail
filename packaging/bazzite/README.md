# Mocktail on Bazzite (immutable Fedora)

Bazzite is an image-based Fedora Atomic desktop (Universal Blue): `/usr` is
read-only, updates are atomic via `rpm-ostree`, and apps are expected to come
from Flatpak, containers (distrobox), or AppImages. Bazzite reports
`ID=bazzite` / `ID_LIKE="fedora"` in `/etc/os-release` with
`VARIANT_ID=bazzite-*`, and it already ships the pieces that make Mocktail
fast out of the box: the GameMode daemon (`gamemoded`, D-Bus activated),
MangoHud, Gamescope, PipeWire, and Mesa/NVIDIA Vulkan drivers. Stable Bazzite
44 tracks Fedora 44, which is exactly what the Mocktail RPM repository is
built for.

Mocktail detects Bazzite at startup and records it in every session log and
support bundle (`os="Bazzite 44 (immutable)" ... bazzite=yes immutable=yes`),
so bug reports are unambiguous.

## Recommended: Flatpak

The Flatpak bundles its own SDL3/WebKitGTK stack and already carries
`--talk-name=com.feralinteractive.GameMode`, so GameMode works on Bazzite with
no extra setup:

```bash
flatpak install flathub space.bigrat.mocktail
flatpak run space.bigrat.mocktail
```

Nightly builds from `main`:

```bash
flatpak install --user https://mocktail.bigrat.space/mocktail.flatpakref
```

Flatpak paths (config, FFlags, logs):

```bash
~/.var/app/space.bigrat.mocktail/config/mocktail/config.yaml
~/.var/app/space.bigrat.mocktail/config/mocktail/fflags.json
~/.var/app/space.bigrat.mocktail/.local/state/mocktail/logs/latest.log
```

## ujust / just recipes

`packaging/bazzite/mocktail.just` ships ready-made recipes. Bazzite bundles
`just`, so run them directly:

```bash
just --justfile packaging/bazzite/mocktail.just --list
just --justfile packaging/bazzite/mocktail.just mocktail-install
```

On custom images you can also expose them through `ujust` by importing the
file from `/usr/share/ublue-os/just/60-custom.just`.

Available recipes: `mocktail-install`, `mocktail-install-nightly`,
`mocktail-update`, `mocktail-run`, `mocktail-uninstall`, `mocktail-mangohud`,
`mocktail-gamemode`, `mocktail-layer-rpm`, `mocktail-logs`.

## Last resort: layer the RPM with rpm-ostree

The RPM repository targets Fedora 44, matching Bazzite 44. Layering is
discouraged on immutable systems (it can pin future image updates until
removed) and requires a reboot:

```bash
sudo curl -fsSL https://mocktail.bigrat.space/rpm/mocktail.repo \
  -o /etc/yum.repos.d/mocktail.repo
sudo rpm-ostree install mocktail        # or: mocktail-nightly
sudo systemctl reboot
```

Remove the layer with `sudo rpm-ostree uninstall mocktail` (or
`rpm-ostree reset` to drop all layers) before rebasing to a newer Bazzite.

## AppImage / native build: use distrobox

The thin AppImage installs its host libraries with a package manager, which
cannot work on the read-only Bazzite base. Mocktail's launcher detects the
immutable host and prints this guidance instead of failing obscurely.

Use a Fedora distrobox container (preconfigured on Bazzite):

```bash
distrobox create --name mocktail --image registry.fedoraproject.org/fedora-toolbox:44
distrobox enter mocktail
# inside the container, install the Fedora 44 build dependencies listed in
# README.md ("Building"), then either build from source (make build) or run
# the standalone AppImage, which bundles its own libraries:
sudo dnf install gcc-c++ cmake git ninja-build pkgconf-pkg-config lld \
  SDL3-devel SDL3_ttf-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel elfutils-libelf-devel minizip-ng-compat-devel \
  capstone-devel gtk4-devel libadwaita-devel webkitgtk6.0-devel libsecret-devel \
  utf8proc-devel fontconfig-devel libglvnd-devel vulkan-headers \
  vulkan-loader-devel libplacebo-devel zlib-ng-compat-devel
```

The standalone AppImage only needs the host GPU driver and Vulkan ICD from
Bazzite (already present); if you run it directly on the host instead of in a
container, make sure `bash java jq unzip file flock timeout readelf sha256sum`
are available and launch it with `MOCKTAIL_SKIP_HOST_CHECK=1`. Bazzite's
GearLever (`ujust` AppImage integration) can register the AppImage in the
menu, but the dependencies still have to come from a distrobox.

## Performance tuning on Bazzite

- **GameMode** — on by default. `performance.gamemode: auto` in
  `config.yaml` requests GameMode through libgamemode; Bazzite ships
  `gamemoded`, and the Flatpak already has the
  `com.feralinteractive.GameMode` D-Bus permission. Confirm in
  `latest.log` with `[gamemode] performance request active`. Set
  `gamemode: on` to make a missing daemon a visible warning, or `off` to opt
  out.
- **MangoHud overlay** — install the Vulkan layer for the Flatpak runtime's
  freedesktop base branch and enable it per-app:

  ```bash
  flatpak install flathub org.freedesktop.Platform.VulkanLayer.MangoHud   # pick the branch matching org.gnome.Platform//50
  flatpak override --user --env=MANGOHUD=1 space.bigrat.mocktail
  flatpak override --user --filesystem=xdg-config/MangoHud:ro space.bigrat.mocktail
  # or for a single run:
  flatpak run --env=MANGOHUD=1 space.bigrat.mocktail
  ```

- **Gamescope** (handhelds/HTPC) — Bazzite ships Gamescope:

  ```bash
  gamescope -- flatpak run space.bigrat.mocktail
  ```

- **Memory cap** — `performance.memory_limit_mb` uses cgroup v2 on Bazzite and
  also disables swap for the process tree while the cap is active. On laptops
  with an integrated GPU (e.g. Iris Xe on a 12th Gen i5-1235U), keep at least
  4 GiB of RAM outside the cap — Mocktail warns in the log if you don't.
- **Hybrid CPUs** — on Alder Lake+ (2 P-cores + 8 E-cores on the i5-1235U),
  Mocktail auto-detects the P/E split and sizes Roblox worker pools from the
  P-cores only; see the "Hardware tuning" section in the main README and the
  `performance.worker_cores` setting.
- **FFlags** — put overrides in
  `~/.var/app/space.bigrat.mocktail/config/mocktail/fflags.json` (see the main
  README).

## Troubleshooting

- Logs: `~/.var/app/space.bigrat.mocktail/.local/state/mocktail/logs/latest.log`
  (Flatpak) or `~/.local/state/mocktail/logs/latest.log` (native). The header
  now includes the host OS line, e.g.
  `os="Bazzite 44 (immutable)" os_id=bazzite os_variant_id=bazzite-nvidia immutable=yes bazzite=yes fedora=yes`.
- Support bundle: native builds — `make support-bundle`; the bundle's
  `runtime.txt` records the full os-release fields including `bazzite=yes`.
- `rpm-ostree status` shows layered packages and the current deployment;
  `ujust update` (or `rpm-ostree upgrade`) updates the base image.
