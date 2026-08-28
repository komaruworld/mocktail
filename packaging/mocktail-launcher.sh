#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

set -Eeuo pipefail

ResolveSelf() {
  local path="${BASH_SOURCE[0]}"
  while [[ -L "${path}" ]]; do
    local directory
    directory="$(cd -P -- "$(dirname -- "${path}")" && pwd)"
    path="$(readlink -- "${path}")"
    [[ "${path}" == /* ]] || path="${directory}/${path}"
  done
  cd -P -- "$(dirname -- "${path}")" && pwd
}

LAUNCHER_DIR="$(ResolveSelf)"
if [[ -n "${MOCKTAIL_BUNDLE_ROOT:-}" ]]; then
  BUNDLE_ROOT="$(cd -P -- "${MOCKTAIL_BUNDLE_ROOT}" && pwd)"
else
  BUNDLE_ROOT="$(cd -P -- "${LAUNCHER_DIR}/../.." && pwd)"
fi
RUNTIME_ROOT="${BUNDLE_ROOT}/mocktail"
BUNDLED_BIN_DIR="${RUNTIME_ROOT}/bin"
ANYLINUX_BIN_DIR="${MOCKTAIL_ANYLINUX_BIN_DIR:-}"
if [[ -n "${ANYLINUX_BIN_DIR}" ]]; then
  [[ "${ANYLINUX_BIN_DIR}" == /* && -d "${ANYLINUX_BIN_DIR}" ]] || {
    printf 'mocktail: invalid AnyLinux binary directory: %s\n' \
      "${ANYLINUX_BIN_DIR}" >&2
    exit 1
  }
  BIN_DIR="${ANYLINUX_BIN_DIR}"
else
  BIN_DIR="${BUNDLED_BIN_DIR}"
fi
MAIN_BINARY="${BIN_DIR}/mocktail"
UPDATE_HELPER="${BIN_DIR}/mocktail_updater"
FAILURE_DIALOG_HELPER="${BIN_DIR}/mocktail_failure_dialog"
WEBVIEW_HELPER="${BIN_DIR}/mocktail_webview_helper"
FREEBSD_SOCKET_HELPER="${BUNDLED_BIN_DIR}/mocktail_freebsd_socket_helper"
ANDROID_BUILD_TOOLS="${RUNTIME_ROOT}/runtime/android-tools/bin"
ANDROID_TOOL_EXEC_DIR="${ANYLINUX_BIN_DIR:-${ANDROID_BUILD_TOOLS}}"
METADATA_DIR="${RUNTIME_ROOT}/metadata"
ABI_MANIFEST="${METADATA_DIR}/ABI.txt"
DEPENDENCY_MANIFEST="${METADATA_DIR}/DEPENDENCIES.txt"
CHECKSUM_MANIFEST="${METADATA_DIR}/SHA256SUMS.txt"
SUPPORT_ROOT="${RUNTIME_ROOT}/runtime"
if [[ -n "${ANYLINUX_BIN_DIR}" ]]; then
  SUPPORT_BIN="${ANYLINUX_BIN_DIR}"
else
  SUPPORT_BIN="${SUPPORT_ROOT}/bin"
fi
export PATH="${BIN_DIR}:${SUPPORT_BIN}:${ANDROID_BUILD_TOOLS}:${PATH}"
export LD_LIBRARY_PATH="${RUNTIME_ROOT}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export MOCKTAIL_FREEBSD_SOCKET_HELPER="${FREEBSD_SOCKET_HELPER}"

Die() {
  printf 'mocktail: %s\n' "$*" >&2
  exit 1
}

ABI_SCHEMA=""
ABI_ARCHITECTURE=""
ABI_LIBC=""
ABI_MODE=""
ABI_INTERPRETER=""
ABI_ANDROID_TOOLS_LIBC=""
WEBKITGTK6_NAMESPACE_DIR=""

LoadAbiManifest() {
  [[ -f "${ABI_MANIFEST}" && ! -L "${ABI_MANIFEST}" ]] ||
    Die "missing regular ABI manifest: ${ABI_MANIFEST}"

  local key value
  local seen_schema=false seen_architecture=false seen_libc=false
  local seen_mode=false seen_interpreter=false
  local seen_android_tools_libc=false
  while IFS='=' read -r key value; do
    case "${key}" in
      schema)
        [[ "${seen_schema}" == false ]] ||
          Die "duplicate ABI manifest field: schema"
        seen_schema=true
        ABI_SCHEMA="${value}"
        ;;
      architecture)
        [[ "${seen_architecture}" == false ]] ||
          Die "duplicate ABI manifest field: architecture"
        seen_architecture=true
        ABI_ARCHITECTURE="${value}"
        ;;
      libc)
        [[ "${seen_libc}" == false ]] ||
          Die "duplicate ABI manifest field: libc"
        seen_libc=true
        ABI_LIBC="${value}"
        ;;
      mode)
        [[ "${seen_mode}" == false ]] ||
          Die "duplicate ABI manifest field: mode"
        seen_mode=true
        ABI_MODE="${value}"
        ;;
      interpreter)
        [[ "${seen_interpreter}" == false ]] ||
          Die "duplicate ABI manifest field: interpreter"
        seen_interpreter=true
        ABI_INTERPRETER="${value}"
        ;;
      android_tools_libc)
        [[ "${seen_android_tools_libc}" == false ]] ||
          Die "duplicate ABI manifest field: android_tools_libc"
        seen_android_tools_libc=true
        ABI_ANDROID_TOOLS_LIBC="${value}"
        ;;
      '') ;;
      *) Die "unknown ABI manifest field: ${key}" ;;
    esac
  done <"${ABI_MANIFEST}"

  [[ "${ABI_SCHEMA}" == 2 ]] || Die "unsupported ABI manifest schema"
  [[ "${ABI_ARCHITECTURE}" == x86_64 ]] ||
    Die "unsupported bundle architecture: ${ABI_ARCHITECTURE:-missing}"
  [[ "${ABI_LIBC}" == glibc || "${ABI_LIBC}" == musl ]] ||
    Die "unsupported bundle libc ABI: ${ABI_LIBC:-missing}"
  [[ "${ABI_MODE}" == standalone || "${ABI_MODE}" == thin ]] ||
    Die "unsupported bundle packaging mode: ${ABI_MODE:-missing}"
  [[ -n "${ABI_INTERPRETER}" ]] || Die "ABI manifest has no ELF interpreter"
  [[ "${ABI_ANDROID_TOOLS_LIBC}" == java ||
     "${ABI_ANDROID_TOOLS_LIBC}" == glibc ]] ||
    Die "unsupported Android build-tools ABI: ${ABI_ANDROID_TOOLS_LIBC:-missing}"
}

CheckBundleChecksums() {
  [[ -f "${CHECKSUM_MANIFEST}" && ! -L "${CHECKSUM_MANIFEST}" ]] ||
    Die "missing regular checksum manifest: ${CHECKSUM_MANIFEST}"
  command -v sha256sum >/dev/null 2>&1 ||
    Die "sha256sum is required to verify the portable bundle"
  (cd -- "${BUNDLE_ROOT}" &&
    sha256sum --quiet -c "mocktail/metadata/SHA256SUMS.txt") ||
    Die "portable bundle checksum verification failed"
}

CheckDependencyManifest() {
  [[ -f "${DEPENDENCY_MANIFEST}" && ! -L "${DEPENDENCY_MANIFEST}" ]] ||
    Die "missing regular dependency manifest: ${DEPENDENCY_MANIFEST}"
  [[ -d "${RUNTIME_ROOT}/lib" && ! -L "${RUNTIME_ROOT}/lib" ]] ||
    Die "missing regular dependency directory: ${RUNTIME_ROOT}/lib"
  local dependency_mode
  dependency_mode="$(sed -n 's/^mode=//p' "${DEPENDENCY_MANIFEST}")"
  [[ -n "${dependency_mode}" && "${dependency_mode}" != *$'\n'* ]] ||
    Die "dependency manifest has an invalid packaging mode"
  [[ "${dependency_mode}" == "${ABI_MODE}" ]] ||
    Die "ABI mode ${ABI_MODE} does not match DEPENDENCIES.txt mode ${dependency_mode}"

  if [[ "${ABI_MODE}" == thin ]] &&
      find "${RUNTIME_ROOT}/lib" -mindepth 1 -maxdepth 1 -print -quit \
        2>/dev/null | grep -q .; then
    Die "thin bundle unexpectedly contains bundled dependency libraries"
  fi
}

ValidateRelativeRuntimePath() {
  local value="$1"
  [[ -z "${value}" || ( "${value}" != /* && "${value}" != .. &&
     "${value}" != ../* && "${value}" != */../* &&
     "${value}" != */.. && "${value}" != *$'\n'* &&
     "${value}" != *$'\r'* ) ]] ||
    Die "standalone environment contains an unsafe relative path"
}

