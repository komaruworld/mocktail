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

SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -P -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
OUTPUT=""
OUTPUT_EXPLICIT=false
MODE=standalone
TARGET_LIBC=glibc
ARTIFACT_NAME=""
TARGET_INTERPRETER=""
PACKAGING_HOST_LIBC=unknown
DRY_RUN=false
APPIMAGE_OUTPUT=""
ANDROID_BUILD_TOOLS_ROOT="${MOCKTAIL_ANDROID_BUILD_TOOLS_ROOT:-}"
ANDROID_APKANALYZER_ROOT="${MOCKTAIL_ANDROID_APKANALYZER_ROOT:-}"
STAGING=""
APPDIR=""
PACKAGING_LIBRARY_PATH=""
ANDROID_TOOLS_ABI=""
APPIMAGE_FORMAT="${MOCKTAIL_APPIMAGE_FORMAT:-classic}"
ANYLINUX_PACKAGER="${MOCKTAIL_ANYLINUX_PACKAGER:-quick-sharun}"
ANYLINUX_APPIMAGETOOL="${MOCKTAIL_ANYLINUX_APPIMAGETOOL:-appimagetool}"
ANYLINUX_SHARUN="${MOCKTAIL_ANYLINUX_SHARUN:-}"
ANYLINUX_C_SOURCE="${MOCKTAIL_ANYLINUX_C_SOURCE:-}"

readonly -a PROJECT_ARTIFACTS=(
  mocktail
  mocktail_updater
  mocktail_failure_dialog
  mocktail_webview_helper
  libmocktail_audio_sdl.so
  libc.so
  libdl.so
  libm.so
  libz.so
  libandroid.so
  liblog.so
  libmediandk.so
  libOpenSLES.so
  libOpenMAXAL.so
  libEGL.so
  libGLESv2.so
  libvulkan.so
)
readonly FREEBSD_SOCKET_HELPER="mocktail_freebsd_socket_helper"

readonly -a RUNTIME_SCRIPTS=(
  collect_support_bundle.sh
)

readonly STANDALONE_SUPPORT_PACKAGER="${PROJECT_ROOT}/scripts/package_standalone_support.sh"
readonly STANDALONE_WEBKIT_PACKAGER="${PROJECT_ROOT}/scripts/package_standalone_webkit.sh"

declare -A COPIED_SOURCES=()
declare -A HOST_REQUIREMENTS=()
declare -a DEPENDENCY_QUEUE=()

Usage() {
  cat <<'EOF'
Usage: scripts/package_portable.sh [OPTIONS]

Create a relocatable x86-64 Linux bundle with a single ./run.sh launcher.

Options:
  --build-dir DIR      Release build directory (default: build).
  --output DIR         Bundle output directory (default includes libc ABI).
  --libc ABI           Target libc ABI: glibc (default) or musl.
  --mode MODE          standalone (default) or thin; aliases: full, static,
                       dynamic, and legacy minimal.
  --dry-run            Audit artifacts and dependency policy without writing.
  --appimage FILE      Also build FILE. Set MOCKTAIL_APPIMAGE_FORMAT to
                       classic (default) or anylinux.
  -h, --help           Show this help.

standalone bundles the application userspace and support tools. thin omits
copied dependency libraries and bootstraps host packages before launch.
The physical GPU kernel driver and vendor ICD remain host-provided.

Every Mocktail ELF build artifact must match --libc. glibc bundles use compact
native aapt plus the Java apksigner library. A musl standalone bundle uses the
Java apkanalyzer backend instead of hiding a glibc runtime dependency.
No Android SDK development tree is published. Public release artifacts are:
Mocktail-x86_64-<libc>-<standalone|thin>.AppImage.
EOF
}

Log() {
  printf '[portable] %s\n' "$*" >&2
}

Die() {
  Log "error: $*"
  exit 1
}

Cleanup() {
  [[ -z "${STAGING}" ]] || rm -rf -- "${STAGING}"
  [[ -z "${APPDIR}" ]] || rm -rf -- "${APPDIR}"
}

