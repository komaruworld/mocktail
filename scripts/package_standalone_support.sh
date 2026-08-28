#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -Eeuo pipefail
umask 022

RUNTIME_ROOT=""
TARGET_LIBC=""
STAGING_ROOT=""
SUPPORT_RUNTIME=""

readonly -a REQUIRED_COMMANDS=(
  bash
  bwrap
  xdg-dbus-proxy
  jq
  unzip
  file
  flock
  timeout
  readelf
  sha256sum
  curl
  tar
  gzip
  find
  sed
  grep
  awk
  sort
  comm
  stat
  id
  mktemp
  dirname
  uname
)

Usage() {
  cat <<'EOF'
Usage: scripts/package_standalone_support.sh --runtime-root DIR --libc ABI

Populate DIR/runtime with the native command, Java, and CA support needed by
the standalone Mocktail launcher. ABI must be glibc or musl and must match the
build host. ELF dependency closure is intentionally left to the parent
packager.
EOF
}

Log() {
  printf '[standalone-support] %s\n' "$*" >&2
}

Die() {
  Log "error: $*"
  exit 1
}

Cleanup() {
  if [[ -n "${STAGING_ROOT}" && -d "${STAGING_ROOT}" &&
        ! -L "${STAGING_ROOT}" ]]; then
    rm -rf -- "${STAGING_ROOT}"
  fi
}

trap Cleanup EXIT

