#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

# The reduced packages are intentionally limited to the disposable builder.
# Never replace a developer's desktop / graphics packages on the host.
[[ -f /.dockerenv || -f /run/.containerenv ]] || {
  printf 'AnyLinux dependency installation requires a disposable container\n' >&2
  exit 1
}
[[ "$(id -u)" == 0 ]] || {
  printf 'AnyLinux dependency installation requires container root\n' >&2
  exit 1
}

readonly ROOT="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
readonly MANIFEST="${ROOT}/packaging/anylinux-dependencies.sha256"
readonly DOWNLOADS="$(mktemp -d "${TMPDIR:-/tmp}/mocktail-debloated.XXXXXX")"
trap 'rm -rf -- "${DOWNLOADS}"' EXIT
readonly BASE_URL=https://github.com/pkgforge-dev/archlinux-pkgs-debloated/releases/download/continuous
declare -a packages=()
while read -r digest filename remainder; do
  [[ -n "${digest}" && "${digest}" != \#* ]] || continue
  [[ "${digest}" =~ ^[0-9a-f]{64}$ &&
     "${filename}" =~ ^[a-z0-9-]+-x86_64\.pkg\.tar\.zst$ &&
     -z "${remainder}" ]] || {
    printf 'Invalid AnyLinux dependency manifest entry\n' >&2
    exit 1
  }
  curl --fail --location --retry 3 --output "${DOWNLOADS}/${filename}" \
    "${BASE_URL}/${filename}"
  printf '%s  %s\n' "${digest}" "${DOWNLOADS}/${filename}" |
    sha256sum --check --strict
  packages+=("${DOWNLOADS}/${filename}")
done <"${MANIFEST}"
(( ${#packages[@]} > 0 ))
pacman -U --noconfirm --needed "${packages[@]}"