ParseArguments() {
  while (( $# > 0 )); do
    case "$1" in
      --build-dir)
        (( $# >= 2 )) || Die "--build-dir requires a directory"
        BUILD_DIR="$2"
        shift 2
        ;;
      --output)
        (( $# >= 2 )) || Die "--output requires a directory"
        OUTPUT="$2"
        OUTPUT_EXPLICIT=true
        shift 2
        ;;
      --libc)
        (( $# >= 2 )) || Die "--libc requires glibc or musl"
        TARGET_LIBC="$2"
        shift 2
        ;;
      --mode)
        (( $# >= 2 )) || Die "--mode requires standalone or thin"
        MODE="$2"
        shift 2
        ;;
      --dry-run)
        DRY_RUN=true
        shift
        ;;
      --appimage)
        (( $# >= 2 )) || Die "--appimage requires an output file"
        APPIMAGE_OUTPUT="$2"
        shift 2
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
  case "${MODE}" in
    standalone|full|static) MODE=standalone ;;
    dynamic|minimal) MODE=thin ;;
  esac
  [[ "${MODE}" == standalone || "${MODE}" == thin ]] ||
    Die "--mode must be standalone or thin"
  [[ "${TARGET_LIBC}" == glibc || "${TARGET_LIBC}" == musl ]] ||
    Die "--libc must be glibc or musl"
  [[ "${APPIMAGE_FORMAT}" == classic ||
     "${APPIMAGE_FORMAT}" == anylinux ]] ||
    Die "MOCKTAIL_APPIMAGE_FORMAT must be classic or anylinux"
  if [[ "${APPIMAGE_FORMAT}" == anylinux && "${TARGET_LIBC}" != glibc ]]; then
    Die "AnyLinux packaging requires a glibc build"
  fi

  if [[ "${TARGET_LIBC}" == musl && "${MODE}" == standalone ]]; then
    ANDROID_TOOLS_ABI=java
  else
    ANDROID_TOOLS_ABI=glibc
  fi

  ARTIFACT_NAME="mocktail-linux-x86_64-${TARGET_LIBC}-${MODE}"
  if [[ "${OUTPUT_EXPLICIT}" == false ]]; then
    OUTPUT="${PROJECT_ROOT}/dist/${ARTIFACT_NAME}"
  fi
}

RequireCommand() {
  command -v "$1" >/dev/null 2>&1 || Die "missing required command: $1"
}

CanonicalDirectory() {
  local path="$1"
  [[ -d "${path}" ]] || Die "directory does not exist: ${path}"
  cd -P -- "${path}" && pwd
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
  shell_interpreter="$(ReadElfInterpreter /proc/$$/exe)"
  case "${shell_interpreter}" in
    *ld-musl-*.so.1) printf musl ;;
    *ld-linux*.so.*) printf glibc ;;
    *) printf unknown ;;
  esac
}

ValidateElfLibc() {
  local path="$1"
  local label="$2"
  local detected interpreter
  detected="$(DetectElfLibc "${path}")"
  interpreter="$(ReadElfInterpreter "${path}")"
  [[ "${detected}" == "${TARGET_LIBC}" ]] ||
    Die "${label} libc ABI mismatch: expected ${TARGET_LIBC}, detected ${detected} (interpreter=${interpreter:-none})"
}

ValidateArtifacts() {
  local artifact path architecture interpreter
  for artifact in "${PROJECT_ARTIFACTS[@]}"; do
    path="${BUILD_DIR}/${artifact}"
    [[ -f "${path}" && ! -L "${path}" ]] ||
      Die "missing regular build artifact: ${path}"
    architecture="$(LC_ALL=C readelf -h "${path}" 2>/dev/null |
      sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
    [[ "${architecture}" == *"X86-64"* ]] ||
      Die "build artifact is not x86-64 ELF: ${path}"
    ValidateElfLibc "${path}" "build artifact ${artifact}"
  done

  TARGET_INTERPRETER="$(ReadElfInterpreter "${BUILD_DIR}/mocktail")"
  [[ -n "${TARGET_INTERPRETER}" ]] ||
    Die "mocktail must be a dynamically linked ${TARGET_LIBC} executable"

  local freebsd_helper="${BUILD_DIR}/${FREEBSD_SOCKET_HELPER}"
  [[ -f "${freebsd_helper}" && ! -L "${freebsd_helper}" &&
     -x "${freebsd_helper}" ]] ||
    Die "missing executable FreeBSD socket helper: ${freebsd_helper}"
  LC_ALL=C readelf -h "${freebsd_helper}" 2>/dev/null |
    grep -Eq 'OS/ABI:[[:space:]]+UNIX - FreeBSD' ||
    Die "socket helper is not branded for FreeBSD"
  LC_ALL=C readelf -h "${freebsd_helper}" 2>/dev/null |
    grep -Eq 'Machine:[[:space:]]+.*X86-64' ||
    Die "socket helper is not an x86-64 ELF"
  [[ -z "$(ReadElfInterpreter "${freebsd_helper}")" ]] ||
    Die "FreeBSD socket helper must be statically linked"
  LC_ALL=C readelf -n "${freebsd_helper}" 2>/dev/null |
    grep -Fq FreeBSD || Die "socket helper has no FreeBSD ABI note"
}

ResolveAndroidBuildTools() {
  local apksigner_path analyzer_path discovered_root
  if [[ -n "${ANDROID_BUILD_TOOLS_ROOT}" ]]; then
    ANDROID_BUILD_TOOLS_ROOT="$(CanonicalDirectory \
      "${ANDROID_BUILD_TOOLS_ROOT}")"
  else
    apksigner_path="$(command -v apksigner)" ||
      Die "apksigner is required to package the standalone updater"
    apksigner_path="$(readlink -f -- "${apksigner_path}")"
    discovered_root="$(dirname -- "${apksigner_path}")"
    ANDROID_BUILD_TOOLS_ROOT="$(CanonicalDirectory "${discovered_root}")"
  fi

  local required
  for required in apksigner lib/apksigner.jar NOTICE.txt source.properties; do
    [[ -f "${ANDROID_BUILD_TOOLS_ROOT}/${required}" &&
       ! -L "${ANDROID_BUILD_TOOLS_ROOT}/${required}" ]] ||
      Die "Android build-tools artifact is unavailable: ${required}"
  done
  if [[ "${ANDROID_TOOLS_ABI}" == glibc ]]; then
    for required in aapt lib64/libc++.so; do
      [[ -f "${ANDROID_BUILD_TOOLS_ROOT}/${required}" &&
         ! -L "${ANDROID_BUILD_TOOLS_ROOT}/${required}" ]] ||
        Die "Android native metadata artifact is unavailable: ${required}"
    done
    return 0
  fi
  if [[ -n "${ANDROID_APKANALYZER_ROOT}" ]]; then
    ANDROID_APKANALYZER_ROOT="$(CanonicalDirectory \
      "${ANDROID_APKANALYZER_ROOT}")"
  else
    analyzer_path="$(command -v apkanalyzer)" ||
      Die "apkanalyzer is required to package portable APK metadata support"
    analyzer_path="$(readlink -f -- "${analyzer_path}")"
    ANDROID_APKANALYZER_ROOT="$(CanonicalDirectory \
      "$(dirname -- "$(dirname -- "${analyzer_path}")")")"
  fi
  [[ -f "${ANDROID_APKANALYZER_ROOT}/lib/apkanalyzer-classpath.jar" &&
     ! -L "${ANDROID_APKANALYZER_ROOT}/lib/apkanalyzer-classpath.jar" ]] ||
    Die "Android apkanalyzer classpath is unavailable"
  [[ -f "${ANDROID_APKANALYZER_ROOT}/NOTICE.txt" &&
     -f "${ANDROID_APKANALYZER_ROOT}/source.properties" ]] ||
    Die "Android apkanalyzer license or source metadata is unavailable"
}

ListElfDependencies() {
  local output status=0
  output="$(LC_ALL=C LD_LIBRARY_PATH="${PACKAGING_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    ldd "$1" 2>&1)" || status=$?
  if grep -Eq '=>[[:space:]]*not found|^Error loading shared library .*: No such file' \
      <<<"${output}"; then
    printf '%s\n' "${output}" >&2
    return 1
  fi
  if (( status != 0 )) && ! grep -q '^Error relocating ' <<<"${output}"; then
    printf '%s\n' "${output}" >&2
    return 1
  fi
  awk '
    /=>[[:space:]]*\// {
      for (field = 1; field <= NF; ++field) {
        if ($field == "=>") {
          print $(field + 1)
          break
        }
      }
      next
    }
    /^[[:space:]]*\// { print $1 }
  ' <<<"${output}"
}

