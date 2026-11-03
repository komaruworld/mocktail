#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

info()    { echo -e "${CYAN}[info]${RESET}  $*"; }
success() { echo -e "${GREEN}[ok]${RESET}    $*"; }
warn()    { echo -e "${YELLOW}[warn]${RESET}  $*"; }
die()     { echo -e "${RED}[error]${RESET} $*" >&2; exit 1; }
step()    { echo; echo -e "${BOLD}[$*]${RESET}"; }

Usage() {
  cat <<'EOF'
Usage: scripts/build.sh [OPTIONS]

With no APK option, this helper builds only the native runtime. The first
normal launch downloads, validates, canaries, and activates the supported
Roblox x86_64 payload automatically. The ordinary entry point is `make build`,
which invokes CMake directly without this helper.

Options:
  --apk PATH          Explicitly extract a local Roblox x86_64 APK.
  --apk-url URL       Explicitly download and extract an APK URL.
  --skip-apk          Compatibility alias for managed-payload mode.
  --build-dir PATH    CMake build directory (default: build).
  --cmake-toolchain FILE
                      Explicit CMake toolchain file.
  --cmake-sysroot DIR Explicit compiler/CMake sysroot.
  --build-type TYPE   Debug, Release, or RelWithDebInfo (default: Release).
  --jobs N            Parallel build jobs (default: nproc).
  --run-tests         Run CTest after building.
  --clean             Remove the selected build directory first.
  -h, --help          Show this help.

Examples:
  make build
  ./scripts/build.sh
  ./scripts/build.sh --build-type Debug --run-tests
  ./scripts/build.sh --apk /path/to/roblox-x86_64.apk
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RBX_BIN_DIR="${PROJECT_ROOT}/rbx_bin"
APK_PATH=""
APK_URL=""
EXTRACT_APK=false
EXPLICIT_RUNTIME_OVERRIDE=false
BUILD_TYPE="Release"
JOBS="$(nproc)"
RUN_TESTS=false
CLEAN=false
CMAKE_TOOLCHAIN_FILE=""
CMAKE_SYSROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --apk)
      [[ $# -ge 2 ]] || die "--apk requires a path"
      APK_PATH="$2"
      EXTRACT_APK=true
      shift 2
      ;;
    --apk-url)
      [[ $# -ge 2 ]] || die "--apk-url requires a URL"
      APK_URL="$2"
      EXTRACT_APK=true
      shift 2
      ;;
    --skip-apk)
      EXTRACT_APK=false
      shift
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires a path"
      BUILD_DIR="$2"
      shift 2
      ;;
    --cmake-toolchain)
      [[ $# -ge 2 ]] || die "--cmake-toolchain requires a file"
      CMAKE_TOOLCHAIN_FILE="$2"
      shift 2
      ;;
    --cmake-sysroot)
      [[ $# -ge 2 ]] || die "--cmake-sysroot requires a directory"
      CMAKE_SYSROOT="$2"
      shift 2
      ;;
    --build-type)  BUILD_TYPE="$2"; shift 2 ;;
    --jobs)        JOBS="$2"; shift 2 ;;
    --run-tests)   RUN_TESTS=true; shift ;;
    --clean)       CLEAN=true; shift ;;
    --help|-h)
      Usage
      exit 0
      ;;
    *)
      die "Unknown option: $1  (try --help)"
      ;;
  esac
done

if [[ "${EXTRACT_APK}" == true && -n "${APK_PATH}" && -n "${APK_URL}" ]]; then
  die "--apk and --apk-url are mutually exclusive"
fi

if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${PROJECT_ROOT}/${BUILD_DIR}"
fi
BUILD_DIR="$(readlink -m -- "${BUILD_DIR}")"
[[ "${BUILD_DIR}" != / && "${BUILD_DIR}" != "${PROJECT_ROOT}" ]] ||
  die "Unsafe build directory: ${BUILD_DIR}"
if [[ -n "${CMAKE_TOOLCHAIN_FILE}" ]]; then
  [[ -f "${CMAKE_TOOLCHAIN_FILE}" ]] ||
    die "CMake toolchain file not found: ${CMAKE_TOOLCHAIN_FILE}"
  CMAKE_TOOLCHAIN_FILE="$(readlink -f -- "${CMAKE_TOOLCHAIN_FILE}")"
fi
if [[ -n "${CMAKE_SYSROOT}" ]]; then
  [[ -d "${CMAKE_SYSROOT}" ]] ||
    die "CMake sysroot not found: ${CMAKE_SYSROOT}"
  CMAKE_SYSROOT="$(cd -P -- "${CMAKE_SYSROOT}" && pwd)"
fi

info "Mocktail build: ${BUILD_TYPE}, -j${JOBS}"
info "Project root: ${PROJECT_ROOT}"
info "Build directory: ${BUILD_DIR}"

step "1/5 checking dependencies"