ConfigureStandaloneEnvironment() {
  [[ "${ABI_MODE}" == standalone ]] || return 0
  local environment_file="${RUNTIME_ROOT}/webkit.env"
  [[ -f "${environment_file}" && ! -L "${environment_file}" ]] ||
    Die "missing standalone WebKit environment"

  local key value
  local webkit6_exec="" webkit6_bundle=""
  local gst_plugins="" gst_scanner="" gio_modules="" gio_tls=""
  local pixbuf_cache="" glycin_data="" schemas=""
  local data_dirs="" fontconfig=""
  while IFS='=' read -r key value; do
    case "${key}" in
      \#*|'') continue ;;
      MOCKTAIL_WEBKITGTK6_EXEC_DIR_REL) webkit6_exec="${value}" ;;
      MOCKTAIL_WEBKITGTK6_INJECTED_BUNDLE_REL) webkit6_bundle="${value}" ;;
      MOCKTAIL_WEBKITGTK6_NAMESPACE_DIR)
        WEBKITGTK6_NAMESPACE_DIR="${value}"
        ;;
      GST_PLUGIN_PATH_1_0_REL) gst_plugins="${value}" ;;
      GST_PLUGIN_SYSTEM_PATH_1_0) export GST_PLUGIN_SYSTEM_PATH_1_0="${value}" ;;
      GST_PLUGIN_SCANNER_REL) gst_scanner="${value}" ;;
      GIO_EXTRA_MODULES_REL) gio_modules="${value}" ;;
      GIO_USE_TLS) gio_tls="${value}" ;;
      GDK_PIXBUF_MODULE_FILE_REL) pixbuf_cache="${value}" ;;
      GLYCIN_DATA_DIR_REL) glycin_data="${value}" ;;
      GSETTINGS_SCHEMA_DIR_REL) schemas="${value}" ;;
      XDG_DATA_DIRS_REL) data_dirs="${value}" ;;
      FONTCONFIG_FILE_REL) fontconfig="${value}" ;;
      *) Die "unknown standalone WebKit environment field: ${key}" ;;
    esac
  done <"${environment_file}"

  local relative
  for relative in "${webkit6_exec}" "${webkit6_bundle}" \
      "${gst_plugins}" "${gst_scanner}" \
      "${gio_modules}" "${pixbuf_cache}" "${glycin_data}" \
      "${schemas}" "${data_dirs}" "${fontconfig}"; do
    ValidateRelativeRuntimePath "${relative}"
  done
  [[ -n "${webkit6_exec}" && -n "${webkit6_bundle}" ]] ||
    Die "standalone WebKit environment is incomplete"
  [[ "${WEBKITGTK6_NAMESPACE_DIR}" =~ ^/usr/(lib|lib64|libexec|lib/x86_64-linux-gnu)/webkitgtk-6\.0$ ]] ||
    Die "standalone WebKitGTK 6 namespace path is invalid"
  export JAVA_HOME="${SUPPORT_ROOT}/jre"
  export SSL_CERT_FILE="${SUPPORT_ROOT}/share/ca-certificates/ca-bundle.crt"
  if [[ -n "${ANYLINUX_BIN_DIR}" ]]; then
    export WEBKIT_EXEC_PATH="${ANYLINUX_BIN_DIR}"
    export GST_PLUGIN_SCANNER="${ANYLINUX_BIN_DIR}/gst-plugin-scanner"
  else
    export WEBKIT_EXEC_PATH="${RUNTIME_ROOT}/${webkit6_exec}"
    export GST_PLUGIN_SCANNER="${RUNTIME_ROOT}/${gst_scanner}"
  fi
  export WEBKIT_INJECTED_BUNDLE_PATH="$(dirname -- \
    "${RUNTIME_ROOT}/${webkit6_bundle}")"
  export GST_PLUGIN_PATH_1_0="${RUNTIME_ROOT}/${gst_plugins}"
  export GIO_EXTRA_MODULES="${RUNTIME_ROOT}/${gio_modules}"
  export GIO_USE_TLS="${gio_tls}"
  export GDK_PIXBUF_MODULE_FILE="${RUNTIME_ROOT}/${pixbuf_cache}"
  [[ -z "${glycin_data}" ]] ||
    export GLYCIN_DATA_DIR="${RUNTIME_ROOT}/${glycin_data}"
  export GSETTINGS_SCHEMA_DIR="${RUNTIME_ROOT}/${schemas}"
  export XDG_DATA_DIRS="${RUNTIME_ROOT}/${data_dirs}${XDG_DATA_DIRS:+:${XDG_DATA_DIRS}}"
  [[ -z "${fontconfig}" ]] ||
    export FONTCONFIG_FILE="${RUNTIME_ROOT}/${fontconfig}"
}

