#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

set -Eeuo pipefail
umask 022

RUNTIME_ROOT=""
TARGET_LIBC=""
SOURCE_ROOT="/"
DRY_RUN=false
STAGING=""
COMMITTED=false
declare -a PUBLISHED_PATHS=()

WEBKITGTK6_EXEC_SOURCE=""
WEBKITGTK6_BUNDLE_SOURCE=""
WEBKITGTK6_NAMESPACE_DIR=""
GSTREAMER_PLUGIN_SOURCE=""
GSTREAMER_SCANNER_SOURCE=""
GDK_PIXBUF_SOURCE=""
GIO_MODULE_SOURCE=""
SCHEMA_SOURCE=""
MIME_SOURCE=""
GLYCIN_BINARY_SOURCE=""
GLYCIN_DATA_SOURCE=""
SOURCE_LAYOUT=""
GDK_PIXBUF_BACKEND=""

Usage() {
  cat <<'EOF'
Usage: scripts/package_standalone_webkit.sh --runtime-root DIR --libc ABI [OPTIONS]

Copy the native WebKitGTK 6 process resources needed by a standalone Mocktail
runtime. Shared-library closure and ELF path patching are handled by the parent
packager.

Required options:
  --runtime-root DIR  Existing Mocktail runtime directory to populate.
  --libc ABI          Native source ABI: glibc or musl.

Optional:
  --source-root DIR   Inspect a mounted native root instead of / (test/audit).
  --dry-run           Stage and validate everything, but publish nothing.
  -h, --help          Show this help.
EOF
}

Log() {
  printf '[standalone-webkit] %s\n' "$*" >&2
}

Die() {
  Log "error: $*"
  exit 1
}

Cleanup() {
  local status=$?
  trap - EXIT
  if (( status != 0 )) && [[ "${COMMITTED}" == false ]]; then
    local path
    for path in "${PUBLISHED_PATHS[@]}"; do
      rm -rf -- "${path}"
    done
  fi
  [[ -z "${STAGING}" ]] || rm -rf -- "${STAGING}"
  exit "${status}"
}

