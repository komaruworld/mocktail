#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"
readonly TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mocktail-dependency-test.XXXXXX")"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT
source "${ROOT}/scripts/install_anylinux_dependencies.sh"

Fail() {
  printf 'AnyLinux dependency test failed: %s\n' "$*" >&2
  exit 1
}

readonly API=https://api.github.com/repos/pkgforge-dev/archlinux-pkgs-debloated/releases
readonly FFMPEG=ffmpeg-mini-x86_64.pkg.tar.zst
readonly OPUS=opus-mini-x86_64.pkg.tar.zst
printf 'ffmpeg fixture\n' >"${TEMP_DIR}/ffmpeg"
printf 'updated ffmpeg fixture\n' >"${TEMP_DIR}/updated-ffmpeg"
printf 'opus fixture\n' >"${TEMP_DIR}/opus"
readonly FFMPEG_HASH="$(sha256sum "${TEMP_DIR}/ffmpeg" | cut -d ' ' -f 1)"
readonly UPDATED_FFMPEG_HASH="$(sha256sum "${TEMP_DIR}/updated-ffmpeg" | cut -d ' ' -f 1)"
readonly OPUS_HASH="$(sha256sum "${TEMP_DIR}/opus" | cut -d ' ' -f 1)"
printf '# Required fixture packages\n\n%s\n%s' "${FFMPEG}" "${OPUS}" \
  >"${TEMP_DIR}/manifest.txt"
jq --null-input --arg api "${API}" --arg ffmpeg "${FFMPEG}" --arg opus "${OPUS}" \
  --arg ffmpeg_hash "${FFMPEG_HASH}" --arg opus_hash "${OPUS_HASH}" '
    {tag_name: "continuous", assets: [
      {id: 10, name: $ffmpeg, state: "uploaded", digest: ("sha256:" + $ffmpeg_hash),
       url: ($api + "/assets/10")},
      {id: 20, name: $opus, state: "uploaded", digest: ("sha256:" + $opus_hash),
       url: ($api + "/assets/20")},
      {id: 30, name: "unrequested-mini-x86_64.pkg.tar.zst", state: "uploaded",
       digest: null, url: ($api + "/assets/30")}
    ]}
  ' >"${TEMP_DIR}/release.json"
jq --arg hash "${UPDATED_FFMPEG_HASH}" '
    .assets[0].id = 11 | .assets[0].url |= sub("/10$"; "/11") |
    .assets[0].digest = ("sha256:" + $hash) |
    .assets[1].id = 21 | .assets[1].url |= sub("/20$"; "/21")
  ' \
  "${TEMP_DIR}/release.json" >"${TEMP_DIR}/refreshed-release.json"
mkdir -p "${TEMP_DIR}/project/scripts" "${TEMP_DIR}/project/packaging"
cp "${ROOT}/scripts/install_anylinux_dependencies.sh" "${TEMP_DIR}/project/scripts/"
cp "${TEMP_DIR}/manifest.txt" "${TEMP_DIR}/project/packaging/anylinux-dependencies.txt"
cp "${ROOT}/packaging/anylinux-dependencies-aarch64.txt" \
  "${TEMP_DIR}/project/packaging/"

# Record requests without any network access or changes to host packages.
curl() {
  local output="" accept="" authorization="" url=""
  while (( $# > 0 )); do
    case "$1" in
      --output) output="$2"; shift 2 ;;
      --header)
        case "$2" in
          Accept:*) accept="$2" ;;
          Authorization:*) authorization="$2" ;;
        esac
        shift 2 ;;
      --retry|--proto|--proto-redir|--connect-timeout|--max-time) shift 2 ;;
      --fail|--silent|--show-error|--location) shift ;;
      https://*) url="$1"; shift ;;
      *) Fail "unexpected curl option: $1" ;;
    esac
  done
  if [[ -n "${GH_TOKEN:-}" ]]; then
    [[ "${authorization}" == "Authorization: Bearer ${GH_TOKEN}" ]] ||
      Fail 'missing authenticated GitHub request'
  fi
  printf '%s\n' "${url}" >>"${TEMP_DIR}/calls"
  case "${url}" in
    "${API}/tags/continuous")
      [[ "${accept}" == 'Accept: application/vnd.github+json' ]]
      if [[ "${SCENARIO}" == metadata-failure ]]; then return 22; fi
      if [[ "${SCENARIO}" == replaced && -f "${TEMP_DIR}/asset-replaced" ]]; then
        cp "${TEMP_DIR}/refreshed-release.json" "${output}"
      else
        cp "${TEST_RELEASE}" "${output}"
      fi ;;
    "${API}/assets/10"|"${API}/assets/11"|"${API}/assets/20"|"${API}/assets/21")
      [[ "${accept}" == 'Accept: application/octet-stream' ]]
      if [[ "${SCENARIO}" == download-failure ]]; then return 22; fi
      if [[ "${SCENARIO}" == replaced && "${url}" == "${API}/assets/20" ]]; then
        touch "${TEMP_DIR}/asset-replaced"
        return 22
      fi
      if [[ "${url}" == "${API}/assets/20" || "${url}" == "${API}/assets/21" ]]; then
        cp "${TEMP_DIR}/opus" "${output}"
      elif [[ "${url}" == "${API}/assets/11" ]]; then
        cp "${TEMP_DIR}/updated-ffmpeg" "${output}"
      else
        cp "${TEMP_DIR}/ffmpeg" "${output}"
      fi
      if [[ "${SCENARIO}" == corrupted && "${url}" == "${API}/assets/20" ]]; then
        printf 'corrupted\n' >>"${output}"
      fi ;;
    *) Fail "unrequested or rolling asset URL: ${url}" ;;
  esac
}

