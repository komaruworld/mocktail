#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail
umask 022

ANYLINUX_PROJECT_ROOT="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ANYLINUX_BUILD_DIR="${ANYLINUX_PROJECT_ROOT}/build"
ANYLINUX_OUTPUT=""
ANYLINUX_WORK=""
ANYLINUX_FREEBSD_HELPER=""
ANYLINUX_SYSTEM_INSTALL=false
ANYLINUX_DEPLOY_INSTALLED=false
ANYLINUX_TOOL="${MOCKTAIL_ANYLINUX_PACKAGER:-quick-sharun}"
ANYLINUX_APPIMAGETOOL="${MOCKTAIL_ANYLINUX_APPIMAGETOOL:-appimagetool}"

AnyLinuxDie() {
  printf '[anylinux] error: %s\n' "$*" >&2
  exit 1
}

AnyLinuxCleanup() {
  [[ -z "${ANYLINUX_WORK}" ]] || rm -rf -- "${ANYLINUX_WORK}"
}

AnyLinuxUsage() {
  cat <<'EOF'
Usage: scripts/package_anylinux.sh --build-dir DIR --appimage FILE [OPTIONS]

Install the native application to /usr, let quick-sharun deploy its complete
runtime, then run quick-sharun --make-appimage. No portable bundle is embedded.

By default /usr is a private, read-only bubblewrap overlay of a DESTDIR install;
the host installation is never modified. Requires an Arch Linux build host.
  --system-install  Install directly to /usr in a disposable build container.
                    Refused outside Docker/Podman containers. Used by CI.
  --deploy-installed  Internal: package the already prepared /usr installation.

MOCKTAIL_ANYLINUX_PACKAGER and MOCKTAIL_ANYLINUX_APPIMAGETOOL select the tools.
MOCKTAIL_ANYLINUX_SHARUN and MOCKTAIL_ANYLINUX_C_SOURCE select verified local
upstream inputs; quick-sharun itself installs/builds them into the AppDir.
EOF
}

AnyLinuxParseArguments() {
  while (( $# > 0 )); do
    case "$1" in
      --build-dir|--appimage)
        (( $# >= 2 )) || AnyLinuxDie "$1 requires a path"
        if [[ "$1" == --build-dir ]]; then
          ANYLINUX_BUILD_DIR="$2"
        else
          ANYLINUX_OUTPUT="$2"
        fi
        shift 2
        ;;
      --system-install) ANYLINUX_SYSTEM_INSTALL=true; shift ;;
      --deploy-installed) ANYLINUX_DEPLOY_INSTALLED=true; shift ;;
      -h|--help) AnyLinuxUsage; exit 0 ;;
      *) AnyLinuxDie "unknown option: $1" ;;
    esac
  done
  [[ -n "${ANYLINUX_OUTPUT}" ]] || AnyLinuxDie "--appimage is required"
  [[ "${ANYLINUX_OUTPUT}" != *$'\n'* &&
     "${ANYLINUX_OUTPUT}" != *$'\r'* ]] || AnyLinuxDie "unsafe output path"
  [[ ! -L "${ANYLINUX_OUTPUT}" ]] || AnyLinuxDie "refusing symlink output"
  ANYLINUX_OUTPUT="$(realpath -m -- "${ANYLINUX_OUTPUT}")"
  [[ "${ANYLINUX_OUTPUT}" == *.AppImage && ! -d "${ANYLINUX_OUTPUT}" ]] ||
    AnyLinuxDie "output must be an AppImage file"
}

AnyLinuxVerifyInstalled() {
  local entry
  for entry in /usr/bin/mocktail \
      /usr/lib/mocktail/mocktail_updater \
      /usr/lib/mocktail/mocktail_failure_dialog \
      /usr/lib/mocktail/mocktail_webview_helper; do
    [[ -x "${entry}" ]] || AnyLinuxDie "missing installed executable: ${entry}"
  done
  for entry in libEGL.so libvulkan.so libmediandk.so; do
    [[ -r "/usr/lib/mocktail/${entry}" ]] ||
      AnyLinuxDie "missing installed Android adapter: ${entry}"
  done
  for entry in roblox_compatibility.json roblox_bootstrap_sources.json \
      roblox_host_abi_reference.json roblox_signing_certificates.json; do
    [[ -r "/usr/share/mocktail/metadata/${entry}" ]] ||
      AnyLinuxDie "missing installed metadata: ${entry}"
  done
  LC_ALL=C readelf -l /usr/bin/mocktail | grep -q 'ld-linux.*so' ||
    AnyLinuxDie "AnyLinux requires a glibc runtime"
  if [[ -f /usr/share/mocktail/helpers/mocktail_freebsd_socket_helper ]]; then
    ANYLINUX_FREEBSD_HELPER=/usr/share/mocktail/helpers/mocktail_freebsd_socket_helper
  fi
}