ListElfNeededSonames() {
  LC_ALL=C readelf -d "$1" 2>/dev/null |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p'
}

HostDependencyReason() {
  local soname="$1"
  case "${soname}" in
    ld-musl-*.so.1)
      printf musl-runtime
      ;;
    libc.so)
      if [[ "${TARGET_LIBC}" == musl ]]; then
        printf musl-runtime
      else
        return 1
      fi
      ;;
    ld-linux*.so*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
    libresolv.so.*|libutil.so.*|libanl.so.*|libnss_*.so.*)
      printf glibc
      ;;
    libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libOpenGL.so.*|libEGL.so.*|\
    libGLESv1_CM.so.*|libGLESv2.so.*|libgbm.so.*|libdrm.so.*|libdrm_*.so.*)
      printf gpu-driver-stack
      ;;
    *)
      return 1
      ;;
  esac
}

RecordHostRequirement() {
  local soname="$1"
  local reason="$2"
  HOST_REQUIREMENTS["${soname}"]="${reason}"
}

IsProjectArtifact() {
  local candidate="$1"
  local artifact
  for artifact in "${PROJECT_ARTIFACTS[@]}"; do
    [[ "${candidate}" != "${artifact}" ]] || return 0
  done
  return 1
}

QueueDependencies() {
  local source="$1"
  local dependencies dependency soname canonical reason destination
  local existing_hash new_hash
  dependencies="$(ListElfDependencies "${source}")" ||
    Die "cannot resolve dependency closure for ${source}"
  while IFS= read -r dependency; do
    [[ -n "${dependency}" ]] || continue
    soname="${dependency##*/}"
    if reason="$(HostDependencyReason "${soname}")"; then
      RecordHostRequirement "${soname}" "${reason}"
      continue
    fi
    if IsProjectArtifact "${soname}"; then
      continue
    fi
    canonical="$(readlink -f -- "${dependency}")"
    [[ -f "${canonical}" ]] || Die "unresolved dependency: ${dependency}"
    if [[ "${MODE}" == thin ]]; then
      RecordHostRequirement "${soname}" thin-mode
      continue
    fi
    destination="${STAGING}/mocktail/lib/${soname}"
    if [[ -n "${COPIED_SOURCES[${soname}]+present}" ]]; then
      existing_hash="$(sha256sum "${COPIED_SOURCES[${soname}]}" |
        awk '{print $1}')"
      new_hash="$(sha256sum "${canonical}" | awk '{print $1}')"
      [[ "${existing_hash}" == "${new_hash}" ]] ||
        Die "dependency basename collision for ${soname}"
      continue
    fi
    COPIED_SOURCES["${soname}"]="${canonical}"
    DEPENDENCY_QUEUE+=("${canonical}")
    if [[ "${DRY_RUN}" == false ]]; then
      install -m 0755 -- "${canonical}" "${destination}"
    fi
  done <<<"${dependencies}"
}

AuditWebViewHostStack() {
  local dependencies dependency soname reason
  dependencies="$(ListElfDependencies \
    "${BUILD_DIR}/mocktail_webview_helper")" ||
    Die "cannot resolve host WebKit dependency closure"
  while IFS= read -r dependency; do
    [[ -n "${dependency}" ]] || continue
    soname="${dependency##*/}"
    reason="$(HostDependencyReason "${soname}" 2>/dev/null || printf webview-stack)"
    RecordHostRequirement "${soname}" "${reason}"
  done <<<"${dependencies}"
}

AuditThinDependencies() {
  local artifact soname reason
  for artifact in "${PROJECT_ARTIFACTS[@]}"; do
    while IFS= read -r soname; do
      [[ -n "${soname}" ]] || continue
      if IsProjectArtifact "${soname}"; then
        continue
      fi
      reason="$(HostDependencyReason "${soname}" 2>/dev/null ||
        printf thin-mode)"
      RecordHostRequirement "${soname}" "${reason}"
    done < <(ListElfNeededSonames "${BUILD_DIR}/${artifact}")
  done
}

CopyRuntimeTree() {
  local runtime_root="${STAGING}/mocktail"
  mkdir -p -- "${runtime_root}/bin" "${runtime_root}/lib" \
    "${runtime_root}/scripts" "${runtime_root}/metadata" \
    "${runtime_root}/runtime"
  chmod 0755 -- "${STAGING}"

  local artifact mode
  for artifact in "${PROJECT_ARTIFACTS[@]}"; do
    mode=0755
    install -m "${mode}" -- "${BUILD_DIR}/${artifact}" \
      "${runtime_root}/bin/${artifact}"
  done
  install -m 0755 -- "${BUILD_DIR}/${FREEBSD_SOCKET_HELPER}" \
    "${runtime_root}/bin/${FREEBSD_SOCKET_HELPER}"

  install -m 0755 -- "${PROJECT_ROOT}/packaging/run.sh" "${STAGING}/run.sh"
  install -m 0755 -- "${PROJECT_ROOT}/packaging/mocktail-launcher.sh" \
    "${runtime_root}/scripts/portable_launcher.sh"
  install -m 0755 -- "${PROJECT_ROOT}/scripts/install_thin_dependencies.sh" \
    "${runtime_root}/scripts/install_thin_dependencies.sh"
  install -m 0644 -- "${PROJECT_ROOT}/config/roblox_compatibility.json" \
    "${PROJECT_ROOT}/config/roblox_bootstrap_sources.json" \
    "${PROJECT_ROOT}/config/roblox_host_abi_reference.json" \
    "${PROJECT_ROOT}/config/roblox_signing_certificates.json" \
    "${runtime_root}/metadata/"

  local script
  for script in "${RUNTIME_SCRIPTS[@]}"; do
    install -m 0755 -- "${PROJECT_ROOT}/scripts/${script}" \
      "${runtime_root}/scripts/${script}"
  done
}