pacman() {
  [[ "$#" == 5 && "$1" == -U && "$2" == --noconfirm && "$3" == --needed ]]
  if [[ "${SCENARIO}" == replaced ]]; then
    cmp "${TEMP_DIR}/updated-ffmpeg" "$4"
  else
    cmp "${TEMP_DIR}/ffmpeg" "$4"
  fi
  cmp "${TEMP_DIR}/opus" "$5"
  local downloads="${4%/*}"
  (cd "${downloads}" && sha256sum --check --strict SHA256SUMS)
  cp "$4" "$5" "${downloads}/SHA256SUMS" "${downloads}/dependencies.tsv" \
    "${TEMP_DIR}/${SCENARIO}/"
  touch "${TEMP_DIR}/installed"
}

export -f curl pacman Fail
export TEMP_DIR API FFMPEG OPUS SCENARIO TEST_RELEASE

ExpectRejectedSnapshot() {
  local description="$1" filter="$2"
  jq "${filter}" "${TEMP_DIR}/release.json" >"${TEMP_DIR}/invalid.json"
  if (AnyLinuxDependencySnapshot "${TEMP_DIR}/manifest.txt" \
      "${TEMP_DIR}/invalid.json" x86_64) >"${TEMP_DIR}/invalid.log" 2>&1; then
    Fail "accepted ${description}"
  fi
}

(AnyLinuxDependencySnapshot "${TEMP_DIR}/manifest.txt" \
  "${TEMP_DIR}/release.json" x86_64) >"${TEMP_DIR}/snapshot.tsv" ||
  Fail 'rejected valid dependency snapshot'
printf '%s\t%s\t%s\n' \
  "${FFMPEG_HASH}" "${FFMPEG}" "${API}/assets/10" \
  "${OPUS_HASH}" "${OPUS}" "${API}/assets/20" \
  >"${TEMP_DIR}/expected-snapshot.tsv"
cmp "${TEMP_DIR}/expected-snapshot.tsv" "${TEMP_DIR}/snapshot.tsv" ||
  Fail 'incorrect dependency snapshot'

readonly ARM_PACKAGE=gdk-pixbuf2-mini-aarch64.pkg.tar.xz
printf '%s\n' "${ARM_PACKAGE}" >"${TEMP_DIR}/arm-manifest.txt"
jq --null-input --arg api "${API}" --arg name "${ARM_PACKAGE}" \
  --arg hash "${FFMPEG_HASH}" '
    {tag_name: "continuous", assets: [
      {id: 40, name: $name, state: "uploaded",
       digest: ("sha256:" + $hash), url: ($api + "/assets/40")}
    ]}
  ' >"${TEMP_DIR}/arm-release.json"
AnyLinuxDependencySnapshot "${TEMP_DIR}/arm-manifest.txt" \
  "${TEMP_DIR}/arm-release.json" aarch64 >"${TEMP_DIR}/arm-snapshot.tsv" ||
  Fail 'rejected valid aarch64 dependency snapshot'
printf '%s\t%s\t%s\n' "${FFMPEG_HASH}" "${ARM_PACKAGE}" \
  "${API}/assets/40" >"${TEMP_DIR}/expected-arm-snapshot.tsv"
cmp "${TEMP_DIR}/expected-arm-snapshot.tsv" "${TEMP_DIR}/arm-snapshot.tsv" ||
  Fail 'incorrect aarch64 dependency snapshot'
if (AnyLinuxDependencySnapshot "${TEMP_DIR}/arm-manifest.txt" \
    "${TEMP_DIR}/arm-release.json" x86_64) >/dev/null 2>&1; then
  Fail 'accepted an aarch64 dependency on an x86_64 host'
fi
bash -Eeuo pipefail -c '
  source "$1/project/scripts/install_anylinux_dependencies.sh"
  AnyLinuxDependencyRequireContainer() { :; }
  uname() { printf "aarch64\n"; }
  AnyLinuxDownloadDependencies() {
    printf "%s\n" "$1" >"${TEMP_DIR}/arm-selected-manifest"
    exit 0
  }
  AnyLinuxDependenciesMain