AnyLinuxVerifyAppDir() {
  local app_dir="$1" entry
  [[ ! -e "${app_dir}/usr" ]] || AnyLinuxDie "unexpected usr prefix in AppDir"
  [[ ! -e "${app_dir}/share/mocktail-bundle" ]] ||
    AnyLinuxDie "a portable bundle was embedded in AppDir"
  for entry in mocktail mocktail_updater mocktail_failure_dialog \
      mocktail_webview_helper; do
    [[ -x "${app_dir}/bin/${entry}" ]] ||
      AnyLinuxDie "quick-sharun did not deploy ${entry}"
  done
  for entry in WebKitNetworkProcess WebKitWebProcess WebKitGPUProcess; do
    [[ -x "${app_dir}/lib/webkitgtk-6.0/${entry}" ]] ||
      AnyLinuxDie "quick-sharun did not preserve the WebKit process layout: ${entry}"
  done
  [[ -r "${app_dir}/lib/webkitgtk-6.0/injected-bundle/libwebkitgtkinjectedbundle.so" ]] ||
    AnyLinuxDie "missing injected WebKit bundle"
  [[ -f "${app_dir}/AppRun.sh" && -f "${app_dir}/AppRun.lib" ]] ||
    AnyLinuxDie "missing quick-sharun startup hooks"
  for entry in libEGL.so libvulkan.so libmediandk.so; do
    [[ -r "${app_dir}/lib/mocktail/${entry}" ]] ||
      AnyLinuxDie "Android adapter was not deployed in its private directory: ${entry}"
  done
  for entry in roblox_compatibility.json roblox_bootstrap_sources.json \
      roblox_host_abi_reference.json roblox_signing_certificates.json; do
    [[ -r "${app_dir}/share/mocktail/metadata/${entry}" ]] ||
      AnyLinuxDie "quick-sharun did not deploy metadata: ${entry}"
  done
  if [[ -n "${ANYLINUX_FREEBSD_HELPER}" ]]; then
    cmp -s -- "${ANYLINUX_FREEBSD_HELPER}" \
      "${app_dir}/share/mocktail/helpers/mocktail_freebsd_socket_helper" ||
      AnyLinuxDie "the static FreeBSD helper was changed or omitted"
  fi
}

AnyLinuxPrepareInstallTree() {
  local prefix="$1"
  # This static FreeBSD executable must not be turned into a Linux sharun
  # wrapper. Keep it as application data, outside the nested lib executables
  # that quick-sharun wraps. The native runtime uses its explicit .env path.
  local helper="${prefix}/lib/mocktail/mocktail_freebsd_socket_helper"
  if [[ -f "${helper}" ]]; then
    install -d -- "${prefix}/share/mocktail/helpers"
    mv -- "${helper}" "${prefix}/share/mocktail/helpers/"
  fi
}

