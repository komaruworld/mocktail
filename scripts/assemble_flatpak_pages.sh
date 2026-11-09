#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail
umask 022

readonly REPOSITORY="${1:?Flatpak repository path is required}"
readonly X86_64_BUNDLE="${2:?x86_64 Flatpak bundle path is required}"
readonly AARCH64_BUNDLE="${3:?aarch64 Flatpak bundle path is required}"
readonly PUBLIC_KEY="${4:?GPG public key path is required}"
readonly OUTPUT="${5:?Pages output path is required}"
readonly NATIVE_REPOSITORIES="${6:?Native repositories path is required}"
readonly BASE_URL="https://mocktail.bigrat.space"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_DIR="$(dirname -- "${SCRIPT_DIR}")"

[[ -d "${REPOSITORY}" && ! -L "${REPOSITORY}" ]] || {
  printf 'Flatpak repository is missing or unsafe: %s\n' "${REPOSITORY}" >&2
  exit 1
}
for bundle in "${X86_64_BUNDLE}" "${AARCH64_BUNDLE}"; do
  [[ -f "${bundle}" && ! -L "${bundle}" ]] || {
    printf 'Flatpak bundle is missing or unsafe: %s\n' "${bundle}" >&2
    exit 1
  }
done
[[ -s "${PUBLIC_KEY}" && ! -L "${PUBLIC_KEY}" ]] || {
  printf 'Flatpak public key is missing or unsafe: %s\n' "${PUBLIC_KEY}" >&2
  exit 1
}
[[ -d "${NATIVE_REPOSITORIES}/apt" &&
   -d "${NATIVE_REPOSITORIES}/rpm" &&
   -d "${NATIVE_REPOSITORIES}/downloads" &&
   -s "${NATIVE_REPOSITORIES}/mocktail-packages.gpg" ]] || {
  printf 'Native repositories are missing: %s\n' "${NATIVE_REPOSITORIES}" >&2
  exit 1
}
[[ ! -e "${OUTPUT}" ]] || {
  printf 'Pages output already exists: %s\n' "${OUTPUT}" >&2
  exit 1
}

mkdir -p -- "${OUTPUT}"
cp -a -- "${REPOSITORY}" "${OUTPUT}/repo"
install -m 0644 -- "${X86_64_BUNDLE}" "${OUTPUT}/Mocktail-x86_64.flatpak"
install -m 0644 -- "${AARCH64_BUNDLE}" "${OUTPUT}/Mocktail-aarch64.flatpak"
install -m 0644 -- "${PUBLIC_KEY}" "${OUTPUT}/mocktail-flatpak.gpg"
install -m 0644 -- \
  "${PROJECT_DIR}/packaging/space.bigrat.mocktail.svg" \
  "${OUTPUT}/mocktail.svg"
install -m 0644 -- \
  "${PROJECT_DIR}/packaging/discord-join.html" \
  "${OUTPUT}/join.html"
install -m 0644 -- "${PROJECT_DIR}/site/index.html" "${OUTPUT}/index.html"
install -m 0644 -- "${PROJECT_DIR}/site/styles.css" "${OUTPUT}/styles.css"
install -d -- "${OUTPUT}/packaging"
install -m 0644 -- \
  "${PROJECT_DIR}/packaging/space.bigrat.mocktail.svg" \
  "${OUTPUT}/packaging/space.bigrat.mocktail.svg"
cp -a -- "${NATIVE_REPOSITORIES}/apt" "${OUTPUT}/apt"
cp -a -- "${NATIVE_REPOSITORIES}/rpm" "${OUTPUT}/rpm"
cp -a -- "${NATIVE_REPOSITORIES}/downloads" "${OUTPUT}/downloads"
install -m 0644 -- \
  "${NATIVE_REPOSITORIES}/mocktail-packages.gpg" \
  "${OUTPUT}/mocktail-packages.gpg"

readonly GPG_KEY="$(base64 --wrap=0 "${PUBLIC_KEY}")"

cat >"${OUTPUT}/mocktail.flatpakrepo" <<EOF
[Flatpak Repo]
Title=Mocktail Nightly
Url=${BASE_URL}/repo/
Homepage=https://github.com/komaruworld/mocktail
Comment=Nightly builds of Mocktail
Description=Signed x86_64 and aarch64 nightly builds published from the latest Mocktail main branch
Icon=${BASE_URL}/mocktail.svg
GPGKey=${GPG_KEY}
EOF

cat >"${OUTPUT}/mocktail.flatpakref" <<EOF
[Flatpak Ref]
Title=Mocktail Nightly
Name=space.bigrat.mocktail
Branch=stable
Url=${BASE_URL}/repo/
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
Homepage=https://github.com/komaruworld/mocktail
Comment=Nightly build from the latest main branch
Icon=${BASE_URL}/mocktail.svg
GPGKey=${GPG_KEY}
IsRuntime=false
EOF