ParseArguments() {
  while (( $# > 0 )); do
    case "$1" in
      --runtime-root)
        (( $# >= 2 )) || Die "--runtime-root requires a directory"
        RUNTIME_ROOT="$2"
        shift 2
        ;;
      --runtime-root=*)
        RUNTIME_ROOT="${1#*=}"
        shift
        ;;
      --libc)
        (( $# >= 2 )) || Die "--libc requires glibc or musl"
        TARGET_LIBC="$2"
        shift 2
        ;;
      --libc=*)
        TARGET_LIBC="${1#*=}"
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
}

RequireBuildCommands() {
  local command_name
  for command_name in cp cmp install java jlink ldd ln mkdir mv \
      readlink realpath rm rmdir; do
    command -v "${command_name}" >/dev/null 2>&1 ||
      Die "required build command is unavailable: ${command_name}"
  done
}

RejectUnsafeText() {
  local -r value="$1"
  local -r description="$2"
  [[ -n "${value}" && "${value}" != *$'\n'* && "${value}" != *$'\r'* &&
     "${value}" != *$'\t'* ]] || Die "${description} is empty or unsafe"
}

ValidateRuntimeRoot() {
  RejectUnsafeText "${RUNTIME_ROOT}" "runtime root"
  [[ "${RUNTIME_ROOT}" == /* ]] || Die "runtime root must be absolute"
  [[ -d "${RUNTIME_ROOT}" && ! -L "${RUNTIME_ROOT}" ]] ||
    Die "runtime root must be an existing non-symlink directory"

  local canonical_root
  canonical_root="$(realpath -e -- "${RUNTIME_ROOT}")" ||
    Die "cannot canonicalize runtime root"
  canonical_root="${canonical_root%/}"
  [[ -n "${canonical_root}" && "${canonical_root}" != / ]] ||
    Die "refusing unsafe runtime root"
  [[ "${RUNTIME_ROOT%/}" == "${canonical_root}" ]] ||
    Die "runtime root must be canonical and contain no symlink components"
  RUNTIME_ROOT="${canonical_root}"

  local -r destination="${RUNTIME_ROOT}/runtime"
  if [[ -e "${destination}" || -L "${destination}" ]]; then
    [[ -d "${destination}" && ! -L "${destination}" ]] ||
      Die "runtime destination must not be a symlink or non-directory"
    if find "${destination}" -mindepth 1 -maxdepth 1 -print -quit |
        grep -q .; then
      Die "runtime destination is not empty: ${destination}"
    fi
  fi
}

DetectHostLibc() {
  local output=""
  output="$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
  if [[ "${output}" == glibc\ * ]]; then
    printf 'glibc\n'
    return
  fi
  output="$(ldd --version 2>&1 || true)"
  if grep -qi musl <<<"${output}"; then
    printf 'musl\n'
    return
  fi

  local interpreter=""
  interpreter="$(LC_ALL=C readelf -l /proc/$$/exe 2>/dev/null |
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')"
  case "${interpreter}" in
    *ld-linux*.so.*) printf 'glibc\n' ;;
    *ld-musl-*.so.1) printf 'musl\n' ;;
    *) printf 'unknown\n' ;;
  esac
}

ValidateNativeHost() {
  local host_libc
  host_libc="$(DetectHostLibc)"
  [[ "${host_libc}" != unknown ]] || Die "cannot determine host libc"
  [[ "${host_libc}" == "${TARGET_LIBC}" ]] ||
    Die "requested ${TARGET_LIBC} support on a ${host_libc} host; cross-packaging is unsupported"
}

PathIsInside() {
  local -r path="$1"
  local -r root="$2"
  [[ "${path}" == "${root}" || "${path}" == "${root}/"* ]]
}

InstallRegularFile() {
  local -r source="$1"
  local -r destination="$2"
  [[ -f "${source}" && ! -L "${source}" ]] ||
    Die "source is not a regular file: ${source}"
  mkdir -p -- "$(dirname -- "${destination}")"
  if [[ -e "${destination}" || -L "${destination}" ]]; then
    [[ -f "${destination}" && ! -L "${destination}" ]] ||
      Die "support file collision: ${destination}"
    cmp -s -- "${source}" "${destination}" ||
      Die "different support files collide at ${destination}"
    return
  fi
  cp -p -- "${source}" "${destination}"
}

CopyCommand() {
  local -r name="$1"
  local source resolved target_name
  source="$(type -P -- "${name}" || true)"
  [[ -n "${source}" ]] || Die "required runtime command is unavailable: ${name}"
  RejectUnsafeText "${source}" "command path"
  [[ "${source}" == /* ]] || Die "command path is not absolute: ${source}"
  resolved="$(realpath -e -- "${source}")" ||
    Die "cannot resolve runtime command: ${source}"
  [[ -f "${resolved}" && -x "${resolved}" ]] ||
    Die "runtime command is not an executable regular file: ${resolved}"
  target_name="$(basename -- "${resolved}")"
  [[ "${target_name}" =~ ^[A-Za-z0-9._+-]+$ ]] ||
    Die "runtime command has an unsafe target name: ${resolved}"

  InstallRegularFile "${resolved}" "${SUPPORT_RUNTIME}/bin/${target_name}"
  if [[ "${name}" != "${target_name}" ]]; then
    if [[ -e "${SUPPORT_RUNTIME}/bin/${name}" ||
          -L "${SUPPORT_RUNTIME}/bin/${name}" ]]; then
      Die "runtime command name collision: ${name}"
    fi
    ln -s -- "${target_name}" "${SUPPORT_RUNTIME}/bin/${name}"
  fi
}

CopyRuntimeCommands() {
  local command_name
  for command_name in "${REQUIRED_COMMANDS[@]}"; do
    CopyCommand "${command_name}"
  done
  if type -P -- rg >/dev/null 2>&1; then
    CopyCommand rg
  fi
}

CopyCaBundle() {
  local -a candidates=()
  if [[ -n "${SSL_CERT_FILE:-}" ]]; then
    candidates+=("${SSL_CERT_FILE}")
  fi
  candidates+=(
    /etc/ssl/certs/ca-certificates.crt
    /etc/pki/tls/certs/ca-bundle.crt
    /etc/ssl/ca-bundle.pem
    /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem
  )
  local candidate resolved=""
  for candidate in "${candidates[@]}"; do
    [[ "${candidate}" == /* && -r "${candidate}" ]] || continue
    resolved="$(realpath -e -- "${candidate}" 2>/dev/null || true)"
    if [[ -n "${resolved}" && -f "${resolved}" && -s "${resolved}" ]]; then
      break
    fi
    resolved=""
  done
  [[ -n "${resolved}" ]] || Die "cannot find a readable host CA bundle"
  mkdir -p -- "${SUPPORT_RUNTIME}/share/ca-certificates"
  install -m 0644 -- "${resolved}" \
    "${SUPPORT_RUNTIME}/share/ca-certificates/ca-bundle.crt"
}

BuildJlinkRuntime() {
  local jlink_executable jlink_resolved java_home module_path
  jlink_executable="$(type -P -- jlink || true)"
  [[ -n "${jlink_executable}" ]] || Die "jlink is required to build support"
  jlink_resolved="$(realpath -e -- "${jlink_executable}")" ||
    Die "cannot resolve jlink"
  java_home="$(dirname -- "$(dirname -- "${jlink_resolved}")")"
  module_path="${java_home}/jmods"
  [[ -f "${module_path}/java.base.jmod" &&
     -f "${module_path}/java.logging.jmod" &&
     -f "${module_path}/java.desktop.jmod" &&
     -f "${module_path}/jdk.zipfs.jmod" ]] ||
    Die "jlink JMOD files are unavailable under ${java_home}"

  local compression=2
  if "${jlink_resolved}" --help 2>&1 | grep -q 'zip-{0-9}'; then
    compression=zip-6
  fi
  "${jlink_resolved}" --module-path "${module_path}" \
    --add-modules java.base,java.logging,java.desktop,jdk.zipfs \
    --strip-debug --no-man-pages \
    --no-header-files --compress="${compression}" \
    --output "${SUPPORT_RUNTIME}/jre"

  # apkanalyzer needs java.desktop data structures, but neither it nor
  # apksigner uses Java Sound. Some OpenJDK builds link this optional native
  # library against ALSA, which is intentionally absent from the AnyLinux
  # build image and must not become a standalone runtime dependency.
  local java_sound_library
  for java_sound_library in \
      "${SUPPORT_RUNTIME}/jre/lib/libjsound.so" \
      "${SUPPORT_RUNTIME}/jre/lib/libjsoundalsa.so"; do
    if [[ -e "${java_sound_library}" || -L "${java_sound_library}" ]]; then
      [[ ! -d "${java_sound_library}" ]] ||
        Die "Java Sound library path is unexpectedly a directory"
      rm -f -- "${java_sound_library}"
    fi
  done

  [[ -x "${SUPPORT_RUNTIME}/jre/bin/java" ]] ||
    Die "jlink did not produce a Java launcher"
  ln -s -- "../jre/bin/java" "${SUPPORT_RUNTIME}/bin/java"

  local modules
  modules="$("${SUPPORT_RUNTIME}/jre/bin/java" --list-modules |
    sed 's/@.*//' | LC_ALL=C sort)"
  [[ "${modules}" == \
     $'java.base\njava.datatransfer\njava.desktop\njava.logging\njava.prefs\njava.xml\njdk.zipfs' ]] ||
    Die "jlink image contains unexpected modules"
}

ValidateCopiedSymlinks() {
  local -r root="$1"
  local link resolved
  while IFS= read -r -d '' link; do
    resolved="$(realpath -e -- "${link}")" ||
      Die "packaged support contains a dangling symlink: ${link}"
    PathIsInside "${resolved}" "${root}" ||
      Die "packaged support symlink escapes its root: ${link}"
  done < <(find -P "${root}" -type l -print0)
}

VerifySupportRuntime() {
  local -r ca_bundle="${SUPPORT_RUNTIME}/share/ca-certificates/ca-bundle.crt"
  [[ -s "${ca_bundle}" ]] || Die "packaged CA support is incomplete"
  ValidateCopiedSymlinks "${SUPPORT_RUNTIME}"
}

PublishRuntime() {
  local -r destination="${RUNTIME_ROOT}/runtime"
  if [[ -d "${destination}" && ! -L "${destination}" ]]; then
    rmdir -- "${destination}" || Die "runtime destination changed during build"
  elif [[ -e "${destination}" || -L "${destination}" ]]; then
    Die "runtime destination changed during build"
  fi
  mv -T -- "${SUPPORT_RUNTIME}" "${destination}"
  rmdir -- "${STAGING_ROOT}" ||
    Die "standalone support staging root is not empty after publish"
  STAGING_ROOT=""
  Log "native ${TARGET_LIBC} support populated at ${destination}"
}

Main() {
  ParseArguments "$@"
  RequireBuildCommands
  ValidateRuntimeRoot
  ValidateNativeHost

  STAGING_ROOT="$(mktemp -d -- \
    "${RUNTIME_ROOT}/.mocktail-standalone-support.XXXXXX")" ||
    Die "cannot create support staging directory"
  SUPPORT_RUNTIME="${STAGING_ROOT}/runtime"
  mkdir -p -- "${SUPPORT_RUNTIME}/bin" "${SUPPORT_RUNTIME}/jre" \
    "${SUPPORT_RUNTIME}/share"
  rmdir -- "${SUPPORT_RUNTIME}/jre"

  CopyRuntimeCommands
  CopyCaBundle
  BuildJlinkRuntime
  VerifySupportRuntime
  PublishRuntime
}

Main "$@"
