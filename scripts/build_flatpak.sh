#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"

BUILD_DIR="${PROJECT_ROOT}/build-flatpak"
MANIFEST="${PROJECT_ROOT}/packaging/flatpak/space.bigrat.mocktail.json"
JOBS="${MOCKTAIL_FLATPAK_JOBS:-4}"

Usage() {
  cat <<'EOF'
Usage: scripts/build_flatpak.sh [OPTIONS]

Build and install Mocktail as a per-user Flatpak for the host architecture.

Options:
  --build-dir DIR  Builder output directory (default: build-flatpak).
  --manifest FILE  Flatpak manifest path.
  --jobs N         Maximum parallel build jobs (default: 4).
  -h, --help       Show this help.
EOF
}

Die() {
  printf 'mocktail-flatpak: %s\n' "$*" >&2
  exit 1
}

CleanupStaleBuilderMounts() {
  local mount_dir
  local mount_type
  local unmount_command
  local rofiles_root="${PROJECT_ROOT}/.flatpak-builder/rofiles"

  [[ -d "${rofiles_root}" ]] || return 0
  command -v findmnt >/dev/null 2>&1 || return 0
  command -v timeout >/dev/null 2>&1 || return 0

  if command -v fusermount3 >/dev/null 2>&1; then
    unmount_command=fusermount3
  elif command -v fusermount >/dev/null 2>&1; then
    unmount_command=fusermount
  else
    return 0
  fi

  shopt -s nullglob
  for mount_dir in "${rofiles_root}"/rofiles-*; do
    mount_type="$(findmnt -rn -T "${mount_dir}" -o FSTYPE 2>/dev/null || true)"
    [[ "${mount_type}" == fuse* ]] || continue
    if ! timeout 2 stat -- "${mount_dir}" >/dev/null 2>&1; then
      printf 'mocktail-flatpak: unmounting stale builder filesystem: %s\n' \
        "${mount_dir}" >&2
      "${unmount_command}" -uz "${mount_dir}" ||
        Die "cannot unmount stale builder filesystem: ${mount_dir}"
    fi
  done
  shopt -u nullglob
}

while (( $# > 0 )); do
  case "$1" in
    --build-dir)
      (( $# >= 2 )) || Die "--build-dir requires a directory"
      BUILD_DIR="$2"
      shift 2
      ;;
    --manifest)
      (( $# >= 2 )) || Die "--manifest requires a file"
      MANIFEST="$2"
      shift 2
      ;;
    --jobs)
      (( $# >= 2 )) || Die "--jobs requires a positive integer"
      JOBS="$2"
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

[[ "${JOBS}" =~ ^[1-9][0-9]*$ && "${JOBS}" -le 256 ]] ||
  Die "--jobs must be an integer between 1 and 256"

HOST_ARCH="$(uname -m)"
case "${HOST_ARCH}" in
  x86_64|aarch64) ;;
  *) Die "Flatpak builds require an x86_64 or aarch64 Linux host" ;;
esac
command -v flatpak >/dev/null 2>&1 || Die "flatpak is required"

MANIFEST="$(realpath -e -- "${MANIFEST}")" ||
  Die "cannot resolve manifest: ${MANIFEST}"
[[ -f "${MANIFEST}" && ! -L "${MANIFEST}" ]] ||
  Die "manifest must be a regular non-symlink file"
[[ "${MANIFEST}" == "${PROJECT_ROOT}/"* ]] ||
  Die "manifest must be inside the Mocktail source tree"

BUILD_DIR="$(realpath -m -- "${BUILD_DIR}")" ||
  Die "cannot resolve build directory: ${BUILD_DIR}"
[[ "${BUILD_DIR}" == "${PROJECT_ROOT}/"* &&
   "${BUILD_DIR}" != "${PROJECT_ROOT}" ]] ||
  Die "build directory must be a child of the Mocktail source tree"

builder=()
if command -v flatpak-builder >/dev/null 2>&1; then
  builder=(flatpak-builder)
elif flatpak info org.flatpak.Builder >/dev/null 2>&1; then
  builder=(flatpak run org.flatpak.Builder)
else
  Die "flatpak-builder is unavailable; install the host package or run: flatpak install flathub org.flatpak.Builder"
fi

CleanupStaleBuilderMounts

cd -- "${PROJECT_ROOT}"
exec "${builder[@]}" \
  --arch="${HOST_ARCH}" \
  --jobs="${JOBS}" \
  --force-clean \
  --install-deps-from=flathub \
  --user \
  --install \
  "${BUILD_DIR}" \
  "${MANIFEST}"