EnterStandaloneNamespace() {
  [[ "${ABI_MODE}" == standalone &&
     -z "${ANYLINUX_BIN_DIR}" &&
     "${MOCKTAIL_STANDALONE_NAMESPACE:-0}" != 1 &&
     "${MOCKTAIL_SKIP_NAMESPACE_CHECK:-0}" != 1 ]] || return 0
  local namespace_usr="${RUNTIME_ROOT}/namespace/usr"
  local bubblewrap="${SUPPORT_BIN}/bwrap"
  [[ -d "${namespace_usr}" && ! -L "${namespace_usr}" ]] ||
    Die "standalone /usr overlay is unavailable"
  [[ -x "${bubblewrap}" && -x "${SUPPORT_BIN}/xdg-dbus-proxy" ]] ||
    Die "standalone WebKit namespace tools are unavailable"

  local -a command=(
    "${bubblewrap}"
    --die-with-parent
    --ro-bind / /
    --dev-bind /dev /dev
    --proc /proc
    --overlay-src /usr
    --overlay-src "${namespace_usr}"
    --ro-overlay /usr
    --bind /tmp /tmp
  )
  if [[ -d /var/tmp ]]; then
    command+=(--bind /var/tmp /var/tmp)
  fi
  if [[ -n "${HOME:-}" && -d "${HOME}" ]]; then
    command+=(--bind "${HOME}" "${HOME}")
  fi
  if [[ -n "${XDG_RUNTIME_DIR:-}" && -d "${XDG_RUNTIME_DIR}" ]]; then
    command+=(--bind "${XDG_RUNTIME_DIR}" "${XDG_RUNTIME_DIR}")
  fi
  command+=(
    --setenv MOCKTAIL_STANDALONE_NAMESPACE 1
    --chdir "${RUNTIME_ROOT}"
    "${SUPPORT_BIN}/bash"
    "${LAUNCHER_DIR}/portable_launcher.sh"
  )
  exec "${command[@]}" "$@"
  Die "cannot enter the standalone WebKit namespace"
}

