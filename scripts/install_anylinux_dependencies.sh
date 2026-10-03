#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

AnyLinuxDependencyDie() {
  printf '[anylinux-dependencies] error: %s\n' "$*" >&2
  exit 1
}

AnyLinuxDependencyRequireContainer() {
  # Never replace a developer's desktop / graphics packages on the host.
  [[ -f /.dockerenv || -f /run/.containerenv ]] ||
    AnyLinuxDependencyDie 'installation requires a disposable container'
  [[ "$(id -u)" == 0 ]] ||
    AnyLinuxDependencyDie 'installation requires container root'
}

AnyLinuxDependencySnapshot() {
  local manifest="$1" release="$2" host_arch="${3:-$(uname -m)}"
  local filename remainder names_json
  [[ "${host_arch}" == x86_64 || "${host_arch}" == aarch64 ]] ||
    AnyLinuxDependencyDie 'unsupported AppImage architecture'
  local -a names=()
  local -A seen=()
  while read -r filename remainder || [[ -n "${filename}" ]]; do
    [[ -n "${filename}" && "${filename}" != \#* ]] || continue
    [[ "${filename}" =~ ^[a-z0-9-]+-mini-${host_arch}\.pkg\.tar\.(zst|xz)$ &&
       -z "${remainder}" && -z "${seen[${filename}]:-}" ]] ||
      AnyLinuxDependencyDie 'invalid or duplicate dependency manifest entry'
    seen["${filename}"]=1
    names+=("${filename}")
  done <"${manifest}"
  (( ${#names[@]} > 0 )) || AnyLinuxDependencyDie 'empty dependency manifest'
  names_json="$(printf '%s\n' "${names[@]}" | jq --raw-input --slurp \
    'split("\n")[:-1]')"

  # continuous assets are replaced on upstream rebuilds. Resolve all required
  # names and GitHub-computed digests in one response, then download by asset ID
  # rather than by rolling filename. These are per-build integrity checks, not
  # immutable version pins. Never accept absent digests or compute our own trust.
  jq --exit-status --raw-output --argjson names "${names_json}" \
    --arg base 'https://api.github.com/repos/pkgforge-dev/archlinux-pkgs-debloated/releases/assets/' '
      (if .tag_name != "continuous" or (.assets | type) != "array" then
        error("invalid continuous release metadata")
      else .assets end) as $assets |
      $names[] as $name |
      [$assets[] | select(.name == $name)] as $matches |
      if ($matches | length) != 1 then
        error("missing or duplicate dependency asset: " + $name)
      else $matches[0] end |
      if .state != "uploaded" or (.id | type) != "number" or
         .id <= 0 or .id != (.id | floor) or
         (.digest | type) != "string" then
        error("invalid dependency asset metadata: " + $name)
      elif (.digest | test("^sha256:[0-9a-f]{64}$") | not) or
           .url != ($base + (.id | tostring)) then
        error("invalid dependency digest or URL: " + $name)
      else
        [(.digest | ltrimstr("sha256:")), $name, .url] | @tsv
      end
    ' "${release}" || AnyLinuxDependencyDie 'cannot resolve verified dependency assets'
}

AnyLinuxDownloadDependencies() {
  local manifest="$1" downloads="$2" attempt digest filename url failed
  local -a curl_options=(
    --fail --silent --show-error --location --retry 3
    --proto '=https' --proto-redir '=https'
    --connect-timeout 15 --max-time 180
    --header 'X-GitHub-Api-Version: 2022-11-28'
  )
  if [[ -n "${GH_TOKEN:-}" ]]; then
    curl_options+=(--header "Authorization: Bearer ${GH_TOKEN}")
  fi
  for attempt in 1 2 3; do
    curl "${curl_options[@]}" --header 'Accept: application/vnd.github+json' \
      --output "${downloads}/release.json" \
      https://api.github.com/repos/pkgforge-dev/archlinux-pkgs-debloated/releases/tags/continuous ||
      AnyLinuxDependencyDie 'cannot fetch upstream release metadata'
    AnyLinuxDependencySnapshot "${manifest}" "${downloads}/release.json" \
      >"${downloads}/dependencies.tsv"
    : >"${downloads}/SHA256SUMS"
    failed=false
    while IFS=$'\t' read -r digest filename url; do
      printf '[anylinux-dependencies] downloading %s (SHA256 %s)\n' \
        "${filename}" "${digest}" >&2
      if ! curl "${curl_options[@]}" --header 'Accept: application/octet-stream' \
          --output "${downloads}/${filename}" "${url}"; then
        failed=true
        break
      fi
      printf '%s  %s\n' "${digest}" "${filename}" >>"${downloads}/SHA256SUMS"
    done <"${downloads}/dependencies.tsv"
    if [[ "${failed}" == true ]]; then
      # An upstream upload can delete an ID between resolution and download.
      # Retry a complete snapshot, never mix old checksums with new filenames.
      printf '[anylinux-dependencies] download failed; refreshing snapshot (%s/3)\n' \
        "${attempt}" >&2
      continue
    fi
    (cd -- "${downloads}" && sha256sum --check --strict SHA256SUMS) ||
      AnyLinuxDependencyDie 'dependency checksum verification failed'
    return 0
  done
  AnyLinuxDependencyDie 'dependency downloads failed after 3 snapshots'
}

AnyLinuxDependenciesMain() (
  AnyLinuxDependencyRequireContainer
  (( $# == 0 )) || AnyLinuxDependencyDie 'no arguments are supported'
  readonly ROOT="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
  local host_arch="$(uname -m)" manifest_name
  case "${host_arch}" in
    x86_64) manifest_name=anylinux-dependencies.txt ;;
    aarch64) manifest_name=anylinux-dependencies-aarch64.txt ;;
    *) AnyLinuxDependencyDie "unsupported AppImage architecture: ${host_arch}" ;;
  esac
  readonly MANIFEST="${ROOT}/packaging/${manifest_name}"
  readonly DOWNLOADS="$(mktemp -d "${TMPDIR:-/tmp}/mocktail-debloated.XXXXXX")"
  trap 'rm -rf -- "${DOWNLOADS}"' EXIT
  AnyLinuxDownloadDependencies "${MANIFEST}" "${DOWNLOADS}"
  local digest filename url
  local -a packages=()
  while IFS=$'\t' read -r digest filename url; do
    packages+=("${DOWNLOADS}/${filename}")
  done <"${DOWNLOADS}/dependencies.tsv"
  pacman -U --noconfirm --needed "${packages[@]}"
)

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  AnyLinuxDependenciesMain "$@"
fi