AnyLinuxDeploy() {
  AnyLinuxVerifyInstalled
  local app_dir="${ANYLINUX_WORK}/AppDir"
  mkdir -p -- "${app_dir}" "${ANYLINUX_WORK}/tmp"
  # .env is runtime configuration, not a custom AppRun. The generated
  # upstream entry point must execute all relocation/certificate hooks.
  install -m 0644 -- "${ANYLINUX_PROJECT_ROOT}/packaging/anylinux.env" \
    "${app_dir}/.env"

  local -a deployment_environment=(
    "APPDIR=${app_dir}" "MAIN_BIN=mocktail" "LIB_DIR=/usr/lib"
    "TMPDIR=${ANYLINUX_WORK}/tmp"
    "DESKTOP=/usr/share/applications/space.bigrat.mocktail.desktop"
    "ICON=/usr/share/icons/hicolor/scalable/apps/space.bigrat.mocktail.svg"
    "DEPLOY_DATADIR=0" "DEPLOY_LOCALE=0" "DEPLOY_VULKAN=0"
    "STRACE_MODE=1" "STRACE_BINARY=mocktail" "STRACE_FLAGS=--help"
    "STRACE_TIME=1"
  )
  if [[ -n "${MOCKTAIL_ANYLINUX_SHARUN:-}" ]]; then
    [[ -x "${MOCKTAIL_ANYLINUX_SHARUN}" ]] || AnyLinuxDie "invalid sharun input"
    deployment_environment+=("SHARUN_LINK=file://$(realpath -e -- "${MOCKTAIL_ANYLINUX_SHARUN}")")
  fi
  if [[ -n "${MOCKTAIL_ANYLINUX_C_SOURCE:-}" ]]; then
    [[ -r "${MOCKTAIL_ANYLINUX_C_SOURCE}" ]] || AnyLinuxDie "invalid anylinux.c input"
    deployment_environment+=("ANYLINUX_LIB_SOURCE=file://$(realpath -e -- "${MOCKTAIL_ANYLINUX_C_SOURCE}")")
  fi
  # Upstream prefers wget, which cannot fetch file:// inputs. Adapt only
  # those local tool downloads; all network downloads still use real wget.
  if [[ -n "${MOCKTAIL_ANYLINUX_SHARUN:-}${MOCKTAIL_ANYLINUX_C_SOURCE:-}" ]] &&
      command -v wget >/dev/null; then
    local downloader_dir="${ANYLINUX_WORK}/download-tools"
    mkdir -p -- "${downloader_dir}"
    cat >"${downloader_dir}/wget" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
if [[ "$#" == 3 && "$1" == -qO && "$3" == file://* ]]; then
  cp -- "${3#file://}" "$2"
else
  exec "${MOCKTAIL_ANYLINUX_REAL_WGET}" "$@"
fi
EOF
    chmod 0755 -- "${downloader_dir}/wget"
    deployment_environment+=(
      "MOCKTAIL_ANYLINUX_REAL_WGET=$(command -v wget)"
      "PATH=${downloader_dir}:${PATH}"
    )
  fi

  printf '[anylinux] deploying the native /usr installation\n' >&2
  env "${deployment_environment[@]}" "${ANYLINUX_TOOL}" \
    /usr/bin/mocktail \
    /usr/lib/mocktail/mocktail_webview_helper \
    /usr/lib/mocktail/mocktail_failure_dialog \
    /usr/lib/mocktail/mocktail_updater \
    /usr/lib/mocktail /usr/share/mocktail /usr/bin/bash
  AnyLinuxVerifyAppDir "${app_dir}"

  # Use the upstream creation entry point, rather than maintaining a second
  # appimagetool invocation and a second copy of the userspace libraries.
  env "${deployment_environment[@]}" \
    APPIMAGETOOL="${ANYLINUX_APPIMAGETOOL}" \
    OUTPATH="${ANYLINUX_WORK}/image" \
    OUTNAME="$(basename -- "${ANYLINUX_OUTPUT}")" \
    "${ANYLINUX_TOOL}" --make-appimage
  local result="${ANYLINUX_WORK}/image/$(basename -- "${ANYLINUX_OUTPUT}")"
  [[ -x "${result}" ]] || AnyLinuxDie "quick-sharun did not create the requested AppImage"
  mv -f -- "${result}" "${ANYLINUX_OUTPUT}"
  printf '[anylinux] AppImage ready: %s\n' "${ANYLINUX_OUTPUT}" >&2
}

AnyLinuxMain() {
  AnyLinuxParseArguments "$@"
  local command_name
  for command_name in cmake readelf realpath "${ANYLINUX_TOOL}" "${ANYLINUX_APPIMAGETOOL}"; do
    command -v "${command_name}" >/dev/null ||
      AnyLinuxDie "missing required command: ${command_name}"
  done
  ANYLINUX_TOOL="$(realpath -e -- "$(command -v "${ANYLINUX_TOOL}")")"
  ANYLINUX_APPIMAGETOOL="$(realpath -e -- "$(command -v "${ANYLINUX_APPIMAGETOOL}")")"
  export MOCKTAIL_ANYLINUX_PACKAGER="${ANYLINUX_TOOL}"
  export MOCKTAIL_ANYLINUX_APPIMAGETOOL="${ANYLINUX_APPIMAGETOOL}"
  mkdir -p -- "$(dirname -- "${ANYLINUX_OUTPUT}")"
  ANYLINUX_WORK="$(mktemp -d "$(dirname -- "${ANYLINUX_OUTPUT}")/.mocktail-anylinux.XXXXXX")"
  trap AnyLinuxCleanup EXIT
  if [[ "${ANYLINUX_DEPLOY_INSTALLED}" == true ]]; then
    AnyLinuxDeploy
    return
  fi

  ANYLINUX_BUILD_DIR="$(realpath -e -- "${ANYLINUX_BUILD_DIR}")"
  [[ -f "${ANYLINUX_BUILD_DIR}/cmake_install.cmake" ]] ||
    AnyLinuxDie "build directory has no CMake installation rules"
  local install_root="${ANYLINUX_WORK}/install"
  if [[ "${ANYLINUX_SYSTEM_INSTALL}" == true ]]; then
    [[ -f /.dockerenv || -f /run/.containerenv ]] ||
      AnyLinuxDie "--system-install is only allowed in a disposable container"
    [[ "$(id -u)" == 0 ]] || AnyLinuxDie "container installation requires root"
    env -u DESTDIR cmake --install "${ANYLINUX_BUILD_DIR}" --prefix /usr
    AnyLinuxPrepareInstallTree /usr
    AnyLinuxDeploy
    return
  fi

  command -v bwrap >/dev/null || AnyLinuxDie "bwrap is required for a private /usr install"
  DESTDIR="${install_root}" cmake --install "${ANYLINUX_BUILD_DIR}" --prefix /usr
  AnyLinuxPrepareInstallTree "${install_root}/usr"
  # Overlay only /usr; keep the host root read-only, with narrowly scoped
  # writable packaging scratch/output directories. No host package install.
  bwrap --die-with-parent --ro-bind / / --dev-bind /dev /dev --proc /proc \
    --overlay-src /usr --overlay-src "${install_root}/usr" --ro-overlay /usr \
    --bind "$(dirname -- "${ANYLINUX_OUTPUT}")" "$(dirname -- "${ANYLINUX_OUTPUT}")" \
    --bind /tmp /tmp \
    -- "${BASH}" "${BASH_SOURCE[0]}" --deploy-installed --appimage "${ANYLINUX_OUTPUT}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  AnyLinuxMain "$@"
fi