' _ "${TEMP_DIR}" || Fail 'aarch64 manifest selection failed'
grep -Fxq "${TEMP_DIR}/project/packaging/anylinux-dependencies-aarch64.txt" \
  "${TEMP_DIR}/arm-selected-manifest" ||
  Fail 'aarch64 build did not select its dependency manifest'

ExpectRejectedSnapshot 'missing asset' '.assets |= .[1:]'
ExpectRejectedSnapshot 'duplicate asset' '.assets += [.assets[0]]'
ExpectRejectedSnapshot 'absent SHA256' '.assets[0].digest = null'
ExpectRejectedSnapshot 'malformed SHA256' '.assets[0].digest = "sha256:bad"'
ExpectRejectedSnapshot 'foreign asset URL' '.assets[0].url = "https://example.org/archive"'
ExpectRejectedSnapshot 'fractional asset ID' '.assets[0].id = 10.5'
ExpectRejectedSnapshot 'unuploaded asset' '.assets[0].state = "starter"'
ExpectRejectedSnapshot 'different release' '.tag_name = "other"'

for invalid_manifest in '../ffmpeg-mini-x86_64.pkg.tar.zst' \
    "${FFMPEG} extra" "${FFMPEG}"$'\n'"${FFMPEG}" ''; do
  printf '%s\n' "${invalid_manifest}" >"${TEMP_DIR}/invalid-manifest.txt"
  if (AnyLinuxDependencySnapshot "${TEMP_DIR}/invalid-manifest.txt" \
      "${TEMP_DIR}/release.json" x86_64) >"${TEMP_DIR}/invalid.log" 2>&1; then
    Fail 'accepted unsafe, duplicate, or empty package allowlist'
  fi
done

for SCENARIO in success replaced corrupted download-failure metadata-failure invalid-metadata; do
  TEST_RELEASE="${TEMP_DIR}/release.json"
  if [[ "${SCENARIO}" == invalid-metadata ]]; then
    # A valid first row must not allow downloads when a later row is invalid.
    jq '.assets[1].digest = null' "${TEMP_DIR}/release.json" >"${TEMP_DIR}/invalid.json"
    TEST_RELEASE="${TEMP_DIR}/invalid.json"
  fi
  mkdir "${TEMP_DIR}/${SCENARIO}"
  : >"${TEMP_DIR}/calls"
  if GH_TOKEN=fixture-token bash -Eeuo pipefail -c '
      source "$1/project/scripts/install_anylinux_dependencies.sh"
      AnyLinuxDependencyRequireContainer() { :; }
      uname() { printf "x86_64\n"; }
      AnyLinuxDependenciesMain
    ' _ "${TEMP_DIR}" \
      >"${TEMP_DIR}/${SCENARIO}.log" 2>&1; then
    [[ "${SCENARIO}" == success || "${SCENARIO}" == replaced ]] ||
      Fail "installed packages after ${SCENARIO}"
    [[ -f "${TEMP_DIR}/installed" ]]
    (cd "${TEMP_DIR}/${SCENARIO}" && sha256sum --check --strict SHA256SUMS)
    rm -- "${TEMP_DIR}/installed"
  else
    if [[ "${SCENARIO}" == success || "${SCENARIO}" == replaced ]]; then
      cat -- "${TEMP_DIR}/${SCENARIO}.log" >&2
      Fail "valid download failed: ${SCENARIO}"
    fi
    [[ ! -e "${TEMP_DIR}/installed" ]] || Fail 'installed unverified archives'
  fi
  case "${SCENARIO}" in
    success)
      [[ "$(wc -l <"${TEMP_DIR}/calls")" == 3 ]]
      grep -Fq "${API}/assets/10" "${TEMP_DIR}/calls" ;;
    replaced)
      [[ "$(wc -l <"${TEMP_DIR}/calls")" == 6 ]]
      grep -Fq "${API}/assets/11" "${TEMP_DIR}/calls"
      grep -Fq "${API}/assets/11" "${TEMP_DIR}/replaced/dependencies.tsv"
      grep -Fq "${API}/assets/21" "${TEMP_DIR}/replaced/dependencies.tsv" ;;
    corrupted)
      grep -Fq 'dependency checksum verification failed' "${TEMP_DIR}/${SCENARIO}.log" ;;
    download-failure)
      [[ "$(wc -l <"${TEMP_DIR}/calls")" == 6 ]]
      grep -Fq 'failed after 3 snapshots' "${TEMP_DIR}/${SCENARIO}.log" ;;
    metadata-failure|invalid-metadata)
      [[ "$(wc -l <"${TEMP_DIR}/calls")" == 1 ]] ;;
  esac
done

if (id() { printf '1000\n'; }; AnyLinuxDependencyRequireContainer) \
    >"${TEMP_DIR}/guard.log" 2>&1; then
  Fail 'container/root installation guard was bypassed'
fi

printf 'AnyLinux dependency tests passed\n'
