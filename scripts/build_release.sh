#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail
umask 022

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -P -- "${SCRIPT_DIR}/.." && pwd)"
readonly ABI_VERIFIER="${PROJECT_ROOT}/scripts/verify_linux_libc_abi.sh"
readonly PACKAGER="${PROJECT_ROOT}/scripts/package_portable.sh"

REQUESTED_LIBC="${MOCKTAIL_RELEASE_LIBC:-auto}"
MODE="${MOCKTAIL_RELEASE_MODE:-standalone}"
JOBS="${MOCKTAIL_BUILD_JOBS:-$(nproc)}"
BUILD_TYPE="${MOCKTAIL_BUILD_TYPE:-Release}"
BUILD_DIR="${MOCKTAIL_RELEASE_BUILD_DIR:-}"
TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE:-}"
SYSROOT="${CMAKE_SYSROOT:-}"
CLEAN="${MOCKTAIL_BUILD_CLEAN:-false}"
TARGET_LIBC=""
HOST_LIBC=""

Usage() {
  cat <<'EOF'
Usage: scripts/build_release.sh [OPTIONS]

Build and package one native x86-64 Linux libc target as an AppImage. This
command never starts a container and never labels a glibc binary as musl.

Options:
  --libc ABI          auto (default), glibc, or musl.
  --mode MODE         standalone (default) or thin; full/static and
                      dynamic/minimal aliases are accepted.
  --toolchain FILE    Explicit CMake toolchain file.
  --sysroot DIR       Explicit compiler/CMake sysroot.
  --build-dir DIR     Override the CMake build directory.
  --build-type TYPE   CMake build type (default: Release).
  --jobs N            Parallel build jobs (default: nproc).
  --clean             Recreate the selected CMake build directory.
  -h, --help          Show this help.

Without --toolchain or --sysroot, an explicit libc must match the native host
libc. Explicit cross-libc inputs are supported only by thin mode, and every
resulting ELF is checked after linking. Standalone mode requires a native matching
host because its dependency closure uses target ldd. LIBC=auto with a
toolchain is resolved from the linked executable before publication.

standalone bundles the application userspace, including support tools. thin
keeps only Mocktail-owned artifacts and bootstraps host dependencies before
launch. The output path is dist/Mocktail-x86_64-<libc>-<mode>.AppImage.
EOF
}

Log() {
  printf '[build-release] %s\n' "$*" >&2
}

Die() {
  Log "error: $*"
  exit 1
}

RequireCommand() {
  command -v "$1" >/dev/null 2>&1 || Die "missing required command: $1"
}

CanonicalFile() {
  local path="$1"
  [[ -f "${path}" ]] || Die "file does not exist: ${path}"
  readlink -f -- "${path}"
}

CanonicalDirectory() {
  local path="$1"
  [[ -d "${path}" ]] || Die "directory does not exist: ${path}"
  cd -P -- "${path}" && pwd
}

NormalizeMode() {
  case "${MODE}" in
    standalone|full|static) MODE=standalone ;;
    thin|dynamic|minimal) MODE=thin ;;
    *) Die "--mode must be standalone or thin" ;;
  esac
}