CopyApkAnalyzerClasspath() {
  local source_root="${ANDROID_APKANALYZER_ROOT}/lib"
  local destination_root="$1"
  local manifest_jar="${source_root}/apkanalyzer-classpath.jar"
  local class_path relative source destination
  class_path="$(unzip -p "${manifest_jar}" META-INF/MANIFEST.MF |
    tr -d '\r' | awk '
      /^Class-Path: / {
        collecting = 1
        value = substr($0, 13)
        next
      }
      collecting && /^ / {
        value = value substr($0, 2)
        next
      }
      collecting {
        print value
        emitted = 1
        exit
      }
      END {
        if (collecting && !emitted) print value
      }
    ')"
  [[ -n "${class_path}" ]] || Die "Android apkanalyzer classpath is empty"
  install -d -m 0755 -- "${destination_root}"
  install -m 0644 -- "${manifest_jar}" \
    "${destination_root}/apkanalyzer-classpath.jar"
  for relative in ${class_path}; do
    [[ "${relative}" != /* && "${relative}" != .. &&
       "${relative}" != ../* && "${relative}" != */../* &&
       "${relative}" != */.. ]] ||
      Die "unsafe Android analyzer classpath entry: ${relative}"
    source="${source_root}/${relative}"
    [[ -f "${source}" && ! -L "${source}" ]] ||
      Die "Android analyzer classpath entry is unavailable: ${relative}"
    destination="${destination_root}/${relative}"
    install -d -m 0755 -- "$(dirname -- "${destination}")"
    install -m 0644 -- "${source}" "${destination}"
  done
}

PackageAndroidRuntime() {
  local android_root="${STAGING}/mocktail/runtime/android-tools"
  install -d -m 0755 -- "${android_root}/bin" "${android_root}/lib" \
    "${android_root}/licenses"
  install -m 0755 -- "${ANDROID_BUILD_TOOLS_ROOT}/apksigner" \
    "${android_root}/bin/apksigner"
  install -m 0644 -- "${ANDROID_BUILD_TOOLS_ROOT}/lib/apksigner.jar" \
    "${android_root}/lib/apksigner.jar"
  ln -s -- ../lib "${android_root}/bin/lib"
  install -m 0644 -- "${ANDROID_BUILD_TOOLS_ROOT}/NOTICE.txt" \
    "${android_root}/licenses/build-tools.txt"

  if [[ "${ANDROID_TOOLS_ABI}" == glibc ]]; then
    install -d -m 0755 -- "${android_root}/bin/lib64"
    install -m 0755 -- "${ANDROID_BUILD_TOOLS_ROOT}/aapt" \
      "${android_root}/bin/aapt"
    install -m 0755 -- "${ANDROID_BUILD_TOOLS_ROOT}/lib64/libc++.so" \
      "${android_root}/bin/lib64/libc++.so"
    return 0
  fi

  install -d -m 0755 -- \
    "${android_root}/sdk/cmdline-tools/latest/lib" \
    "${android_root}/sdk/build-tools/37.0.0"
  install -m 0755 -- "${PROJECT_ROOT}/packaging/aapt-apkanalyzer.sh" \
    "${android_root}/bin/aapt"
  install -m 0755 -- "${PROJECT_ROOT}/packaging/apkanalyzer.sh" \
    "${android_root}/bin/apkanalyzer"
  CopyApkAnalyzerClasspath \
    "${android_root}/sdk/cmdline-tools/latest/lib"
  install -m 0755 -- "${PROJECT_ROOT}/packaging/aapt-apkanalyzer.sh" \
    "${android_root}/sdk/build-tools/37.0.0/aapt"
  install -m 0644 -- "${ANDROID_BUILD_TOOLS_ROOT}/source.properties" \
    "${android_root}/sdk/build-tools/37.0.0/source.properties"
  install -m 0644 -- "${ANDROID_APKANALYZER_ROOT}/source.properties" \
    "${android_root}/sdk/cmdline-tools/latest/source.properties"
  install -m 0644 -- "${ANDROID_APKANALYZER_ROOT}/NOTICE.txt" \
    "${android_root}/licenses/cmdline-tools.txt"
}

PackageStandaloneRuntime() {
  [[ "${MODE}" == standalone ]] || return 0
  rmdir -- "${STAGING}/mocktail/runtime"
  "${STANDALONE_SUPPORT_PACKAGER}" \
    --runtime-root "${STAGING}/mocktail" --libc "${TARGET_LIBC}"
  "${STANDALONE_WEBKIT_PACKAGER}" \
    --runtime-root "${STAGING}/mocktail" --libc "${TARGET_LIBC}"
  PACKAGING_LIBRARY_PATH="$(find "${STAGING}/mocktail" -type f \
    \( -perm -0100 -o -name '*.so' -o -name '*.so.*' \) -print0 |
    while IFS= read -r -d '' path; do
      IsElfFile "${path}" || continue
      dirname -- "${path}"
    done | LC_ALL=C sort -u | paste -sd: -)"
}

BuildStandaloneNamespace() {
  [[ "${MODE}" == standalone ]] || return 0
  local environment_file="${STAGING}/mocktail/webkit.env"
  local webkit6_namespace namespace_root
  webkit6_namespace="$(sed -n \
    's/^MOCKTAIL_WEBKITGTK6_NAMESPACE_DIR=//p' "${environment_file}")"
  [[ "${webkit6_namespace}" =~ ^/usr/(lib|lib64|libexec|lib/x86_64-linux-gnu)/webkitgtk-6\.0$ ]] ||
    Die "unsafe WebKitGTK 6 namespace directory: ${webkit6_namespace}"

  namespace_root="${STAGING}/mocktail/namespace"
  mkdir -p -- "${namespace_root}/usr/bin" \
    "${namespace_root}${webkit6_namespace}/injected-bundle"
  local process_name
  for process_name in WebKitGPUProcess WebKitNetworkProcess WebKitWebProcess; do
    ln -- "${STAGING}/mocktail/libexec/webkitgtk-6.0/${process_name}" \
      "${namespace_root}${webkit6_namespace}/${process_name}"
  done
  ln -- "${STAGING}/mocktail/lib/webkitgtk-6.0/injected-bundle/libwebkitgtkinjectedbundle.so" \
    "${namespace_root}${webkit6_namespace}/injected-bundle/libwebkitgtkinjectedbundle.so"
  ln -- "$(readlink -f -- "${STAGING}/mocktail/runtime/bin/bwrap")" \
    "${namespace_root}/usr/bin/bwrap"
  ln -- "$(readlink -f -- "${STAGING}/mocktail/runtime/bin/xdg-dbus-proxy")" \
    "${namespace_root}/usr/bin/xdg-dbus-proxy"
}