ReadElfInterpreter() {
  LC_ALL=C readelf -l "$1" 2>/dev/null |
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p'
}

DetectElfLibc() {
  local path="$1"
  local interpreter needed interpreter_libc="" needed_libc=""
  interpreter="$(ReadElfInterpreter "${path}")"
  case "${interpreter}" in
    *ld-musl-*.so.1) interpreter_libc=musl ;;
    *ld-linux*.so.*) interpreter_libc=glibc ;;
  esac
  needed="$(LC_ALL=C readelf -d "${path}" 2>/dev/null |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')"
  if grep -Eq '^(libc\.so\.6|ld-linux[^[:space:]]*\.so(\.[0-9]+)*)$' \
      <<<"${needed}"; then
    needed_libc=glibc
  fi
  if grep -Eq '^(libc\.so|ld-musl-[^[:space:]]*\.so\.1)$' \
      <<<"${needed}"; then
    if [[ -n "${needed_libc}" && "${needed_libc}" != musl ]]; then
      printf mixed
      return 0
    fi
    needed_libc=musl
  fi
  if [[ -n "${interpreter_libc}" && -n "${needed_libc}" &&
        "${interpreter_libc}" != "${needed_libc}" ]]; then
    printf mixed
  elif [[ -n "${interpreter_libc}" ]]; then
    printf '%s' "${interpreter_libc}"
  elif [[ -n "${needed_libc}" ]]; then
    printf '%s' "${needed_libc}"
  else
    printf unknown
  fi
}

