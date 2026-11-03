# Mocktail recipes for Bazzite (immutable Fedora).
#
# Bazzite bundles `just`, so run these directly from the repo:
#   just --justfile packaging/bazzite/mocktail.just --list
#   just --justfile packaging/bazzite/mocktail.just mocktail-install
#
# On custom Universal Blue images, import this file from
# /usr/share/ublue-os/just/60-custom.just to expose the recipes via `ujust`.
# See packaging/bazzite/README.md for the full guide.

app_id := "space.bigrat.mocktail"
flatpak_config := env_var("HOME") + "/.var/app/" + app_id + "/config/mocktail/config.yaml"
flatpak_logs := env_var("HOME") + "/.var/app/" + app_id + "/.local/state/mocktail/logs/latest.log"
nightly_ref := "https://mocktail.bigrat.space/mocktail.flatpakref"

# Install the stable Flatpak from Flathub.
[group('Mocktail')]
mocktail-install:
    flatpak install -y flathub {{ app_id }}

# Install the nightly Flatpak build from main (user scope).
[group('Mocktail')]
mocktail-install-nightly:
    flatpak install -y --user {{ nightly_ref }}

# Update the installed Flatpak.
[group('Mocktail')]
mocktail-update:
    flatpak update -y {{ app_id }}

# Run Mocktail.
[group('Mocktail')]
mocktail-run:
    flatpak run {{ app_id }}

# Uninstall the Flatpak (keeps ~/.var/app data; add --delete-data to remove it).
[group('Mocktail')]
mocktail-uninstall:
    flatpak uninstall -y {{ app_id }}

# Enable or disable the MangoHud Vulkan overlay for the Flatpak.
# Requires the layer for the runtime's freedesktop base branch, e.g.:
#   flatpak install flathub org.freedesktop.Platform.VulkanLayer.MangoHud
[group('Mocktail')]
mocktail-mangohud action="on":
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{ action }}" in
      on)
        flatpak override --user --env=MANGOHUD=1 {{ app_id }}
        flatpak override --user --filesystem=xdg-config/MangoHud:ro {{ app_id }}
        echo "MangoHud enabled for {{ app_id }} (restart Mocktail)."
        ;;
      off)
        flatpak override --user --unset-env=MANGOHUD {{ app_id }}
        echo "MangoHud disabled for {{ app_id }}."
        ;;
      *)
        echo "usage: mocktail-mangohud on|off" >&2
        exit 2
        ;;
    esac

# Set performance.gamemode in the Flatpak config (auto, on, or off).
# Bazzite ships gamemoded, so the default 'auto' already activates it.
[group('Mocktail')]
mocktail-gamemode mode="auto":
    #!/usr/bin/env bash
    set -euo pipefail
    config="{{ flatpak_config }}"
    case "{{ mode }}" in
      auto|on|off) ;;
      *)
        echo "usage: mocktail-gamemode auto|on|off" >&2
        exit 2
        ;;
    esac
    if [[ ! -f "$config" ]]; then
      echo "config not found: $config" >&2
      echo "launch Mocktail once so it creates the config, then rerun." >&2
      exit 1
    fi
    sed -i -E 's/^([[:space:]]*gamemode:).*/\1 {{ mode }}/' "$config"
    grep -E '^[[:space:]]*gamemode:' "$config"

# Layer the RPM onto the immutable base with rpm-ostree (last resort,
# requires a reboot, can pin future image updates until removed).
[group('Mocktail')]
mocktail-layer-rpm package="mocktail":
    #!/usr/bin/env bash
    set -euo pipefail
    echo "Layering {{ package }} with rpm-ostree modifies the read-only base"
    echo "image and requires a reboot. Prefer 'mocktail-install' (Flatpak)."
    read -r -p "Continue? [y/N] " answer
    case "$answer" in
      y|Y|yes|YES|Yes) ;;
      *) echo "aborted"; exit 1 ;;
    esac
    sudo curl -fsSL https://mocktail.bigrat.space/rpm/mocktail.repo \
      -o /etc/yum.repos.d/mocktail.repo
    sudo rpm-ostree install "{{ package }}"
    echo "Reboot to apply: sudo systemctl reboot"

# Print Mocktail's Flatpak config/log locations and tail the latest log.
[group('Mocktail')]
mocktail-logs:
    #!/usr/bin/env bash
    set -euo pipefail
    echo "config: {{ flatpak_config }}"
    echo "log:    {{ flatpak_logs }}"
    if [[ -f "{{ flatpak_logs }}" ]]; then
      tail -n 40 "{{ flatpak_logs }}"
    else
      echo "no log yet; launch Mocktail once first."
    fi