ParseArguments() {
  while (( $# > 0 )); do
    case "$1" in
      --libc)
        (( $# >= 2 )) || Die "--libc requires auto, glibc, or musl"
        REQUESTED_LIBC="$2"
        shift 2
        ;;
      --mode)
        (( $# >= 2 )) || Die "--mode requires standalone or thin"
        MODE="$2"
        shift 2
        ;;
      --toolchain)
        (( $# >= 2 )) || Die "--toolchain requires a file"
        TOOLCHAIN_FILE="$2"
        shift 2
        ;;
      --sysroot)
        (( $# >= 2 )) || Die "--sysroot requires a directory"
        SYSROOT="$2"
        shift 2
        ;;
      --build-dir)
        (( $# >= 2 )) || Die "--build-dir requires a directory"
        BUILD_DIR="$2"
        shift 2
        ;;
      --build-type)
        (( $# >= 2 )) || Die "--build-type requires a value"
        BUILD_TYPE="$2"
        shift 2
        ;;
      --jobs)
        (( $# >= 2 )) || Die "--jobs requires a positive integer"
        JOBS="$2"
        shift 2
        ;;
      --clean)
        CLEAN=true
        shift
        ;;
      -h|--help)
        Usage
        exit 0
        ;;
      *) Die "unknown option: $1" ;;
    esac
  done

  [[ "${REQUESTED_LIBC}" == auto || "${REQUESTED_LIBC}" == glibc ||
     "${REQUESTED_LIBC}" == musl ]] ||
    Die "--libc must be auto, glibc, or musl"
  [[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || Die "invalid job count: ${JOBS}"
  [[ "${CLEAN}" == true || "${CLEAN}" == false || "${CLEAN}" == 0 ||
     "${CLEAN}" == 1 ]] || Die "invalid clean flag: ${CLEAN}"
  NormalizeMode
}

DetectHostLibc() {
  local shell_executable
  [[ -r "/proc/$$/exe" ]] ||
    Die "cannot inspect the build shell through /proc/$$/exe"
  shell_executable="$(readlink -f -- "/proc/$$/exe")"
  HOST_LIBC="$("${ABI_VERIFIER}" --detect "${shell_executable}")" ||
    Die "cannot identify native host libc from ${shell_executable}"
  [[ "${HOST_LIBC}" == glibc || "${HOST_LIBC}" == musl ]] ||
    Die "unsupported native host libc: ${HOST_LIBC}"
}

ResolveBuildContract() {
  local has_explicit_target=false
  [[ -z "${TOOLCHAIN_FILE}" && -z "${SYSROOT}" ]] ||
    has_explicit_target=true

  if [[ -n "${TOOLCHAIN_FILE}" ]]; then
    TOOLCHAIN_FILE="$(CanonicalFile "${TOOLCHAIN_FILE}")"
  fi
  if [[ -n "${SYSROOT}" ]]; then
    SYSROOT="$(CanonicalDirectory "${SYSROOT}")"
  fi

  if [[ "${has_explicit_target}" == false ]]; then
    if [[ "${REQUESTED_LIBC}" == auto ]]; then
      TARGET_LIBC="${HOST_LIBC}"
    elif [[ "${REQUESTED_LIBC}" != "${HOST_LIBC}" ]]; then
      Die "requested ${REQUESTED_LIBC}, but the native host is ${HOST_LIBC}; use a native ${REQUESTED_LIBC} host or pass --toolchain/--sysroot"
    else
      TARGET_LIBC="${REQUESTED_LIBC}"
    fi
  elif [[ "${REQUESTED_LIBC}" != auto ]]; then
    TARGET_LIBC="${REQUESTED_LIBC}"
  fi

  if [[ "${MODE}" == standalone && -n "${TARGET_LIBC}" &&
        "${TARGET_LIBC}" != "${HOST_LIBC}" ]]; then
    Die "standalone mode requires a native ${TARGET_LIBC} host because dependency closure uses target ldd; use MODE=thin for an explicit cross-libc toolchain"
  fi

  local build_selector="${TARGET_LIBC:-auto}"
  if [[ -z "${BUILD_DIR}" ]]; then
    BUILD_DIR="${PROJECT_ROOT}/out/mocktail-linux-x86_64-${build_selector}-${MODE}-build"
  elif [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${PROJECT_ROOT}/${BUILD_DIR}"
  fi
  BUILD_DIR="$(readlink -m -- "${BUILD_DIR}")"
  [[ "${BUILD_DIR}" != / && "${BUILD_DIR}" != "${PROJECT_ROOT}" ]] ||
    Die "unsafe build directory: ${BUILD_DIR}"
}

BuildRuntime() {
  local -a arguments=(
    --no-print-directory
    build
    "BUILD_DIR=${BUILD_DIR}"
    "BUILD_TYPE=${BUILD_TYPE}"
    "JOBS=${JOBS}"
    "RELEASE_CHECK=ON"
  )
  if [[ -n "${TOOLCHAIN_FILE}" ]]; then
    arguments+=("CMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
  fi
  if [[ -n "${SYSROOT}" ]]; then
    arguments+=("CMAKE_SYSROOT=${SYSROOT}")
  fi
  if [[ "${CLEAN}" == true || "${CLEAN}" == 1 ]]; then
    make -C "${PROJECT_ROOT}" --no-print-directory clean \
      "BUILD_DIR=${BUILD_DIR}"
  fi

  Log "building x86_64 ${TARGET_LIBC:-auto}/${MODE} in ${BUILD_DIR}"
  make -C "${PROJECT_ROOT}" "${arguments[@]}"
}

CollectBuildArtifacts() {
  local -n output=$1
  while IFS= read -r -d '' artifact; do
    output+=("${artifact}")
  done < <(find "${BUILD_DIR}" -maxdepth 1 -type f \
    \( -name mocktail -o -name mocktail_failure_dialog -o \
       -name mocktail_webview_helper -o -name '*.so' \) \
    -print0 | sort -z)
  (( ${#output[@]} > 0 )) || Die "no linked ELF artifacts in ${BUILD_DIR}"
}

VerifyBuildAbi() {
  local detected
  [[ -x "${BUILD_DIR}/mocktail" ]] ||
    Die "linked mocktail executable is unavailable: ${BUILD_DIR}/mocktail"
  detected="$("${ABI_VERIFIER}" --detect "${BUILD_DIR}/mocktail")" ||
    Die "cannot identify linked mocktail libc ABI"
  [[ "${detected}" == glibc || "${detected}" == musl ]] ||
    Die "unsupported linked mocktail libc ABI: ${detected}"

  if [[ -z "${TARGET_LIBC}" ]]; then
    TARGET_LIBC="${detected}"
  elif [[ "${detected}" != "${TARGET_LIBC}" ]]; then
    Die "linked mocktail is ${detected}, expected ${TARGET_LIBC}"
  fi
  if [[ "${MODE}" == standalone && "${TARGET_LIBC}" != "${HOST_LIBC}" ]]; then
    Die "standalone mode cannot resolve ${TARGET_LIBC} dependencies safely on a ${HOST_LIBC} host; rebuild natively or use MODE=thin"
  fi

  local -a artifacts=()
  CollectBuildArtifacts artifacts
  "${ABI_VERIFIER}" --libc "${TARGET_LIBC}" "${artifacts[@]}"
}

PackageRelease() {
  local artifact_name output appimage
  artifact_name="mocktail-linux-x86_64-${TARGET_LIBC}-${MODE}"
  output="${PROJECT_ROOT}/dist/${artifact_name}"
  appimage="${PROJECT_ROOT}/dist/Mocktail-x86_64-${TARGET_LIBC}-${MODE}.AppImage"

  RequireCommand appimagetool
  Log "packaging ${TARGET_LIBC}/${MODE} AppImage"
  "${PACKAGER}" \
    --build-dir "${BUILD_DIR}" \
    --libc "${TARGET_LIBC}" \
    --mode "${MODE}" \
    --output "${output}" \
    --appimage "${appimage}"

  [[ -d "${output}" && -x "${appimage}" ]] ||
    Die "packager did not publish the expected directory and AppImage"
  Log "directory: ${output}"
  Log "AppImage:  ${appimage}"
}

Main() {
  ParseArguments "$@"
  RequireCommand cmake
  RequireCommand find
  RequireCommand ld.lld
  RequireCommand elfedit
  RequireCommand readelf
  RequireCommand readlink
  RequireCommand sort
  DetectHostLibc
  ResolveBuildContract
  BuildRuntime
  VerifyBuildAbi
  PackageRelease
}

Main "$@"