DetectHostLibc() {
  local version shell_interpreter
  version="$(LC_ALL=C getconf GNU_LIBC_VERSION 2>/dev/null || true)"
  if [[ "${version}" == glibc\ * ]]; then
    printf glibc
    return 0
  fi
  version="$(LC_ALL=C ldd --version 2>&1 || true)"
  if grep -qi 'musl' <<<"${version}"; then
    printf musl
    return 0
  fi
  shell_interpreter="$(ReadElfInterpreter "/proc/$$/exe")"
  case "${shell_interpreter}" in
    *ld-musl-*.so.1) printf musl ;;
    *ld-linux*.so.*) printf glibc ;;
    *) printf unknown ;;
  esac
}

ValidateBundledElfAbi() {
  local path="$1"
  local label="$2"
  local expected="${3:-${ABI_LIBC}}"
  local detected
  detected="$(DetectElfLibc "${path}")"
  [[ "${detected}" == "${expected}" ]] ||
    Die "${label} targets ${detected}, but ABI.txt requires ${expected}"
}

CheckBundleAbi() {
  command -v readelf >/dev/null 2>&1 ||
    Die "readelf is required to verify the portable ABI contract"
  CheckBundleChecksums
  LoadAbiManifest
  CheckDependencyManifest

  [[ "$(uname -m)" == "${ABI_ARCHITECTURE}" ]] ||
    Die "bundle requires ${ABI_ARCHITECTURE}, host is $(uname -m)"
  local main_interpreter host_libc
  main_interpreter="$(ReadElfInterpreter "${BUNDLED_BIN_DIR}/mocktail")"
  [[ "${main_interpreter}" == "${ABI_INTERPRETER}" ]] ||
    Die "mocktail ELF interpreter does not match ABI.txt (expected ${ABI_INTERPRETER}, found ${main_interpreter:-none})"
  ValidateBundledElfAbi "${BUNDLED_BIN_DIR}/mocktail" "mocktail"
  ValidateBundledElfAbi "${BUNDLED_BIN_DIR}/mocktail_updater" \
    "Mocktail updater"
  ValidateBundledElfAbi "${BUNDLED_BIN_DIR}/mocktail_webview_helper" \
    "WebView helper"
  [[ -x "${ANDROID_BUILD_TOOLS}/aapt" ]] ||
    Die "Android APK metadata tool is unavailable"
  if [[ "${ABI_ANDROID_TOOLS_LIBC}" == java ]]; then
    [[ -x "${ANDROID_BUILD_TOOLS}/apkanalyzer" ]] ||
      Die "Android Java APK analyzer is unavailable"
  else
    ValidateBundledElfAbi "${ANDROID_BUILD_TOOLS}/aapt" \
      "Android aapt" glibc
  fi

  if [[ -z "${ANYLINUX_BIN_DIR}" ]]; then
    host_libc="$(DetectHostLibc)"
    [[ "${host_libc}" != unknown ]] ||
      Die "cannot determine host libc ABI; install working getconf or ldd"
    [[ "${host_libc}" == "${ABI_LIBC}" ]] ||
      Die "bundle targets ${ABI_LIBC}, but host libc is ${host_libc}; use a Mocktail x86-64 ${host_libc} AppImage"
  fi
}