IsElfFile() {
  LC_ALL=C readelf -h "$1" 2>/dev/null |
    grep -Eq '^[[:space:]]*Type:[[:space:]]*(DYN|EXEC)'
}

QueuePackagedRuntimeDependencies() {
  local source
  while IFS= read -r -d '' source; do
    IsElfFile "${source}" || continue
    case "${source#${STAGING}/mocktail/}" in
      bin/${FREEBSD_SOCKET_HELPER}) continue ;;
      runtime/android-tools/*) continue ;;
    esac
    QueueDependencies "${source}"
  done < <(find "${STAGING}/mocktail/bin" \
    "${STAGING}/mocktail/runtime" "${STAGING}/mocktail/libexec" \
    "${STAGING}/mocktail/lib/plugins" \
    "${STAGING}/mocktail/lib/webkitgtk-6.0" \
    -type f \( -perm -0100 -o -name '*.so' -o -name '*.so.*' \) \
    -print0 2>/dev/null)
}

CollectDependencies() {
  local artifact source queue_index=0
  if [[ "${MODE}" == thin ]]; then
    AuditThinDependencies
    return 0
  fi
  if [[ "${DRY_RUN}" == true ]]; then
    for artifact in "${PROJECT_ARTIFACTS[@]}"; do
      QueueDependencies "${BUILD_DIR}/${artifact}"
    done
  else
    QueuePackagedRuntimeDependencies
  fi
  while (( queue_index < ${#DEPENDENCY_QUEUE[@]} )); do
    source="${DEPENDENCY_QUEUE[queue_index]}"
    ((queue_index += 1))
    QueueDependencies "${source}"
  done
}

WriteDependencyManifest() {
  {
    printf 'Mocktail portable dependency contract\n'
    printf 'mode=%s\n' "${MODE}"
    printf 'architecture=x86_64\n'
    printf 'libc=%s\n' "${TARGET_LIBC}"
    printf 'interpreter=%s\n' "${TARGET_INTERPRETER}"
    printf 'android_tools_libc=%s\n\n' "${ANDROID_TOOLS_ABI}"
    printf 'Bundled userspace libraries:\n'
    if (( ${#COPIED_SOURCES[@]} == 0 )); then
      if [[ "${MODE}" == thin ]]; then
        printf '  (none; thin mode)\n'
      else
        printf '  (none required by the standalone closure)\n'
      fi
    else
      local soname
      while IFS= read -r soname; do
        printf '  %s <- %s\n' "${soname}" "${COPIED_SOURCES[${soname}]}"
      done < <(printf '%s\n' "${!COPIED_SOURCES[@]}" | sort)
    fi
    printf '\nHost-provided libraries:\n'
    if (( ${#HOST_REQUIREMENTS[@]} == 0 )); then
      printf '  (none)\n'
    else
      local soname
      while IFS= read -r soname; do
        printf '  %s (%s)\n' "${soname}" "${HOST_REQUIREMENTS[${soname}]}"
      done < <(printf '%s\n' "${!HOST_REQUIREMENTS[@]}" | sort)
    fi
    printf '\nThe Linux kernel and x86-64 %s ABI remain host-provided.\n' \
      "${TARGET_LIBC}"
    printf 'Only the physical GPU driver, Vulkan ICD, and GL/display driver stack are host-provided.\n'
  } >"${STAGING}/mocktail/metadata/DEPENDENCIES.txt"
}

WriteAbiManifest() {
  {
    printf 'schema=2\n'
    printf 'architecture=x86_64\n'
    printf 'libc=%s\n' "${TARGET_LIBC}"
    printf 'mode=%s\n' "${MODE}"
    printf 'interpreter=%s\n' "${TARGET_INTERPRETER}"
    printf 'android_tools_libc=%s\n' "${ANDROID_TOOLS_ABI}"
  } >"${STAGING}/mocktail/metadata/ABI.txt"
}

PatchRuntimePaths() {
  local artifact
  if [[ "${MODE}" == thin ]]; then
    patchelf --set-rpath '$ORIGIN' "${STAGING}/mocktail/bin/mocktail"
    patchelf --remove-rpath \
      "${STAGING}/mocktail/bin/mocktail_webview_helper"
    for artifact in "${PROJECT_ARTIFACTS[@]}"; do
      case "${artifact}" in
        mocktail|mocktail_webview_helper) continue ;;
      esac
      patchelf --set-rpath '$ORIGIN' \
        "${STAGING}/mocktail/bin/${artifact}"
    done
    return 0
  fi

  local elf directory relative_to_lib central_path existing_path new_path
  while IFS= read -r -d '' elf; do
    IsElfFile "${elf}" || continue
    LC_ALL=C readelf -d "${elf}" 2>/dev/null |
      grep -q 'Dynamic section' || continue
    case "${elf#${STAGING}/mocktail/}" in
      runtime/android-tools/*) continue ;;
    esac
    directory="$(dirname -- "${elf}")"
    relative_to_lib="$(realpath --relative-to="${directory}" -- \
      "${STAGING}/mocktail/lib")"
    if [[ "${relative_to_lib}" == . ]]; then
      central_path='$ORIGIN'
    else
      central_path="\$ORIGIN/${relative_to_lib}"
    fi
    existing_path="$(patchelf --print-rpath "${elf}" 2>/dev/null || true)"
    new_path="\$ORIGIN:${central_path}"
    if [[ -n "${existing_path}" ]]; then
      new_path+=":${existing_path}"
    fi
    patchelf --set-rpath "${new_path}" "${elf}"
  done < <(find "${STAGING}/mocktail" -type f \
    \( -perm -0100 -o -name '*.so' -o -name '*.so.*' \) -print0)
  patchelf --set-rpath '$ORIGIN:$ORIGIN/../lib' \
    "${STAGING}/mocktail/bin/mocktail" \
    "${STAGING}/mocktail/bin/mocktail_failure_dialog" \
    "${STAGING}/mocktail/bin/mocktail_webview_helper"
}

WriteBundleChecksums() {
  touch "${STAGING}/mocktail/metadata/.mocktail-portable-bundle"
  (
    cd -- "${STAGING}"
    find . -type f \
      ! -path './mocktail/metadata/SHA256SUMS.txt' -print0 |
      LC_ALL=C sort -z |
      while IFS= read -r -d '' path; do
        sha256sum -- "${path}"
      done
  ) >"${STAGING}/mocktail/metadata/SHA256SUMS.txt"
}

VerifyBundle() {
  local elf dynamic_path dependencies packaged_interpreter packaged_libc
  local public_entries runtime_entries
  local expected_dynamic_path='$ORIGIN'
  if [[ "${MODE}" == standalone ]]; then
    expected_dynamic_path='$ORIGIN:$ORIGIN/../lib'
  fi
  public_entries="$(find "${STAGING}" -mindepth 1 -maxdepth 1 \
    -printf '%f\n' | LC_ALL=C sort)"
  [[ "${public_entries}" == $'mocktail\nrun.sh' ]] ||
    Die "portable root must contain only run.sh and mocktail/"
  [[ -x "${STAGING}/run.sh" && ! -L "${STAGING}/run.sh" &&
     -d "${STAGING}/mocktail" && ! -L "${STAGING}/mocktail" ]] ||
    Die "portable public layout contains an invalid launcher or runtime root"
  runtime_entries="$(find "${STAGING}/mocktail" -mindepth 1 -maxdepth 1 \
    -printf '%f\n' | LC_ALL=C sort)"
  if [[ "${MODE}" == standalone ]]; then
    [[ "${runtime_entries}" == \
       $'bin\nlib\nlibexec\nmetadata\nnamespace\nruntime\nscripts\nshare\nwebkit.env' ]] ||
      Die "standalone runtime root has an unexpected public entry: ${runtime_entries//$'\n'/,}"
  else
    [[ "${runtime_entries}" == \
       $'bin\nlib\nmetadata\nruntime\nscripts' ]] ||
      Die "thin runtime root has an unexpected public entry: ${runtime_entries//$'\n'/,}"
  fi
  while IFS= read -r -d '' elf; do
    if LC_ALL=C readelf -d "${elf}" 2>/dev/null |
        grep -F -- "${BUILD_DIR}" >/dev/null; then
      Die "absolute build path remains in packaged ELF: ${elf}"
    fi
  done < <(find "${STAGING}/mocktail" -type f \
    \( -perm -0100 -o -name '*.so' -o -name '*.so.*' \) -print0)

  dynamic_path="$(LC_ALL=C readelf -d "${STAGING}/mocktail/bin/mocktail" |
    sed -n 's/.*\(RPATH\|RUNPATH\).*\[\(.*\)\].*/\2/p')"
  [[ "${dynamic_path}" == "${expected_dynamic_path}" ]] ||
    Die "unexpected packaged mocktail RUNPATH: ${dynamic_path}"

  packaged_interpreter="$(ReadElfInterpreter \
    "${STAGING}/mocktail/bin/mocktail")"
  [[ "${packaged_interpreter}" == "${TARGET_INTERPRETER}" ]] ||
    Die "packaged mocktail interpreter changed unexpectedly"
  packaged_libc="$(DetectElfLibc "${STAGING}/mocktail/bin/mocktail")"
  [[ "${packaged_libc}" == "${TARGET_LIBC}" ]] ||
    Die "packaged mocktail libc ABI changed unexpectedly"
  grep -Fxq 'schema=2' "${STAGING}/mocktail/metadata/ABI.txt" ||
    Die "portable ABI manifest has an invalid schema"
  grep -Fxq 'architecture=x86_64' \
    "${STAGING}/mocktail/metadata/ABI.txt" ||
    Die "portable ABI manifest has an invalid architecture"
  grep -Fxq "libc=${TARGET_LIBC}" \
    "${STAGING}/mocktail/metadata/ABI.txt" ||
    Die "portable ABI manifest has an invalid libc"
  grep -Fxq "mode=${MODE}" "${STAGING}/mocktail/metadata/ABI.txt" ||
    Die "portable ABI manifest has an invalid packaging mode"
  grep -Fxq "interpreter=${TARGET_INTERPRETER}" \
    "${STAGING}/mocktail/metadata/ABI.txt" ||
    Die "portable ABI manifest has an invalid interpreter"
  grep -Fxq "android_tools_libc=${ANDROID_TOOLS_ABI}" \
    "${STAGING}/mocktail/metadata/ABI.txt" ||
    Die "portable ABI manifest has an invalid Android tools ABI"
  grep -Fxq "mode=${MODE}" \
    "${STAGING}/mocktail/metadata/DEPENDENCIES.txt" ||
    Die "portable dependency manifest has an invalid packaging mode"

  if [[ "${MODE}" == thin ]] &&
      find "${STAGING}/mocktail/lib" -mindepth 1 -maxdepth 1 \
        -print -quit |
        grep -q .; then
    Die "thin bundle unexpectedly contains userspace dependency libraries"
  fi

  local artifact
  for artifact in "${PROJECT_ARTIFACTS[@]}"; do
    if [[ "${MODE}" == standalone ]]; then
      dependencies="$(ListElfDependencies \
        "${STAGING}/mocktail/bin/${artifact}")" ||
        Die "packaged artifact has unresolved dependencies: ${artifact}"
    else
      ListElfNeededSonames \
        "${STAGING}/mocktail/bin/${artifact}" >/dev/null ||
        Die "cannot inspect packaged artifact dependencies: ${artifact}"
    fi
  done
  if [[ "${MODE}" == standalone ]]; then
    local support_elf
    for support_elf in \
        runtime/bin/bash runtime/jre/bin/java \
        runtime/bin/bwrap runtime/bin/xdg-dbus-proxy \
        libexec/webkitgtk-6.0/WebKitWebProcess \
        libexec/gstreamer-1.0/gst-plugin-scanner; do
      ListElfDependencies "${STAGING}/mocktail/${support_elf}" >/dev/null ||
        Die "standalone support ELF has unresolved dependencies: ${support_elf}"
    done
  fi
  local android_runtime="${STAGING}/mocktail/runtime/android-tools/bin"
  local verification_path="${android_runtime}:${PATH}"
  local verification_java_home="${JAVA_HOME:-}"
  if [[ "${MODE}" == standalone ]]; then
    verification_path="${STAGING}/mocktail/runtime/bin:${verification_path}"
    verification_java_home="${STAGING}/mocktail/runtime/jre"
  fi
  if [[ "${ANDROID_TOOLS_ABI}" == java ]]; then
    JAVA_HOME="${verification_java_home}" PATH="${verification_path}" \
      "${android_runtime}/apkanalyzer" --help >/dev/null 2>&1
  else
    "${android_runtime}/aapt" version >/dev/null
  fi
  JAVA_HOME="${verification_java_home}" PATH="${verification_path}" \
    "${android_runtime}/apksigner" \
    version >/dev/null
  (cd -- "${STAGING}" &&
    sha256sum --quiet -c mocktail/metadata/SHA256SUMS.txt) ||
    Die "packaged file checksum verification failed"
  if [[ "${PACKAGING_HOST_LIBC}" == "${TARGET_LIBC}" ]]; then
    MOCKTAIL_SKIP_HOST_CHECK=1 MOCKTAIL_SKIP_NAMESPACE_CHECK=1 \
      "${STAGING}/run.sh" --help >/dev/null
  else
    Log "skipping cross-libc launcher execution during thin packaging"
  fi
}