check_tool() {
  local tool="$1"
  local hint="${2:-install ${tool}}"
  if command -v "${tool}" &>/dev/null; then
    success "${tool} found: $(command -v "${tool}")"
  else
    die "${tool} not found. Hint: ${hint}"
  fi
}

check_tool cmake   "install CMake 3.20 or newer"
if [[ -z "${CMAKE_TOOLCHAIN_FILE}" ]]; then
  check_tool c++ "install GCC or Clang with C++17 support"
fi
check_tool git     "sudo apt install git"
check_tool java    "sudo apt install default-jdk"
check_tool ld.lld  "install LLD"
check_tool elfedit "install binutils"

if [[ "${EXTRACT_APK}" == true ]]; then
  check_tool unzip "sudo apt install unzip"
fi
if [[ "${EXTRACT_APK}" == true && -n "${APK_URL}" ]] &&
   ! command -v curl &>/dev/null; then
  check_tool wget "sudo apt install wget"
fi

if [[ "${EXTRACT_APK}" == true && -n "${APK_URL}" ]]; then
  step "2/5 downloading Roblox APK"
  APK_PATH="${PROJECT_ROOT}/rbx_bin/roblox-x86_64.apk"
  mkdir -p "${RBX_BIN_DIR}"
  info "URL: ${APK_URL}"
  info "Destination: ${APK_PATH}"
  if command -v curl &>/dev/null; then
    curl -L --fail --progress-bar -o "${APK_PATH}" "${APK_URL}"
  else
    wget -q --show-progress -O "${APK_PATH}" "${APK_URL}"
  fi
  success "APK downloaded."
fi

LIBROBLOX_DST="${RBX_BIN_DIR}/libroblox.so"

if [[ -n "${ROBLOX_LIB_PATH:-}" ]]; then
  info "Explicit runtime library override detected — APK extraction disabled."
  EXTRACT_APK=false
  EXPLICIT_RUNTIME_OVERRIDE=true
fi

if [[ "${EXTRACT_APK}" == true ]]; then
  step "3/5 extracting libroblox.so"
  [[ -n "${APK_PATH}" ]] || die "explicit APK extraction has no input"
  [[ -f "${APK_PATH}" ]] || die "APK not found: ${APK_PATH}"
  mkdir -p "${RBX_BIN_DIR}"
  info "APK: ${APK_PATH}"
  info "Looking for lib/x86_64/libroblox.so inside the archive…"

  if ! unzip -l "${APK_PATH}" | grep -q "lib/x86_64/libroblox.so"; then
    die "APK does not contain lib/x86_64/libroblox.so; use an x86_64 split APK"
  else
    unzip -jo "${APK_PATH}" "lib/x86_64/libroblox.so" -d "${RBX_BIN_DIR}"
    success "Extracted → ${LIBROBLOX_DST}"
  fi
else
  step "2/5 selecting managed Roblox payload"
  success "No APK or libroblox.so is required at build time."
fi

step "4/5 initialising git submodules"
cd "${PROJECT_ROOT}"
if [[ -f ".gitmodules" ]]; then
  git submodule update --init --recursive
  success "Submodules ready."
else
  warn "No .gitmodules found — skipping submodule init."
  info "To add submodules run: ./scripts/add_submodules.sh"
fi

step "5/5 configuring CMake"

if [[ "${CLEAN}" == true && -d "${BUILD_DIR}" ]]; then
  info "Removing old build directory…"
  rm -rf "${BUILD_DIR}"
fi

if [[ "${RUN_TESTS}" == true ]]; then
  CMAKE_BUILD_TESTING=ON
else
  CMAKE_BUILD_TESTING=OFF
fi

cmake_arguments=(
  -S "${PROJECT_ROOT}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DBUILD_TESTING="${CMAKE_BUILD_TESTING}"
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)
if [[ -n "${CMAKE_TOOLCHAIN_FILE}" ]]; then
  cmake_arguments+=("-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
fi
if [[ -n "${CMAKE_SYSROOT}" ]]; then
  cmake_arguments+=("-DCMAKE_SYSROOT=${CMAKE_SYSROOT}")
fi
cmake "${cmake_arguments[@]}"

success "CMake configuration complete."

step "building Mocktail"

cmake --build "${BUILD_DIR}" -j"${JOBS}"

success "Build complete → ${BUILD_DIR}/mocktail"

if [[ "${RUN_TESTS}" == true ]]; then
  step "running unit tests"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure -j"${JOBS}"
  success "All tests passed."
fi

echo
success "Build successful."
echo "Run:"
echo -e "  ${CYAN}${BUILD_DIR}/mocktail${RESET}"
if [[ "${EXPLICIT_RUNTIME_OVERRIDE}" == true ]]; then
  info "The explicit runtime library override remains active for this shell."
elif [[ "${EXTRACT_APK}" == false ]]; then
  info "First launch downloads, verifies, and activates the supported Roblox x86_64 payload automatically."
else
  info "Explicit APK extraction completed; managed first-run updates remain available."
fi
echo