CheckCommand() {
  local name="$1"
  if command -v "${name}" >/dev/null 2>&1; then
    return 0
  fi
  printf '  missing command: %s\n' "${name}" >&2
  return 1
}

CheckElfDependencies() {
  local executable="$1"
  local label="$2"
  local missing output
  if ! output="$(LC_ALL=C ldd "${executable}" 2>&1)"; then
    printf '  cannot inspect %s libraries:\n%s\n' "${label}" \
      "${output}" >&2
    return 1
  fi
  missing="$(sed -n \
    's/^[[:space:]]*\([^[:space:]]\+\)[[:space:]]*=>[[:space:]]*not found.*$/\1/p' \
    <<<"${output}")"
  if [[ -z "${missing}" ]]; then
    return 0
  fi
  printf '  missing %s libraries:\n%s\n' "${label}" "${missing}" >&2
  return 1
}

CheckSystem() {
  local status=0

  if [[ "${ABI_MODE}" == thin ]]; then
    CheckElfDependencies "${MAIN_BINARY}" runtime || status=1
    CheckElfDependencies "${FAILURE_DIALOG_HELPER}" libadwaita/GTK || status=1
    CheckElfDependencies "${WEBVIEW_HELPER}" WebKit/GTK || status=1
  fi

  local command_name
  for command_name in bash java jq unzip aapt apksigner file flock \
      timeout readelf sha256sum; do
    CheckCommand "${command_name}" || status=1
  done
  if ! "${ANDROID_TOOL_EXEC_DIR}/aapt" version >/dev/null 2>&1; then
    printf '  bundled Android APK analyzer cannot execute\n' >&2
    status=1
  fi
  if [[ "${ABI_ANDROID_TOOLS_LIBC}" == java ]] &&
      ! "${ANDROID_TOOL_EXEC_DIR}/apkanalyzer" --help >/dev/null 2>&1; then
      printf '  bundled Android apkanalyzer cannot execute with Java\n' >&2
      status=1
  fi
  if ! "${ANDROID_TOOL_EXEC_DIR}/apksigner" version >/dev/null 2>&1; then
    printf '  bundled Android apksigner cannot execute with host Java\n' >&2
    status=1
  fi

  local webview_dependencies icd_manifest
  if [[ "${ABI_MODE}" == thin ]]; then
    webview_dependencies="$(LC_ALL=C ldd "${WEBVIEW_HELPER}" 2>/dev/null || true)"
    if ! grep -q 'libvulkan\.so\.1[[:space:]]*=>' \
        <<<"${webview_dependencies}"; then
      printf '  missing host Vulkan loader: libvulkan.so.1\n' >&2
      status=1
    fi
  fi
  icd_manifest="$(find /usr/share/vulkan/icd.d /etc/vulkan/icd.d \
    -maxdepth 1 -type f -name '*.json' -print -quit 2>/dev/null || true)"
  if [[ -z "${VK_ICD_FILENAMES:-}" && -z "${icd_manifest}" ]]; then
    printf '  no Vulkan ICD manifest found; install the GPU vendor driver\n' >&2
    status=1
  fi

  if (( status != 0 )); then
    if [[ "${ABI_LIBC}" == glibc ]]; then
      printf '\nArch example:\n' >&2
      printf '  sudo pacman -S --needed vulkan-icd-loader libadwaita webkitgtk-6.0 jq unzip file binutils jre-openjdk-headless\n' >&2
      printf '\nVoid x86_64-glibc example:\n' >&2
    else
      printf '\nVoid x86_64-musl example:\n' >&2
    fi
    printf '  sudo xbps-install -S vulkan-loader libadwaita libwebkitgtk60 jq unzip file binutils openjdk17-jre\n' >&2
    printf 'Install the Vulkan ICD matching the GPU (for example vulkan-radeon, nvidia-utils, or vulkan-intel).\n' >&2
    return "${status}"
  fi

  printf 'Mocktail portable host check passed.\n'
  printf 'Packaging mode: %s.\n' "${ABI_MODE}"
  printf 'GPU driver and Vulkan ICD remain host-provided by design.\n'
}