PublishBundle() {
  if [[ -e "${OUTPUT}" ]]; then
    [[ -d "${OUTPUT}" &&
       ( -f "${OUTPUT}/mocktail/metadata/.mocktail-portable-bundle" ||
         -f "${OUTPUT}/.mocktail-portable-bundle" ) ]] ||
      Die "refusing to replace unmarked output path: ${OUTPUT}"
    rm -rf -- "${OUTPUT}"
  fi
  mv -- "${STAGING}" "${OUTPUT}"
  STAGING=""
  Log "bundle ready: ${OUTPUT}/run.sh"
}

BuildAppImage() {
  local icon_path icon_relative_path
  [[ -n "${APPIMAGE_OUTPUT}" ]] || return 0
  APPDIR="$(mktemp -d "${OUTPUT%/*}/.Mocktail.AppDir.XXXXXX")"
  mkdir -p "${APPDIR}/usr/share/mocktail-bundle" \
    "${APPDIR}/usr/share/applications" \
    "${APPDIR}/usr/share/icons/hicolor/scalable/apps"
  cp -a -- "${OUTPUT}/." "${APPDIR}/usr/share/mocktail-bundle/"
  ln -s "share/mocktail-bundle/run.sh" "${APPDIR}/usr/mocktail"
  install -m 0755 -- "${PROJECT_ROOT}/packaging/AppRun" "${APPDIR}/AppRun"
  install -m 0644 -- \
    "${PROJECT_ROOT}/packaging/space.bigrat.mocktail.desktop" \
    "${APPDIR}/space.bigrat.mocktail.desktop"
  install -m 0644 -- \
    "${PROJECT_ROOT}/packaging/space.bigrat.mocktail.desktop" \
    "${APPDIR}/usr/share/applications/space.bigrat.mocktail.desktop"
  install -m 0644 -- \
    "${PROJECT_ROOT}/packaging/space.bigrat.mocktail.svg" \
    "${APPDIR}/space.bigrat.mocktail.svg"
  install -m 0644 -- \
    "${PROJECT_ROOT}/packaging/space.bigrat.mocktail.svg" \
    "${APPDIR}/usr/share/icons/hicolor/scalable/apps/space.bigrat.mocktail.svg"
  for icon_path in \
      "${PROJECT_ROOT}"/packaging/icons/hicolor/*x*/apps/space.bigrat.mocktail.png; do
    [[ -f "${icon_path}" ]] || Die "hicolor icon set is incomplete"
    icon_relative_path="${icon_path#"${PROJECT_ROOT}/packaging/icons/hicolor/"}"
    install -D -m 0644 -- "${icon_path}" \
      "${APPDIR}/usr/share/icons/hicolor/${icon_relative_path}"
  done
  ln -s space.bigrat.mocktail.svg "${APPDIR}/.DirIcon"
  mkdir -p -- "$(dirname -- "${APPIMAGE_OUTPUT}")"
  if [[ "${APPIMAGE_FORMAT}" == anylinux ]]; then
    BuildAnyLinuxAppImage
  else
    ARCH=x86_64 appimagetool "${APPDIR}" "${APPIMAGE_OUTPUT}"
  fi
  rm -rf -- "${APPDIR}"
  APPDIR=""
  Log "AppImage ready: ${APPIMAGE_OUTPUT}"
}

BuildAnyLinuxAppImage() {
  local apkanalyzer path
  local -a inputs=(
    "${APPDIR}/usr/share/mocktail-bundle/mocktail/bin/mocktail"
  )
  mv -- "${APPDIR}/AppRun" "${APPDIR}/AppRun.sh"
  while IFS= read -r -d '' path; do
    [[ "${path}" != "${inputs[0]}" ]] || continue
    [[ "${path##*/}" != "${FREEBSD_SOCKET_HELPER}" ]] || continue
    IsElfFile "${path}" || continue
    [[ -n "$(ReadElfInterpreter "${path}")" ]] || continue
    inputs+=("${path}")
  done < <(find "${APPDIR}/usr/share/mocktail-bundle" \
    \( -type f -o -type l \) -print0 |
    LC_ALL=C sort -z)

  if [[ -n "${ANYLINUX_SHARUN}" ]]; then
    install -m 0755 -- "${ANYLINUX_SHARUN}" "${APPDIR}/sharun"
  fi
  if [[ -n "${ANYLINUX_C_SOURCE}" ]]; then
    mkdir -p -- "${APPDIR}/lib"
    cc -shared -fPIC -O2 "${ANYLINUX_C_SOURCE}" \
      -o "${APPDIR}/lib/anylinux.so"
  fi

  Log "deploying AnyLinux runtime"
  APPDIR="${APPDIR}" MAIN_BIN=mocktail STRACE_MODE=0 NO_STRIP=1 \
    DEPLOY_DATADIR=0 DEPLOY_LOCALE=0 \
  "${ANYLINUX_PACKAGER}" "${inputs[@]}"

  WriteAnyLinuxScriptWrapper apksigner
  apkanalyzer="${APPDIR}/usr/share/mocktail-bundle/mocktail/runtime/android-tools/bin/apkanalyzer"
  if [[ -x "${apkanalyzer}" ]]; then
    WriteAnyLinuxScriptWrapper apkanalyzer
  fi
  RefreshAnyLinuxBundleChecksums

  APPDIR="${APPDIR}" APPIMAGE_ARCH=x86_64 \
    "${ANYLINUX_APPIMAGETOOL}" "${APPDIR}" \
      --output "$(dirname -- "${APPIMAGE_OUTPUT}")" \
      --name "$(basename -- "${APPIMAGE_OUTPUT}")"
}

WriteAnyLinuxScriptWrapper() {
  local name="$1"
  local wrapper="${APPDIR}/bin/${name}"
  rm -f -- "${wrapper}"
  cat >"${wrapper}" <<EOF
#!/bin/sh
exec "\${APPDIR}/bin/bash" \
  "\${APPDIR}/usr/share/mocktail-bundle/mocktail/runtime/android-tools/bin/${name}" "\$@"
EOF
  chmod 0755 -- "${wrapper}"
}

RefreshAnyLinuxBundleChecksums() {
  local bundle="${APPDIR}/usr/share/mocktail-bundle"
  (
    cd -- "${bundle}"
    find . -type f \
      ! -path './mocktail/metadata/SHA256SUMS.txt' -print0 |
      LC_ALL=C sort -z |
      while IFS= read -r -d '' path; do
        sha256sum -- "${path}"
      done
  ) >"${bundle}/mocktail/metadata/SHA256SUMS.txt"
}

DryRun() {
  STAGING="$(mktemp -d "${TMPDIR:-/tmp}/mocktail-portable-dry-run.XXXXXX")"
  CollectDependencies
  printf 'mode=%s\n' "${MODE}"
  printf 'libc=%s\n' "${TARGET_LIBC}"
  printf 'artifact_name=%s\n' "${ARTIFACT_NAME}"
  printf 'interpreter=%s\n' "${TARGET_INTERPRETER}"
  printf 'android_tools_libc=%s\n' "${ANDROID_TOOLS_ABI}"
  printf 'project_artifacts=%d\n' "${#PROJECT_ARTIFACTS[@]}"
  printf 'bundled_dependency_count=%d\n' "${#COPIED_SOURCES[@]}"
  printf 'host_dependency_count=%d\n' "${#HOST_REQUIREMENTS[@]}"
  if (( ${#HOST_REQUIREMENTS[@]} > 0 )); then
    local soname
    while IFS= read -r soname; do
      printf 'host=%s reason=%s\n' "${soname}" "${HOST_REQUIREMENTS[${soname}]}"
    done < <(printf '%s\n' "${!HOST_REQUIREMENTS[@]}" | sort)
  fi
}

Main() {
  ParseArguments "$@"
  RequireCommand readelf
  RequireCommand patchelf
  RequireCommand sha256sum
  RequireCommand paste
  RequireCommand unzip
  if [[ -n "${APPIMAGE_OUTPUT}" ]]; then
    if [[ "${APPIMAGE_FORMAT}" == anylinux ]]; then
      RequireCommand "${ANYLINUX_PACKAGER}"
      RequireCommand "${ANYLINUX_APPIMAGETOOL}"
      if [[ -n "${ANYLINUX_SHARUN}" ]]; then
        [[ -x "${ANYLINUX_SHARUN}" ]] ||
          Die "AnyLinux sharun is not executable: ${ANYLINUX_SHARUN}"
      fi
      if [[ -n "${ANYLINUX_C_SOURCE}" ]]; then
        [[ -r "${ANYLINUX_C_SOURCE}" ]] ||
          Die "AnyLinux C source is not readable: ${ANYLINUX_C_SOURCE}"
        RequireCommand cc
      fi
    else
      RequireCommand appimagetool
    fi
  fi
  BUILD_DIR="$(CanonicalDirectory "${BUILD_DIR}")"
  ValidateArtifacts
  PACKAGING_HOST_LIBC="$(DetectHostLibc)"
  [[ "${PACKAGING_HOST_LIBC}" != unknown ]] ||
    Die "cannot determine packaging host libc"
  if [[ "${MODE}" == standalone ]]; then
    RequireCommand ldd
    [[ "${PACKAGING_HOST_LIBC}" == "${TARGET_LIBC}" ]] ||
      Die "standalone mode requires a matching ${TARGET_LIBC} host; detected ${PACKAGING_HOST_LIBC}"
  fi
  ResolveAndroidBuildTools
  trap Cleanup EXIT

  if [[ "${DRY_RUN}" == true ]]; then
    DryRun
    return 0
  fi

  [[ ! -L "${OUTPUT}" ]] || Die "refusing symlink output path: ${OUTPUT}"
  OUTPUT="$(readlink -m -- "${OUTPUT}")"
  [[ "${OUTPUT}" != / && "${OUTPUT}" != "${PROJECT_ROOT}" ]] ||
    Die "unsafe output path: ${OUTPUT}"
  mkdir -p -- "$(dirname -- "${OUTPUT}")"
  STAGING="$(mktemp -d "${OUTPUT}.tmp.XXXXXX")"
  CopyRuntimeTree
  PackageStandaloneRuntime
  PackageAndroidRuntime
  CollectDependencies
  WriteDependencyManifest
  PatchRuntimePaths
  BuildStandaloneNamespace
  WriteAbiManifest
  WriteBundleChecksums
  VerifyBundle
  PublishBundle
  BuildAppImage
}

Main "$@"