ParseArguments() {
  while (( $# > 0 )); do
    case "$1" in
      --runtime-root)
        (( $# >= 2 )) || Die "--runtime-root requires a directory"
        RUNTIME_ROOT="$2"
        shift 2
        ;;
      --libc)
        (( $# >= 2 )) || Die "--libc requires glibc or musl"
        TARGET_LIBC="$2"
        shift 2
        ;;
      --source-root)
        (( $# >= 2 )) || Die "--source-root requires a directory"
        SOURCE_ROOT="$2"
        shift 2
        ;;
      --dry-run)
        DRY_RUN=true
        shift
        ;;
      -h|--help)
        Usage
        exit 0
        ;;
      *)
        Die "unknown option: $1"
        ;;
    esac
  done

  [[ -n "${RUNTIME_ROOT}" ]] || Die "--runtime-root is required"
  [[ "${TARGET_LIBC}" == glibc || "${TARGET_LIBC}" == musl ]] ||
    Die "--libc must be glibc or musl"
  [[ -d "${RUNTIME_ROOT}" && ! -L "${RUNTIME_ROOT}" ]] ||
    Die "runtime root must be an existing non-symlink directory"
  [[ -d "${SOURCE_ROOT}" && ! -L "${SOURCE_ROOT}" ]] ||
    Die "source root must be an existing non-symlink directory"
  RUNTIME_ROOT="$(cd -P -- "${RUNTIME_ROOT}" && pwd)"
  SOURCE_ROOT="$(cd -P -- "${SOURCE_ROOT}" && pwd)"
}

RequireCommands() {
  local command_name
  for command_name in awk cat cp dirname find grep install mkdir mktemp mv \
      readelf readlink rm sed sort wc; do
    command -v "${command_name}" >/dev/null 2>&1 ||
      Die "missing required command: ${command_name}"
  done
}

SourcePath() {
  local suffix="$1"
  if [[ "${SOURCE_ROOT}" == / ]]; then
    printf '/%s\n' "${suffix#/}"
  else
    printf '%s/%s\n' "${SOURCE_ROOT}" "${suffix#/}"
  fi
}

FindDirectoryContaining() {
  local required_name="$1"
  shift
  local suffix candidate
  for suffix in "$@"; do
    candidate="$(SourcePath "${suffix}")"
    if [[ -d "${candidate}" && ! -L "${candidate}" &&
          -f "${candidate}/${required_name}" &&
          ! -L "${candidate}/${required_name}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

FindDirectory() {
  local suffix candidate
  for suffix in "$@"; do
    candidate="$(SourcePath "${suffix}")"
    if [[ -d "${candidate}" && ! -L "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

FindRegularFile() {
  local suffix candidate
  for suffix in "$@"; do
    candidate="$(SourcePath "${suffix}")"
    if [[ -f "${candidate}" && ! -L "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

ValidateNamespaceDirectory() {
  local namespace_directory="$1"
  local family="$2"
  case "${family}:${namespace_directory}" in
    webkitgtk6:/usr/lib/webkitgtk-6.0|\
webkitgtk6:/usr/lib/x86_64-linux-gnu/webkitgtk-6.0|\
webkitgtk6:/usr/lib64/webkitgtk-6.0|\
webkitgtk6:/usr/libexec/webkitgtk-6.0)
      ;;
    *)
      Die "unsupported ${family} namespace directory: ${namespace_directory}"
      ;;
  esac
}

NamespaceDirectoryForSource() {
  local source="$1"
  local family="$2"
  local namespace_directory
  if [[ "${SOURCE_ROOT}" == / ]]; then
    namespace_directory="${source}"
  else
    case "${source}" in
      "${SOURCE_ROOT}"/*)
        namespace_directory="${source#"${SOURCE_ROOT}"}"
        ;;
      *)
        Die "${family} process directory escaped the source root: ${source}"
        ;;
    esac
  fi
  ValidateNamespaceDirectory "${namespace_directory}" "${family}"
  printf '%s\n' "${namespace_directory}"
}

DiscoverSources() {
  WEBKITGTK6_EXEC_SOURCE="$(FindDirectoryContaining WebKitWebProcess \
    usr/lib/webkitgtk-6.0 \
    usr/lib/x86_64-linux-gnu/webkitgtk-6.0 \
    usr/lib64/webkitgtk-6.0 \
    usr/libexec/webkitgtk-6.0)" ||
    Die "cannot find native WebKitGTK 6 process directory"
  WEBKITGTK6_NAMESPACE_DIR="$(NamespaceDirectoryForSource \
    "${WEBKITGTK6_EXEC_SOURCE}" webkitgtk6)"
  WEBKITGTK6_BUNDLE_SOURCE="$(FindDirectoryContaining \
    libwebkitgtkinjectedbundle.so usr/lib/webkitgtk-6.0/injected-bundle \
    usr/lib/x86_64-linux-gnu/webkitgtk-6.0/injected-bundle \
    usr/lib64/webkitgtk-6.0/injected-bundle)" ||
    Die "cannot find WebKitGTK 6 injected bundle"
  GSTREAMER_PLUGIN_SOURCE="$(FindDirectory \
    usr/lib/gstreamer-1.0 \
    usr/lib/x86_64-linux-gnu/gstreamer-1.0 \
    usr/lib64/gstreamer-1.0)" ||
    Die "cannot find the native GStreamer plugin directory"
  GSTREAMER_SCANNER_SOURCE="$(FindRegularFile \
    usr/lib/gstreamer-1.0/gst-plugin-scanner \
    usr/lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner \
    usr/libexec/gstreamer-1.0/gst-plugin-scanner \
    usr/lib64/gstreamer-1.0/gst-plugin-scanner)" ||
    Die "cannot find the native GStreamer plugin scanner"
  GIO_MODULE_SOURCE="$(FindDirectory usr/lib/gio/modules \
    usr/lib/x86_64-linux-gnu/gio/modules \
    usr/lib64/gio/modules)" || Die "cannot find native GIO modules"
  SCHEMA_SOURCE="$(FindDirectory usr/share/glib-2.0/schemas)" ||
    Die "cannot find GLib schemas"
  MIME_SOURCE="$(FindDirectory usr/share/mime)" ||
    Die "cannot find the shared MIME database"

  GDK_PIXBUF_SOURCE="$(FindDirectory \
    usr/lib/gdk-pixbuf-2.0/2.10.0 \
    usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0 \
    usr/lib64/gdk-pixbuf-2.0/2.10.0 || true)"
  GLYCIN_BINARY_SOURCE="$(FindDirectory \
    usr/lib/glycin-loaders/2+ usr/lib64/glycin-loaders/2+ \
    usr/libexec/glycin-loaders/2+ || true)"
  GLYCIN_DATA_SOURCE="$(FindDirectory \
    usr/share/glycin-loaders/2+/conf.d || true)"
  if [[ -n "${GDK_PIXBUF_SOURCE}" &&
        -d "${GDK_PIXBUF_SOURCE}/loaders" ]]; then
    GDK_PIXBUF_BACKEND=classic
  elif [[ -n "${GLYCIN_BINARY_SOURCE}" &&
          -n "${GLYCIN_DATA_SOURCE}" ]]; then
    GDK_PIXBUF_BACKEND=glycin
  else
    Die "cannot find classic GdkPixbuf modules or glycin loaders"
  fi

  case "${WEBKITGTK6_EXEC_SOURCE}" in
    */usr/lib/x86_64-linux-gnu/webkitgtk-6.0) SOURCE_LAYOUT=debian-multiarch ;;
    */usr/lib/webkitgtk-6.0) SOURCE_LAYOUT=arch-lib ;;
    */usr/libexec/webkitgtk-6.0) SOURCE_LAYOUT=void-libexec ;;
    *) SOURCE_LAYOUT=custom ;;
  esac
}

ReadElfInterpreter() {
  LC_ALL=C readelf -l "$1" 2>/dev/null |
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p'
}

DetectElfLibc() {
  local path="$1"
  local interpreter needed version_info
  interpreter="$(ReadElfInterpreter "${path}")"
  case "${interpreter}" in
    *ld-musl-*.so.1) printf musl; return 0 ;;
    *ld-linux*.so.*) printf glibc; return 0 ;;
  esac
  needed="$(LC_ALL=C readelf -d "${path}" 2>/dev/null |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')"
  if grep -Fxq libc.so.6 <<<"${needed}"; then
    printf glibc
    return 0
  fi
  if grep -Fxq libc.so <<<"${needed}"; then
    printf musl
    return 0
  fi
  version_info="$(LC_ALL=C readelf --version-info "${path}" 2>/dev/null || true)"
  if grep -Eq 'GLIBC_[0-9]' <<<"${version_info}"; then
    printf glibc
  else
    printf unknown
  fi
}

ValidateElf() {
  local path="$1"
  local label="$2"
  local allow_unknown="${3:-false}"
  local machine detected
  [[ -f "${path}" && ! -L "${path}" ]] ||
    Die "${label} is not a regular file: ${path}"
  machine="$(LC_ALL=C readelf -h "${path}" 2>/dev/null |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
  [[ "${machine}" == *X86-64* ]] || Die "${label} is not x86-64 ELF"
  detected="$(DetectElfLibc "${path}")"
  if [[ "${allow_unknown}" == true && "${detected}" == unknown ]]; then
    return 0
  fi
  [[ "${detected}" == "${TARGET_LIBC}" ]] ||
    Die "${label} targets ${detected}, expected ${TARGET_LIBC}: ${path}"
}

ValidateSourceTree() {
  local directory="$1"
  local label="$2"
  [[ -d "${directory}" && ! -L "${directory}" ]] ||
    Die "${label} is not a regular directory"
  local canonical target link
  canonical="$(readlink -f -- "${directory}")"
  while IFS= read -r -d '' link; do
    target="$(readlink -f -- "${link}" 2>/dev/null || true)"
    [[ -n "${target}" &&
       ( "${target}" == "${canonical}" ||
         "${target}" == "${canonical}/"* ) ]] ||
      Die "${label} contains an escaping or broken symlink: ${link}"
  done < <(find -P "${directory}" -type l -print0)
  if find -P "${directory}" -mindepth 1 \
      ! -type d ! -type f ! -type l -print -quit | grep -q .; then
    Die "${label} contains a special filesystem entry"
  fi
}

ValidateSources() {
  local process_name
  for process_name in WebKitGPUProcess WebKitNetworkProcess WebKitWebProcess; do
    ValidateElf "${WEBKITGTK6_EXEC_SOURCE}/${process_name}" \
      "WebKitGTK 6 ${process_name}"
  done
  ValidateElf "${WEBKITGTK6_BUNDLE_SOURCE}/libwebkitgtkinjectedbundle.so" \
    "WebKitGTK 6 injected bundle"
  ValidateElf "${GSTREAMER_SCANNER_SOURCE}" "GStreamer plugin scanner"

  [[ -f "${SCHEMA_SOURCE}/gschemas.compiled" ]] ||
    Die "GLib schema directory has no gschemas.compiled"
  [[ -f "${MIME_SOURCE}/mime.cache" ]] ||
    Die "shared MIME database has no mime.cache"
  [[ -f "${GIO_MODULE_SOURCE}/libgiognutls.so" ]] ||
    Die "GIO TLS module libgiognutls.so is unavailable"

  local plugin_count gio_count
  plugin_count="$(find -P "${GSTREAMER_PLUGIN_SOURCE}" -maxdepth 1 \
    -type f -name '*.so' -printf . | wc -c)"
  gio_count="$(find -P "${GIO_MODULE_SOURCE}" -maxdepth 1 \
    -type f -name '*.so' -printf . | wc -c)"
  (( plugin_count > 0 )) || Die "GStreamer plugin directory is empty"
  (( gio_count > 0 )) || Die "GIO module directory is empty"
}

CopyTree() {
  local source="$1"
  local destination="$2"
  local label="$3"
  ValidateSourceTree "${source}" "${label}"
  mkdir -p -- "${destination}"
  cp -a -- "${source}/." "${destination}/"
}

CopyFlatElfSet() {
  local source="$1"
  local destination="$2"
  local label="$3"
  mkdir -p -- "${destination}"
  local path count=0
  while IFS= read -r -d '' path; do
    ValidateElf "${path}" "${label}" true
    install -m 0755 -- "${path}" "${destination}/${path##*/}"
    ((count += 1))
  done < <(find -P "${source}" -maxdepth 1 -type f -name '*.so' \
    -print0 | sort -z)
  (( count > 0 )) || Die "${label} set is empty"
}

CopyWebKitProcesses() {
  local source="$1"
  local destination="$2"
  local label="$3"
  mkdir -p -- "${destination}"
  local process_name
  for process_name in WebKitGPUProcess WebKitNetworkProcess WebKitWebProcess; do
    install -m 0755 -- "${source}/${process_name}" \
      "${destination}/${process_name}"
  done
  Log "copied ${label} processes from ${source}"
}

WriteClassicPixbufCache() {
  local source_cache="${GDK_PIXBUF_SOURCE}/loaders.cache"
  local destination_cache=
  destination_cache="${STAGING}/lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders.cache"
  [[ -f "${source_cache}" && ! -L "${source_cache}" ]] ||
    Die "classic GdkPixbuf loader cache is unavailable"
  sed -E \
    -e 's@^# LoaderDir = .*$@# LoaderDir = lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders@' \
    -e 's#^"[^"]*/(libpixbufloader[-_][^/"]+\.so)"$#"lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders/\1"#' \
    "${source_cache}" >"${destination_cache}"
  if grep -Eq '^"/(usr|lib|lib64|opt|home|hdd|tmp)/' \
      "${destination_cache}"; then
    Die "GdkPixbuf cache still contains an absolute loader path"
  fi
}

CopyGdkPixbufResources() {
  local destination="${STAGING}/lib/plugins/gdk-pixbuf-2.0/2.10.0"
  mkdir -p -- "${destination}"
  if [[ "${GDK_PIXBUF_BACKEND}" == classic ]]; then
    CopyFlatElfSet "${GDK_PIXBUF_SOURCE}/loaders" \
      "${destination}/loaders" "GdkPixbuf loader"
    WriteClassicPixbufCache
    return
  fi

  printf '%s\n' \
    '# GdkPixbuf Image Loader Modules file' \
    '# No classic modules: this native build uses glycin loaders.' \
    >"${destination}/loaders.cache"
  mkdir -p -- "${STAGING}/lib/plugins/glycin-loaders/2+" \
    "${STAGING}/share/glycin-loaders/2+/conf.d"
  local executable config
  while IFS= read -r -d '' executable; do
    # Arch ships the HEIF loader even when its optional libheif dependency is
    # absent. Mocktail's WebKit UI does not require HEIF/AVIF decoding.
    [[ "${executable##*/}" != glycin-heif ]] || continue
    ValidateElf "${executable}" "glycin image loader"
    install -m 0755 -- "${executable}" \
      "${STAGING}/lib/plugins/glycin-loaders/2+/${executable##*/}"
  done < <(find -P "${GLYCIN_BINARY_SOURCE}" -maxdepth 1 -type f \
    -perm -0100 -print0 | sort -z)
  while IFS= read -r -d '' config; do
    [[ "${config##*/}" != glycin-heif.conf ]] || continue
    sed -E \
      's#^([[:space:]]*Exec=).*/([^/[:space:]]+)[[:space:]]*$#\1lib/plugins/glycin-loaders/2+/\2#' \
      "${config}" \
      >"${STAGING}/share/glycin-loaders/2+/conf.d/${config##*/}"
  done < <(find -P "${GLYCIN_DATA_SOURCE}" -maxdepth 1 -type f \
    -name '*.conf' -print0 | sort -z)
  if grep -REq '^[[:space:]]*Exec=/' \
      "${STAGING}/share/glycin-loaders/2+/conf.d"; then
    Die "glycin configuration still contains an absolute executable path"
  fi
}

CopyGtkData() {
  CopyTree "${SCHEMA_SOURCE}" "${STAGING}/share/glib-2.0/schemas" \
    "GLib schemas"
  CopyTree "${MIME_SOURCE}" "${STAGING}/share/mime" \
    "shared MIME database"

  local suffix source destination
  for suffix in icons/Adwaita icons/AdwaitaLegacy themes/Default \
      fonts/Adwaita; do
    source="$(SourcePath "usr/share/${suffix}")"
    [[ -d "${source}" && ! -L "${source}" ]] || continue
    destination="${STAGING}/share/${suffix}"
    CopyTree "${source}" "${destination}" "GTK data ${suffix}"
  done

  source="$(SourcePath usr/share/icons/hicolor/index.theme)"
  if [[ -f "${source}" && ! -L "${source}" ]]; then
    mkdir -p -- "${STAGING}/share/icons/hicolor"
    install -m 0644 -- "${source}" \
      "${STAGING}/share/icons/hicolor/index.theme"
  fi

  if [[ -d "${STAGING}/share/fonts" ]]; then
    mkdir -p -- "${STAGING}/share/fontconfig"
    cat >"${STAGING}/share/fontconfig/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>share/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
</fontconfig>
EOF
  fi
}

WriteEnvironmentFile() {
  local glycin_rel="" fontconfig_rel=""
  if [[ "${GDK_PIXBUF_BACKEND}" == glycin ]]; then
    glycin_rel=share/glycin-loaders/2+
  fi
  if [[ -f "${STAGING}/share/fontconfig/fonts.conf" ]]; then
    fontconfig_rel=share/fontconfig/fonts.conf
  fi
  cat >"${STAGING}/webkit.env" <<EOF
# Relative to the packaged Mocktail runtime root. The launcher must prefix
# path-valued *_REL entries with the resolved runtime root before exporting
# them. Namespace directories are fixed paths used inside the WebKit sandbox.
MOCKTAIL_WEBKITGTK6_NAMESPACE_DIR=${WEBKITGTK6_NAMESPACE_DIR}
MOCKTAIL_WEBKITGTK6_EXEC_DIR_REL=libexec/webkitgtk-6.0
MOCKTAIL_WEBKITGTK6_INJECTED_BUNDLE_REL=lib/webkitgtk-6.0/injected-bundle/libwebkitgtkinjectedbundle.so
GST_PLUGIN_PATH_1_0_REL=lib/plugins/gstreamer-1.0
GST_PLUGIN_SYSTEM_PATH_1_0=
GST_PLUGIN_SCANNER_REL=libexec/gstreamer-1.0/gst-plugin-scanner
GIO_EXTRA_MODULES_REL=lib/plugins/gio/modules
GIO_USE_TLS=gnutls
GDK_PIXBUF_MODULE_FILE_REL=lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders.cache
GLYCIN_DATA_DIR_REL=${glycin_rel}
GSETTINGS_SCHEMA_DIR_REL=share/glib-2.0/schemas
XDG_DATA_DIRS_REL=share
FONTCONFIG_FILE_REL=${fontconfig_rel}
EOF
}

VerifyEnvironmentFile() {
  local line key value
  local saw_webkitgtk6_namespace=false
  while IFS= read -r line || [[ -n "${line}" ]]; do
    [[ -n "${line}" && "${line}" != \#* ]] || continue
    [[ "${line}" == *=* ]] || Die "malformed webkit.env entry: ${line}"
    key="${line%%=*}"
    value="${line#*=}"
    case "${key}" in
      MOCKTAIL_WEBKITGTK6_NAMESPACE_DIR)
        [[ "${value}" == "${WEBKITGTK6_NAMESPACE_DIR}" ]] ||
          Die "WebKitGTK 6 namespace directory changed during staging"
        ValidateNamespaceDirectory "${value}" webkitgtk6
        saw_webkitgtk6_namespace=true
        ;;
      *_REL)
        [[ "${value}" != /* && "${value}" != .. &&
           "${value}" != ../* && "${value}" != */../* &&
           "${value}" != */.. ]] ||
          Die "webkit.env ${key} is not a relocatable path"
        ;;
      *)
        [[ "${value}" != /* ]] ||
          Die "webkit.env contains an unexpected absolute path: ${key}"
        ;;
    esac
  done <"${STAGING}/webkit.env"
  [[ "${saw_webkitgtk6_namespace}" == true ]] ||
    Die "webkit.env has no WebKitGTK 6 namespace directory"
}

StageResources() {
  mkdir -p -- "${STAGING}/lib/plugins/gstreamer-1.0" \
    "${STAGING}/lib/plugins/gio/modules" \
    "${STAGING}/lib/webkitgtk-6.0/injected-bundle" \
    "${STAGING}/libexec/gstreamer-1.0"

  CopyWebKitProcesses "${WEBKITGTK6_EXEC_SOURCE}" \
    "${STAGING}/libexec/webkitgtk-6.0" "WebKitGTK 6"
  install -m 0755 -- \
    "${WEBKITGTK6_BUNDLE_SOURCE}/libwebkitgtkinjectedbundle.so" \
    "${STAGING}/lib/webkitgtk-6.0/injected-bundle/"
  CopyFlatElfSet "${GSTREAMER_PLUGIN_SOURCE}" \
    "${STAGING}/lib/plugins/gstreamer-1.0" "GStreamer plugin"
  install -m 0755 -- "${GSTREAMER_SCANNER_SOURCE}" \
    "${STAGING}/libexec/gstreamer-1.0/gst-plugin-scanner"
  CopyFlatElfSet "${GIO_MODULE_SOURCE}" \
    "${STAGING}/lib/plugins/gio/modules" "GIO module"
  if [[ -f "${GIO_MODULE_SOURCE}/giomodule.cache" ]]; then
    install -m 0644 -- "${GIO_MODULE_SOURCE}/giomodule.cache" \
      "${STAGING}/lib/plugins/gio/modules/giomodule.cache"
  fi

  CopyGdkPixbufResources
  CopyGtkData
  WriteEnvironmentFile
}

VerifyStaging() {
  local required
  local -a required_paths=(
    libexec/webkitgtk-6.0/WebKitGPUProcess
    libexec/webkitgtk-6.0/WebKitNetworkProcess
    libexec/webkitgtk-6.0/WebKitWebProcess
    libexec/gstreamer-1.0/gst-plugin-scanner
    lib/webkitgtk-6.0/injected-bundle/libwebkitgtkinjectedbundle.so
    lib/plugins/gio/modules/libgiognutls.so
    lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders.cache
    share/glib-2.0/schemas/gschemas.compiled
    share/mime/mime.cache
    webkit.env
  )
  for required in "${required_paths[@]}"; do
    [[ -f "${STAGING}/${required}" && ! -L "${STAGING}/${required}" ]] ||
      Die "staged WebKit resource is missing: ${required}"
  done
  if find -P "${STAGING}" -type f \
      \( -name '*_icd*.json' -o -name 'libvulkan_*.so*' \
         -o -name 'libGLX_*.so*' -o -name 'libEGL_*.so*' \) \
      -print -quit | grep -q .; then
    Die "staging unexpectedly contains a GPU driver or Vulkan ICD"
  fi
  VerifyEnvironmentFile

  local file_count byte_count plugin_count
  file_count="$(find -P "${STAGING}" -type f -printf . | wc -c)"
  byte_count="$(find -P "${STAGING}" -type f -printf '%s\n' |
    awk '{total += $1} END {print total + 0}')"
  plugin_count="$(find -P "${STAGING}/lib/plugins/gstreamer-1.0" \
    -type f -name '*.so' -printf . | wc -c)"
  printf 'source_layout=%s\n' "${SOURCE_LAYOUT}"
  printf 'libc=%s\n' "${TARGET_LIBC}"
  printf 'gdk_pixbuf_backend=%s\n' "${GDK_PIXBUF_BACKEND}"
  printf 'gstreamer_plugins=%s\n' "${plugin_count}"
  printf 'files=%s\n' "${file_count}"
  printf 'bytes=%s\n' "${byte_count}"
}

PublishPath() {
  local source="$1"
  local destination="$2"
  [[ ! -e "${destination}" && ! -L "${destination}" ]] ||
    Die "refusing to replace existing runtime resource: ${destination}"
  mv -- "${source}" "${destination}"
  PUBLISHED_PATHS+=("${destination}")
}

PublishResources() {
  mkdir -p -- "${RUNTIME_ROOT}/lib"
  local name
  for name in plugins webkitgtk-6.0; do
    PublishPath "${STAGING}/lib/${name}" "${RUNTIME_ROOT}/lib/${name}"
  done
  PublishPath "${STAGING}/libexec" "${RUNTIME_ROOT}/libexec"
  PublishPath "${STAGING}/share" "${RUNTIME_ROOT}/share"
  PublishPath "${STAGING}/webkit.env" "${RUNTIME_ROOT}/webkit.env"
  COMMITTED=true
  Log "WebKit standalone resources published under ${RUNTIME_ROOT}"
}

Main() {
  ParseArguments "$@"
  RequireCommands
  DiscoverSources
  ValidateSources

  local staging_parent
  if [[ "${DRY_RUN}" == true ]]; then
    staging_parent="${TMPDIR:-/tmp}"
  else
    staging_parent="$(dirname -- "${RUNTIME_ROOT}")"
  fi
  STAGING="$(mktemp -d "${staging_parent}/.mocktail-webkit.XXXXXX")"
  trap Cleanup EXIT
  StageResources
  VerifyStaging
  if [[ "${DRY_RUN}" == true ]]; then
    Log "dry-run complete; runtime root was not changed"
    return 0
  fi
  PublishResources
}

Main "$@"