[[ -d "${RUNTIME_ROOT}" && ! -L "${RUNTIME_ROOT}" ]] ||
  Die "missing regular runtime directory: ${RUNTIME_ROOT}"
[[ -x "${MAIN_BINARY}" ]] || Die "missing executable: ${MAIN_BINARY}"
[[ -x "${UPDATE_HELPER}" ]] || Die "missing executable: ${UPDATE_HELPER}"
[[ -x "${FAILURE_DIALOG_HELPER}" ]] ||
  Die "missing executable: ${FAILURE_DIALOG_HELPER}"
[[ -x "${WEBVIEW_HELPER}" ]] || Die "missing WebView helper: ${WEBVIEW_HELPER}"
[[ -x "${ANDROID_BUILD_TOOLS}/aapt" &&
   -x "${ANDROID_BUILD_TOOLS}/apksigner" ]] ||
  Die "missing bundled Android validation tools"
[[ -r "${METADATA_DIR}/roblox_compatibility.json" ]] ||
  Die "missing compatibility manifest"
[[ -r "${METADATA_DIR}/roblox_host_abi_reference.json" ]] ||
  Die "missing native HostAbi reference profile"

CheckBundleAbi
ConfigureStandaloneEnvironment
EnterStandaloneNamespace "$@"

if [[ "${1:-}" == --check-system ]]; then
  CheckSystem
  exit $?
fi

data_root="${MOCKTAIL_DATA_ROOT:-${XDG_DATA_HOME:-${HOME}/.local/share}/mocktail}"
information_only=false
case "${1:-}" in
  --help|-h) information_only=true ;;
esac
if [[ "${information_only}" == false && -z "${ROBLOX_LIB_PATH:-}" &&
      ! -f "${data_root}/current.json" &&
      "${MOCKTAIL_SKIP_HOST_CHECK:-0}" != 1 ]]; then
  printf 'Mocktail first-run host check...\n'
  CheckSystem || Die "host requirements are incomplete"
fi

export MOCKTAIL_PROJECT_ROOT="${RUNTIME_ROOT}"
export MOCKTAIL_COMPATIBILITY_MANIFEST="${METADATA_DIR}/roblox_compatibility.json"
export MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${METADATA_DIR}/roblox_compatibility.json"
export MOCKTAIL_UPDATE_SIGNING_TRUST_PATH="${METADATA_DIR}/roblox_signing_certificates.json"
export MOCKTAIL_UPDATE_HOST_ABI_REFERENCE="${METADATA_DIR}/roblox_host_abi_reference.json"
export MOCKTAIL_BOOTSTRAP_SOURCES_PATH="${METADATA_DIR}/roblox_bootstrap_sources.json"
export MOCKTAIL_UPDATE_HELPER="${UPDATE_HELPER}"
export MOCKTAIL_UPDATE_CANARY_BIN="${MAIN_BINARY}"
export MOCKTAIL_BIN="${MAIN_BINARY}"
export MOCKTAIL_PORTABLE_MODE="${ABI_MODE}"

cd -- "${RUNTIME_ROOT}"
exec "${MAIN_BINARY}" "$@"
