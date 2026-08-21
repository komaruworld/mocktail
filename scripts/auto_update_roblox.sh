#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

set -Eeuo pipefail
umask 077

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
DATA_ROOT="${MOCKTAIL_DATA_ROOT:-${XDG_DATA_HOME:-${HOME}/.local/share}/mocktail}"
CACHE_ROOT="${MOCKTAIL_CACHE_ROOT:-${XDG_CACHE_HOME:-${HOME}/.cache}/mocktail}"
STATE_ROOT="${MOCKTAIL_STATE_ROOT:-${XDG_STATE_HOME:-${HOME}/.local/state}/mocktail}"
DEFAULT_METADATA_ROOT="${PROJECT_ROOT}/config"
if [[ -d "${PROJECT_ROOT}/metadata" ]]; then
  DEFAULT_METADATA_ROOT="${PROJECT_ROOT}/metadata"
fi
COMPATIBILITY_PATH="${MOCKTAIL_UPDATE_COMPATIBILITY_PATH:-${DEFAULT_METADATA_ROOT}/roblox_compatibility.json}"
BOOTSTRAP_SOURCES_PATH="${MOCKTAIL_BOOTSTRAP_SOURCES_PATH:-${DEFAULT_METADATA_ROOT}/roblox_bootstrap_sources.json}"
SIGNING_TRUST_PATH="${MOCKTAIL_UPDATE_SIGNING_TRUST_PATH:-${DEFAULT_METADATA_ROOT}/roblox_signing_certificates.json}"
# Child validators/providers must use the same resolved source/install layout.
# In an installed tree PROJECT_ROOT contains metadata/, never config/.
export MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${COMPATIBILITY_PATH}"
export MOCKTAIL_BOOTSTRAP_SOURCES_PATH="${BOOTSTRAP_SOURCES_PATH}"
export MOCKTAIL_UPDATE_SIGNING_TRUST_PATH="${SIGNING_TRUST_PATH}"
FETCH_SCRIPT="${MOCKTAIL_UPDATE_FETCH_SCRIPT:-${SCRIPT_DIR}/fetch_roblox_apks.sh}"
VALIDATOR_SCRIPT="${MOCKTAIL_UPDATE_VALIDATOR_SCRIPT:-${SCRIPT_DIR}/update_roblox_payload.sh}"
PAYLOAD_STORE_SCRIPT="${MOCKTAIL_UPDATE_STORE_SCRIPT:-${SCRIPT_DIR}/payload_store.sh}"
SMOKE_SCRIPT="${MOCKTAIL_UPDATE_SMOKE_SCRIPT:-${PROJECT_ROOT}/scripts/real_bringup_smoke.sh}"
PROFILE_DERIVER="${MOCKTAIL_UPDATE_PROFILE_DERIVER:-${SCRIPT_DIR}/derive_roblox_host_abi_profile.py}"
PACKAGED_CANARY_BINARY=""
if [[ "${DEFAULT_METADATA_ROOT}" == "${PROJECT_ROOT}/metadata" ]]; then
  packaged_prefix=""
  if [[ "${PROJECT_ROOT}" == */share/mocktail ]]; then
    packaged_prefix="${PROJECT_ROOT%/share/mocktail}"
  fi
  for packaged_candidate in \
      "${PROJECT_ROOT}/bin/mocktail" \
      "${packaged_prefix:+${packaged_prefix}/lib/mocktail/mocktail}" \
      "${packaged_prefix:+${packaged_prefix}/bin/mocktail}"; do
    if [[ -n "${packaged_candidate}" && -f "${packaged_candidate}" &&
          ! -L "${packaged_candidate}" && -x "${packaged_candidate}" ]]; then
      PACKAGED_CANARY_BINARY="${packaged_candidate}"
      break
    fi
  done
fi
if [[ -n "${MOCKTAIL_CONFIG_FILE:-}" ]]; then
  CONFIG_FILE="${MOCKTAIL_CONFIG_FILE}"
elif [[ -n "${MOCKTAIL_CONFIG_ROOT:-}" ]]; then
  CONFIG_FILE="${MOCKTAIL_CONFIG_ROOT}/config.yaml"
else
  CONFIG_FILE="${XDG_CONFIG_HOME:-${HOME}/.config}/mocktail/config.yaml"
fi
PROVIDER_DIR="${SCRIPT_DIR}/apk_providers"
VERSION_CHECK_COMMAND="${MOCKTAIL_UPDATE_CHECK_SCRIPT:-}"
SOURCE="${MOCKTAIL_UPDATE_SOURCE:-}"
# Packaged launchers invoke this script internally. This lets users pin the
# provider request in YAML (or through this environment override).
VERSION="${MOCKTAIL_UPDATE_VERSION:-}"
EXPECTED_PAYLOAD_VERSION_CODE=""
CANARY_ROOT=""
CANARY_LOG=""
PROBATION_ROOT=""
CANDIDATE_PROFILE=""
CANDIDATE_COMPATIBILITY=""
CANDIDATE_IS_PROBATION=false
CANARY_ATTESTATIONS=()
CANARY_BINARY=""
RUNTIME_FINGERPRINT=""
RUNTIME_BUILD_ID=""
LATEST_STAGED_PAYLOAD_ID=""
LAST_ATTEMPT_BASELINE_PAYLOAD_ID="none"
BOOTSTRAP_SELECTED_VERSION_CODE=""
CANDIDATE_NEEDS_REFERENCE=false
CANDIDATE_REJECTION_DEFERRED=false
CANDIDATE_REJECTION_KEY=""
CANDIDATE_REJECTION_PROFILE_SHA256=""
CANDIDATE_REJECTION_COMPATIBILITY_SHA256=""
CANDIDATE_REJECTION_REFERENCE_SHA256=""
CANDIDATE_REJECTION_DERIVER_SHA256=""
CANDIDATE_REJECTION_SCHEMA_VERSION=""
LAUNCH="${MOCKTAIL_UPDATE_LAUNCH:-false}"
SKIP_BUILD=false
SCHEDULED=false
STARTUP_PREFLIGHT=false
UPDATE_CONFIG_JSON="{}"
STAGED_PAYLOAD_ID=""
DOWNLOAD_STAGING_ROOT="${CACHE_ROOT}/downloads/apk-staging"
DOWNLOADED_BUNDLE_DIR=""
UPDATER_LOG=""
SUPPORT_BUNDLE_COLLECTED=false
SUPPORT_CANDIDATE_METADATA=""
SUPPORT_SOURCE_LOG=""
SUPPORT_FAILURE_REASON="updater-startup-failed"

Usage() {
  cat <<'EOF'
Usage: scripts/auto_update_roblox.sh [OPTIONS]

Discover and stage latest Roblox x86_64, then activate it only after exact
profile derivation and two isolated real-frame canaries. Failed candidates
remain staged while the previous working payload stays current.
An older supported first-run payload may use its pinned bootstrap mirror.

Options:
  --source NAME       Download source. Normal updates use apk-pure.
  --version VERSION   Request an exact provider version.
  --launch            Launch the current supported payload after updating.
  --no-launch         Never launch from this updater invocation.
  --skip-build        Reuse the existing Release binary for the canary.
  --scheduled         Run only when updates.automatic is true in YAML.
  --startup-preflight Validate a runnable local payload without checking for
                      remote updates; provision one when none is available.
  -h, --help          Show this help.
EOF
}

Log() {
  printf '[auto-update] %s\n' "$*" >&2
  if [[ -n "${UPDATER_LOG}" ]]; then
    printf '[auto-update] %s\n' "$*" >>"${UPDATER_LOG}" 2>/dev/null || true
  fi
}

Progress() {
  local -r descriptor="${MOCKTAIL_UPDATE_PROGRESS_FD:-}"
  [[ "${descriptor}" =~ ^[0-9]{1,3}$ ]] || return 0
  (( 10#${descriptor} >= 3 && 10#${descriptor} <= 255 )) || return 0
  [[ -n "$*" ]] || return 0
  printf 'P%s' "$*" >&"${descriptor}" 2>/dev/null || true
}

Die() {
  Log "error: $*"
  exit 1
}

ParseArguments() {
  while (( $# > 0 )); do
    case "$1" in
      --source)
        (( $# >= 2 )) || Die "--source requires a name"
        SOURCE="$2"
        shift 2
        ;;
      --version)
        (( $# >= 2 )) || Die "--version requires a value"
        VERSION="$2"
        if [[ "${VERSION}" =~ ^[0-9]+$ ]]; then
          EXPECTED_PAYLOAD_VERSION_CODE="${VERSION}"
        fi
        shift 2
        ;;
      --launch)
        LAUNCH=true
        shift
        ;;
      --no-launch)
        LAUNCH=false
        shift
        ;;
      --skip-build)
        SKIP_BUILD=true
        shift
        ;;
      --scheduled)
        SCHEDULED=true
        shift
        ;;
      --startup-preflight)
        STARTUP_PREFLIGHT=true
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
}

LoadYamlDefaults() {
  UPDATE_CONFIG_JSON="$(python3 "${SCRIPT_DIR}/read_update_config.py" "${CONFIG_FILE}")"
  [[ -n "${SOURCE}" ]] || SOURCE="$(jq -r '.source // "apk-pure"' <<<"${UPDATE_CONFIG_JSON}")"
  [[ -n "${VERSION}" ]] || VERSION="$(jq -r '.version // empty' <<<"${UPDATE_CONFIG_JSON}")"
  if [[ "${LAUNCH}" != true &&
        "$(jq -r '.launch_after_update // false' <<<"${UPDATE_CONFIG_JSON}")" == true ]]; then
    LAUNCH=true
  fi
}

Cleanup() {
  if [[ -n "${CANARY_ROOT}" ]]; then
    rm -rf -- "${CANARY_ROOT}"
  fi
  if [[ -n "${PROBATION_ROOT}" ]]; then
    rm -rf -- "${PROBATION_ROOT}"
  fi
  CleanupDownloadedBundle
  if [[ -n "${UPDATER_LOG}" ]]; then
    rm -f -- "${UPDATER_LOG}"
    UPDATER_LOG=""
  fi
}

CollectUpdaterSupportBundle() {
  local -r exit_code="$1"
  [[ "${MOCKTAIL_DISABLE_SUPPORT_BUNDLE:-0}" != 1 ]] || return 0
  [[ "${SUPPORT_BUNDLE_COLLECTED}" != true ]] || return 0
  local -r collector="${MOCKTAIL_SUPPORT_BUNDLE_SCRIPT:-${SCRIPT_DIR}/collect_support_bundle.sh}"
  [[ "${collector}" == /* && -f "${collector}" &&
     ! -L "${collector}" && -x "${collector}" ]] || return 0

  local -a arguments=(
    --context updater
    --reason "${SUPPORT_FAILURE_REASON}"
    --exit-code "${exit_code}"
    --output-dir "${STATE_ROOT}/support"
  )
  if [[ -n "${SUPPORT_CANDIDATE_METADATA}" &&
        -f "${SUPPORT_CANDIDATE_METADATA}" &&
        ! -L "${SUPPORT_CANDIDATE_METADATA}" ]]; then
    arguments+=(--payload-metadata "${SUPPORT_CANDIDATE_METADATA}")
  fi
  local source_log="${SUPPORT_SOURCE_LOG}"
  [[ -n "${source_log}" ]] || source_log="${UPDATER_LOG}"
  if [[ -n "${source_log}" && -f "${source_log}" &&
        ! -L "${source_log}" ]]; then
    arguments+=(--log "${source_log}")
  fi

  local archive
  archive="$("${collector}" "${arguments[@]}" 2>/dev/null || true)"
  if [[ -n "${archive}" ]]; then
    SUPPORT_BUNDLE_COLLECTED=true
    local marker="${MOCKTAIL_UPDATE_SUPPORT_MARKER:-}"
    if [[ "${marker}" == "${STATE_ROOT}"/.update-support.*.marker &&
          "${marker##*/}" =~ ^\.update-support\.[0-9]+\.marker$ ]]; then
      mkdir -m 0700 -- "${marker}" 2>/dev/null || true
    fi
  fi
}

OnExit() {
  local -r exit_code="$1"
  trap - EXIT
  if (( exit_code != 0 )); then
    CollectUpdaterSupportBundle "${exit_code}"
  fi
  Cleanup
  exit "${exit_code}"
}

CleanupDownloadedBundle() {
  [[ -n "${DOWNLOADED_BUNDLE_DIR}" ]] || return 0
  local canonical_root canonical_bundle bundle_name
  canonical_root="$(cd -- "${DOWNLOAD_STAGING_ROOT}" 2>/dev/null && pwd -P)" ||
    return 0
  canonical_bundle="$(cd -- "${DOWNLOADED_BUNDLE_DIR}" 2>/dev/null && pwd -P)" ||
    return 0
  bundle_name="${canonical_bundle##*/}"
  if [[ "${canonical_bundle%/*}" != "${canonical_root}" ||
        ! "${bundle_name}" =~ ^com\.roblox\.client-[0-9]+-[0-9a-f]{12}-[0-9a-f]{12}$ ]]; then
    Log "refusing to remove provider bundle outside the managed staging root"
    return 0
  fi
  chmod -R u+w -- "${canonical_bundle}" 2>/dev/null || true
  rm -rf -- "${canonical_bundle}"
  DOWNLOADED_BUNDLE_DIR=""
}

AcquireUpdaterLock() {
  SUPPORT_FAILURE_REASON=updater-lock-failed
  exec 8>"${DATA_ROOT}/.auto-update.lock"
  if flock -n 8; then
    return 0
  fi
  if [[ "${STARTUP_PREFLIGHT}" != true ]]; then
    Die "another automatic Roblox update is already running"
  fi

  Progress "Waiting for Roblox update..."
  Log "another Roblox update is already running; waiting for its result"
  flock 8 || Die "could not wait for the running Roblox update"
  Log "the running Roblox update finished; validating its result"
}

ReleaseUpdaterLock() {
  flock -u 8 2>/dev/null || true
  exec 8>&-
}

VerifiedCurrentPayloadId() {
  local payload_id
  payload_id="$("${PAYLOAD_STORE_SCRIPT}" --root "${DATA_ROOT}" \
    --compatibility "${COMPATIBILITY_PATH}" verify-current)" || return $?
  [[ "${payload_id}" == none ||
     "${payload_id}" =~ ^[0-9]+-[0-9a-f]{40}$ ]] || return 1
  printf '%s\n' "${payload_id}"
}

CurrentPayloadId() {
  local payload_id
  payload_id="$(VerifiedCurrentPayloadId)" ||
    Die "current payload or approval generation failed verification"
  printf '%s\n' "${payload_id}"
}

LocalVersionCode() {
  if [[ -n "${MOCKTAIL_UPDATE_LOCAL_VERSION+x}" ]]; then
    [[ "${MOCKTAIL_UPDATE_LOCAL_VERSION}" =~ ^[0-9]+$ ]] || return 1
    printf '%s\n' "${MOCKTAIL_UPDATE_LOCAL_VERSION}"
    return 0
  fi
  local current_payload_id
  current_payload_id="$(CurrentPayloadId)"
  if [[ "${current_payload_id}" != none ]]; then
    printf '%s\n' "${current_payload_id%%-*}"
    return 0
  fi
  if [[ -f "${PROJECT_ROOT}/rbx_bin/libroblox.so" &&
        ! -L "${PROJECT_ROOT}/rbx_bin/libroblox.so" &&
        -f "${PROJECT_ROOT}/rbx_bin/sober_apk/base.apk" ]] &&
     command -v aapt >/dev/null 2>&1; then
    aapt dump badging "${PROJECT_ROOT}/rbx_bin/sober_apk/base.apk" 2>/dev/null |
      sed -n "s/^package:.* versionCode='\([0-9][0-9]*\)'.*/\1/p" |
      head -n 1
  fi
}

CheckLatestVersion() {
  if [[ -n "${VERSION_CHECK_COMMAND}" ]]; then
    "${VERSION_CHECK_COMMAND}" "${SOURCE}"
    return
  fi
  local -r provider="${PROVIDER_DIR}/direct_apkpure.sh"
  [[ -x "${provider}" ]] || return 1
  local -a arguments=(--check --package com.roblox.client --source "${SOURCE}")
  local -r timeout_seconds="${MOCKTAIL_UPDATE_CHECK_TIMEOUT_S:-5}"
  [[ "${timeout_seconds}" =~ ^[1-9][0-9]?$ ]] && (( timeout_seconds <= 30 )) ||
    Die "MOCKTAIL_UPDATE_CHECK_TIMEOUT_S must be between 1 and 30"
  timeout --signal=TERM --kill-after=1 "${timeout_seconds}s" \
    "${provider}" "${arguments[@]}"
}

FindStagedVersion() {
  local -r version_code="$1"
  local metadata selected_payload_id=""
  for metadata in "${DATA_ROOT}"/payloads/*/roblox_payload.json; do
    [[ -f "${metadata}" ]] || continue
    local payload_dir="${metadata%/roblox_payload.json}"
    if [[ ! -L "${metadata}" &&
          -f "${payload_dir}/libroblox.so" &&
          ! -L "${payload_dir}/libroblox.so" &&
          -d "${payload_dir}/assets/content" &&
          ! -L "${payload_dir}/assets/content" &&
          "$(jq -r '.version_code // empty' "${metadata}")" == "${version_code}" ]]; then
      if [[ -n "${selected_payload_id}" &&
            "${selected_payload_id}" != "${payload_dir##*/}" ]]; then
        STAGED_PAYLOAD_ID=""
        Log "multiple staged Build IDs share versionCode ${version_code}; downloading provider latest to resolve it"
        return 2
      fi
      selected_payload_id="${payload_dir##*/}"
    fi
  done
  [[ -n "${selected_payload_id}" ]] || return 1
  STAGED_PAYLOAD_ID="${selected_payload_id}"
}

FindUniqueSupportedStagedVersion() {
  local -r version_code="$1"
  local metadata payload_dir payload_id build_id
  local selected_payload_id=""
  for metadata in "${DATA_ROOT}"/payloads/*/roblox_payload.json; do
    [[ -f "${metadata}" && ! -L "${metadata}" ]] || continue
    payload_dir="${metadata%/roblox_payload.json}"
    [[ -d "${payload_dir}" && ! -L "${payload_dir}" &&
       -f "${payload_dir}/libroblox.so" &&
       ! -L "${payload_dir}/libroblox.so" &&
       -d "${payload_dir}/assets/content" &&
       ! -L "${payload_dir}/assets/content" ]] || continue
    payload_id="${payload_dir##*/}"
    build_id="$(jq -er '.elf_build_id | select(type == "string") |
      ascii_downcase | select(test("^[0-9a-f]{40}$"))' \
      "${metadata}" 2>/dev/null || true)"
    [[ -n "${build_id}" &&
       "${payload_id}" == "${version_code}-${build_id}" &&
       "$(jq -r '.version_code // empty' "${metadata}")" == \
         "${version_code}" ]] || continue
    IsSupportedCandidate "${metadata}" || continue
    if [[ -n "${selected_payload_id}" &&
          "${selected_payload_id}" != "${payload_id}" ]]; then
      Log "multiple exact-supported staged payloads share versionCode ${version_code}; refusing an ambiguous first-run choice"
      STAGED_PAYLOAD_ID=""
      return 2
    fi
    selected_payload_id="${payload_id}"
  done
  [[ -n "${selected_payload_id}" ]] || return 1
  STAGED_PAYLOAD_ID="${selected_payload_id}"
}

FindNewestSupportedStagedCandidate() {
  local -r minimum_version_code="$1"
  local -r maximum_version_code="$2"
  [[ "${minimum_version_code}" =~ ^[0-9]+$ &&
     "${maximum_version_code}" =~ ^[0-9]+$ ]] || return 1

  local metadata payload_dir payload_id version_code build_id
  local best_payload_id="" best_version_code=0 ambiguous=false
  for metadata in "${DATA_ROOT}"/payloads/*/roblox_payload.json; do
    [[ -f "${metadata}" && ! -L "${metadata}" ]] || continue
    payload_dir="${metadata%/roblox_payload.json}"
    [[ -d "${payload_dir}" && ! -L "${payload_dir}" &&
       -f "${payload_dir}/libroblox.so" &&
       ! -L "${payload_dir}/libroblox.so" &&
       -d "${payload_dir}/assets/content" &&
       ! -L "${payload_dir}/assets/content" ]] || continue
    payload_id="${payload_dir##*/}"
    version_code="$(jq -er '.version_code |
      select(type == "number" and . > 0 and floor == .)' \
      "${metadata}" 2>/dev/null || true)"
    build_id="$(jq -er '.elf_build_id | select(type == "string") |
      ascii_downcase | select(test("^[0-9a-f]{40}$"))' \
      "${metadata}" 2>/dev/null || true)"
    [[ -n "${version_code}" && -n "${build_id}" &&
       "${payload_id}" == "${version_code}-${build_id}" ]] || continue
    (( 10#${version_code} > 10#${minimum_version_code} &&
       10#${version_code} <= 10#${maximum_version_code} )) || continue
    IsSupportedCandidate "${metadata}" || continue

    if (( 10#${version_code} > 10#${best_version_code} )); then
      best_version_code="${version_code}"
      best_payload_id="${payload_id}"
      ambiguous=false
    elif (( 10#${version_code} == 10#${best_version_code} )) &&
         [[ "${payload_id}" != "${best_payload_id}" ]]; then
      ambiguous=true
    fi
  done

  [[ -n "${best_payload_id}" ]] || return 1
  if [[ "${ambiguous}" == true ]]; then
    Log "multiple exact-supported staged payloads share versionCode ${best_version_code}; refusing an ambiguous automatic choice"
    return 2
  fi
  STAGED_PAYLOAD_ID="${best_payload_id}"
}

LatestSupportedBootstrapProfile() {
  local -r remote_version_code="${1:-}"
  jq -er --arg remote_version_code "${remote_version_code}" '
    [.profiles[]? |
      select(($remote_version_code == "" or
              .version_code <= ($remote_version_code | tonumber)) and
             .status == "supported" and .default_allowed == true and
             .allow_legacy_binary_patches == false and
             (.version_name | type) == "string" and
             (.version_name | length) > 0)] |
    group_by(.version_code) |
    map(select(length == 1) | .[0]) |
    sort_by(.version_code) | last | select(type == "object") |
    [.version_code, .version_name] | @tsv
  ' "${COMPATIBILITY_PATH}"
}

SelectSupportedBootstrapFallback() {
  local -r remote_version_code="${1:-}"
  local profile fallback_version_code fallback_version_name
  profile="$(LatestSupportedBootstrapProfile "${remote_version_code}" || true)"
  [[ -n "${profile}" ]] ||
    Die "no supported Roblox compatibility profile is available for first-run bootstrap"
  IFS=$'\t' read -r fallback_version_code fallback_version_name <<<"${profile}"
  [[ "${fallback_version_code}" =~ ^[0-9]+$ &&
     -n "${fallback_version_name}" ]] ||
    Die "supported first-run compatibility profile is invalid"
  BOOTSTRAP_SELECTED_VERSION_CODE="${fallback_version_code}"
  EXPECTED_PAYLOAD_VERSION_CODE="${fallback_version_code}"

  local bootstrap_source=""
  if [[ -e "${BOOTSTRAP_SOURCES_PATH}" ||
        -L "${BOOTSTRAP_SOURCES_PATH}" ]]; then
    [[ -f "${BOOTSTRAP_SOURCES_PATH}" &&
       ! -L "${BOOTSTRAP_SOURCES_PATH}" ]] ||
      Die "bootstrap source manifest must be a regular file"
    jq -e '.schema_version == 1 and (.sources | type == "array")' \
      "${BOOTSTRAP_SOURCES_PATH}" >/dev/null ||
      Die "bootstrap source manifest is invalid"
    bootstrap_source="$(jq -er --argjson version_code "${fallback_version_code}" '
      [.sources[]? |
       select(.version_code == $version_code and
              .provider == "uptodown")] |
      if length == 1 then .[0].provider else empty end
    ' "${BOOTSTRAP_SOURCES_PATH}" 2>/dev/null || true)"
  fi
  if [[ -n "${bootstrap_source}" ]]; then
    SOURCE="${bootstrap_source}"
  fi

  STAGED_PAYLOAD_ID=""
  local staged_selection_status=0
  FindUniqueSupportedStagedVersion "${fallback_version_code}" ||
    staged_selection_status=$?
  if (( staged_selection_status == 0 )); then
    if [[ "${remote_version_code}" == "${fallback_version_code}" ]]; then
      Log "reusing exact-supported first-run payload ${fallback_version_code}"
    elif [[ -n "${remote_version_code}" ]]; then
      Log "latest Roblox versionCode ${remote_version_code} is not supported yet; reusing supported first-run payload ${fallback_version_code}"
    else
      Log "latest-version API is unavailable; reusing supported first-run payload ${fallback_version_code}"
    fi
    return 0
  fi
  if (( staged_selection_status == 2 )); then
    Die "supported first-run payload ${fallback_version_code} is ambiguous"
  fi

  VERSION="${fallback_version_name}"
  if [[ "${remote_version_code}" == "${fallback_version_code}" ]]; then
    Log "downloading exact-supported first-run payload ${fallback_version_name} (${fallback_version_code}) from ${SOURCE}"
  elif [[ -n "${remote_version_code}" ]]; then
    Log "latest Roblox versionCode ${remote_version_code} is not supported yet; downloading supported first-run payload ${fallback_version_name} (${fallback_version_code}) from ${SOURCE}"
  else
    Log "latest-version API is unavailable; downloading supported first-run payload ${fallback_version_name} (${fallback_version_code}) from ${SOURCE}"
  fi
}

StageSupportedBootstrapFallback() {
  local ceiling="${1:-}"
  local selection status=1 selected_version
  while selection="$(LatestSupportedBootstrapProfile "${ceiling}" 2>/dev/null)" &&
        [[ -n "${selection}" ]]; do
    SelectSupportedBootstrapFallback "${ceiling}"
    selected_version="${BOOTSTRAP_SELECTED_VERSION_CODE}"
    status=0
    StagePlannedPayload || status=$?
    (( status != 0 )) || return 0
    Log "supported bootstrap ${selected_version} acquisition failed; trying the next lower exact-supported profile"
    (( 10#${selected_version} > 1 )) || return "${status}"
    ceiling="$((10#${selected_version} - 1))"
  done
  return "${status}"
}

SkipDownloadWhenCurrent() {
  local refresh_status=0
  RefreshApprovedCurrentForRuntime || refresh_status=$?
  if (( refresh_status >= 2 )); then
    Die "current payload is not runnable by this Mocktail runtime and rollback failed"
  fi
  [[ -z "${VERSION}" ]] || return 1
  local local_version remote_json remote_version
  local_version="$(LocalVersionCode || true)"
  if ! remote_json="$(CheckLatestVersion)"; then
    if [[ "${local_version}" =~ ^[0-9]+$ ]]; then
      Log "latest-version API check failed; preserving the runnable local payload"
      return 0
    fi
    Log "latest-version API check failed and no runnable local payload exists; using the compatibility bootstrap profile"
    SelectSupportedBootstrapFallback
    return 1
  fi
  remote_version="$(jq -r '.version_code // .latest_version_code // empty' <<<"${remote_json}")"
  if [[ ! "${remote_version}" =~ ^[0-9]+$ ]]; then
    if [[ "${local_version}" =~ ^[0-9]+$ ]]; then
      Log "latest-version API returned invalid metadata; preserving the runnable local payload"
      return 0
    fi
    Log "provider returned invalid version metadata and no runnable local payload exists; using the compatibility bootstrap profile"
    SelectSupportedBootstrapFallback
    return 1
  fi
  EXPECTED_PAYLOAD_VERSION_CODE="${remote_version}"
  if [[ "${remote_version}" == "${local_version}" ]]; then
    Log "Roblox versionCode ${local_version} is current; download skipped"
    return 0
  fi
  if [[ "${local_version}" =~ ^[0-9]+$ ]] &&
     (( 10#${remote_version} < 10#${local_version} )); then
    Log "latest-version API regressed from local versionCode ${local_version} to ${remote_version}; refusing a downgrade"
    return 0
  fi
  if FindStagedVersion "${remote_version}"; then
    Log "reusing staged latest Roblox versionCode ${remote_version} for probation"
    return 1
  fi
  if [[ ! "${local_version}" =~ ^[0-9]+$ ]]; then
    Log "no runnable local Roblox payload; downloading latest versionCode ${remote_version} before fallback"
    return 1
  fi
  Log "Roblox update available: versionCode ${local_version} -> ${remote_version}"
  return 1
}

SkipRemoteUpdateForStartup() {
  local refresh_status=0
  RefreshApprovedCurrentForRuntime || refresh_status=$?
  if (( refresh_status >= 2 )); then
    Die "current payload is not runnable by this Mocktail runtime and rollback failed"
  fi

  local current_payload_id
  current_payload_id="$(VerifiedCurrentPayloadId 2>/dev/null || true)"
  if [[ "${current_payload_id}" =~ ^[0-9]+-[0-9a-f]{40}$ ]]; then
    Log "startup preflight accepted runnable local payload ${current_payload_id}; remote update deferred to the background timer"
    return 0
  fi
  return 1
}

IsSupportedCandidate() {
  local -r metadata="$1"
  local build_id version_code
  build_id="$(jq -r '.elf_build_id | ascii_downcase' "${metadata}")"
  version_code="$(jq -r '.version_code' "${metadata}")"
  jq -e --arg build_id "${build_id}" --argjson version_code "${version_code}" '
    any(.profiles[]?;
      (.elf_build_id | ascii_downcase) == $build_id and
      .version_code == $version_code and
      .status == "supported" and .default_allowed == true and
      .allow_legacy_binary_patches == false)
  ' "${COMPATIBILITY_PATH}" >/dev/null
}

BuildRuntime() {
  SUPPORT_FAILURE_REASON=updater-build-failed
  [[ -z "${PACKAGED_CANARY_BINARY}" ]] || return 0
  [[ "${SKIP_BUILD}" == false ]] || return 0
  Log "building canary runtime"
  make -C "${PROJECT_ROOT}" --no-print-directory build \
    BUILD_TYPE=Release JOBS="$(nproc)"
}

ResolveCanaryRuntime() {
  local default_canary_binary="${PROJECT_ROOT}/build/mocktail"
  [[ -z "${PACKAGED_CANARY_BINARY}" ]] ||
    default_canary_binary="${PACKAGED_CANARY_BINARY}"
  CANARY_BINARY="${MOCKTAIL_UPDATE_CANARY_BIN:-${default_canary_binary}}"
  [[ -f "${CANARY_BINARY}" && ! -L "${CANARY_BINARY}" &&
     -x "${CANARY_BINARY}" ]] || {
    Log "canary binary is not an executable regular file: ${CANARY_BINARY}"
    return 1
  }
  RUNTIME_FINGERPRINT="$(sha256sum "${CANARY_BINARY}" | awk '{print $1}')"
  RUNTIME_BUILD_ID="${MOCKTAIL_UPDATE_RUNTIME_BUILD_ID:-$(
    LC_ALL=C readelf -n "${CANARY_BINARY}" 2>/dev/null |
      sed -n 's/^[[:space:]]*Build ID: \([0-9A-Fa-f]\{40\}\)$/\1/p' |
      head -n 1
  )}"
  RUNTIME_BUILD_ID="${RUNTIME_BUILD_ID,,}"
  [[ "${RUNTIME_FINGERPRINT}" =~ ^[0-9a-f]{64}$ &&
     "${RUNTIME_BUILD_ID}" =~ ^[0-9a-f]{40}$ ]]
}

ApprovalMatchesResolvedRuntime() {
  local -r approval_file="$1"
  [[ -f "${approval_file}" && ! -L "${approval_file}" &&
     "${RUNTIME_FINGERPRINT}" =~ ^[0-9a-f]{64}$ &&
     "${RUNTIME_BUILD_ID}" =~ ^[0-9a-f]{40}$ ]] || return 1
  jq -e --arg runtime_build_id "${RUNTIME_BUILD_ID}" \
    --arg runtime_sha256 "${RUNTIME_FINGERPRINT}" '
      (.runtime_build_id | type == "string") and
      (.runtime_build_id | ascii_downcase) == $runtime_build_id and
      .runtime_sha256 == $runtime_sha256 and
      .canary_runtime_sha256 == $runtime_sha256
    ' "${approval_file}" >/dev/null
}

ResolveDerivationReference() {
  REFERENCE_LIBRARY=""
  REFERENCE_PROFILE=""
  local -r reference_build_id="${MOCKTAIL_UPDATE_REFERENCE_BUILD_ID:-1686400865ae0e408cd7bd67de7a439625c6fd13}"
  local -r reference_profile="${MOCKTAIL_UPDATE_REFERENCE_PROFILE:-${DEFAULT_METADATA_ROOT}/roblox_host_abi_reference.json}"
  local metadata payload_dir build_id

  if [[ -f "${DATA_ROOT}/current.json" &&
        ! -L "${DATA_ROOT}/current.json" ]]; then
    local current_payload_path current_profile_path
    current_payload_path="$(jq -er '.payload_path |
      select(test("^payloads/[0-9]+-[0-9a-f]{40}$"))' \
      "${DATA_ROOT}/current.json" 2>/dev/null || true)"
    current_profile_path="$(jq -er '.host_abi_profile_path // empty |
      select(test("^host_abi_profiles/[0-9]+-[0-9a-f]{40}-[0-9a-f]{40}\\.json$"))' \
      "${DATA_ROOT}/current.json" 2>/dev/null || true)"
    if [[ -n "${current_payload_path}" ]]; then
      payload_dir="${DATA_ROOT}/${current_payload_path}"
      metadata="${payload_dir}/roblox_payload.json"
      build_id="$(jq -er '.elf_build_id | ascii_downcase' \
        "${metadata}" 2>/dev/null || true)"
      if [[ "${build_id}" == "${reference_build_id}" &&
            -f "${payload_dir}/libroblox.so" &&
            ! -L "${payload_dir}/libroblox.so" &&
            -f "${reference_profile}" && ! -L "${reference_profile}" ]]; then
        REFERENCE_LIBRARY="${payload_dir}/libroblox.so"
        REFERENCE_PROFILE="${reference_profile}"
        return 0
      fi
      if [[ -n "${current_profile_path}" &&
            -f "${DATA_ROOT}/${current_profile_path}" &&
            ! -L "${DATA_ROOT}/${current_profile_path}" &&
            -f "${payload_dir}/libroblox.so" &&
            ! -L "${payload_dir}/libroblox.so" ]]; then
        REFERENCE_LIBRARY="${payload_dir}/libroblox.so"
        REFERENCE_PROFILE="${DATA_ROOT}/${current_profile_path}"
        return 0
      fi
    fi
  fi

  for metadata in "${DATA_ROOT}"/payloads/*/roblox_payload.json; do
    [[ -f "${metadata}" && ! -L "${metadata}" ]] || continue
    build_id="$(jq -er '.elf_build_id | ascii_downcase' \
      "${metadata}" 2>/dev/null || true)"
    [[ "${build_id}" == "${reference_build_id}" ]] || continue
    payload_dir="${metadata%/roblox_payload.json}"
    [[ -f "${payload_dir}/libroblox.so" &&
       ! -L "${payload_dir}/libroblox.so" &&
       -f "${reference_profile}" && ! -L "${reference_profile}" ]] ||
      continue
    REFERENCE_LIBRARY="${payload_dir}/libroblox.so"
    REFERENCE_PROFILE="${reference_profile}"
    return 0
  done
  return 3
}

DeriveCandidateProfile() {
  local -r payload_dir="$1"
  local -r payload_id="$2"
  SUPPORT_FAILURE_REASON=profile-derivation-failed
  ResolveDerivationReference || return $?
  [[ -f "${PROFILE_DERIVER}" && ! -L "${PROFILE_DERIVER}" ]] || {
    Log "host-ABI profile deriver is unavailable: ${PROFILE_DERIVER}"
    return 2
  }
  [[ -n "${PROBATION_ROOT}" ]] ||
    PROBATION_ROOT="$(mktemp -d "${CACHE_ROOT}/.update-probation.${payload_id}.XXXXXX")"
  CANDIDATE_PROFILE="${PROBATION_ROOT}/host_abi_profile.json"
  CANDIDATE_COMPATIBILITY="${PROBATION_ROOT}/compatibility.json"
  local -a derive_arguments=(
    --reference-lib "${REFERENCE_LIBRARY}"
    --candidate-lib "${payload_dir}/libroblox.so"
    --payload-metadata "${payload_dir}/roblox_payload.json"
    --output "${CANDIDATE_PROFILE}"
    --compatibility-output "${CANDIDATE_COMPATIBILITY}"
  )
  [[ -n "${REFERENCE_PROFILE}" ]] || return 3
  derive_arguments+=(--reference-profile "${REFERENCE_PROFILE}")
  Progress "Checking Roblox..."
  Log "deriving exact host-ABI profile for ${payload_id}"
  if ! python3 "${PROFILE_DERIVER}" "${derive_arguments[@]}"; then
    Log "candidate ${payload_id} profile derivation was not unique"
    return 4
  fi
  [[ -f "${CANDIDATE_PROFILE}" && ! -L "${CANDIDATE_PROFILE}" &&
     -f "${CANDIDATE_COMPATIBILITY}" &&
     ! -L "${CANDIDATE_COMPATIBILITY}" ]] || return 4
  local -r payload_sha256="$(sha256sum \
    "${payload_dir}/libroblox.so" | awk '{print $1}')"
  jq -e --arg payload_id "${payload_id}" \
    --arg payload_path "payloads/${payload_id}" \
    --arg build_id "${payload_id#*-}" \
    --arg payload_sha256 "${payload_sha256}" '
      .schema_version == 1 and .payload_id == $payload_id and
      .payload_path == $payload_path and
      (.elf_build_id | ascii_downcase) == $build_id and
      .payload_sha256 == $payload_sha256 and
      (.profile | type) == "object"
    ' "${CANDIDATE_PROFILE}" >/dev/null || return 4
  jq -e --arg build_id "${payload_id#*-}" \
    --argjson version_code "${payload_id%%-*}" '
      .schema_version == 1 and (.profiles | length) == 1 and
      .profiles[0].version_code == $version_code and
      (.profiles[0].elf_build_id | ascii_downcase) == $build_id and
      .profiles[0].status == "experimental" and
      .profiles[0].default_allowed == true and
      .profiles[0].allow_legacy_binary_patches == false and
      .profiles[0].allow_host_abi_bridges == true and
      .profiles[0].allow_host_constructor_replay == true
    ' "${CANDIDATE_COMPATIBILITY}" >/dev/null || return 4
  CANDIDATE_IS_PROBATION=true
}

RunCandidateCanary() {
  SUPPORT_FAILURE_REASON=canary-failed
  local -r payload_dir="$1"
  local -r payload_id="$2"
  local -r run_index="${3:-1}"
  local -r candidate_fingerprint="${4:-}"
  local -r timeout_seconds="${MOCKTAIL_UPDATE_CANARY_TIMEOUT_S:-150}"
  [[ "${timeout_seconds}" =~ ^[1-9][0-9]{0,2}$ ]] &&
    (( 10#${timeout_seconds} <= 600 )) ||
    Die "MOCKTAIL_UPDATE_CANARY_TIMEOUT_S must be between 1 and 600"

  [[ -n "${CANARY_BINARY}" && -n "${RUNTIME_FINGERPRINT}" ]] ||
    ResolveCanaryRuntime || return 2

  CANARY_ROOT="$(mktemp -d "${CACHE_ROOT}/.update-canary.${payload_id}.${run_index}.XXXXXX")"
  local -r canary_log_root="${STATE_ROOT}/logs/update-canary/${payload_id}"
  mkdir -p "${canary_log_root}"
  local canary_log_dir
  canary_log_dir="$(mktemp -d "${canary_log_root}/run.XXXXXX")"
  mkdir -p "${CANARY_ROOT}/home" "${CANARY_ROOT}/data" \
    "${CANARY_ROOT}/cache" "${CANARY_ROOT}/state" \
    "${CANARY_ROOT}/config"

  local -a canary_environment=()
  local environment_entry environment_name
  while IFS= read -r -d '' environment_entry; do
    environment_name="${environment_entry%%=*}"
    case "${environment_name}" in
      DISPLAY|WAYLAND_DISPLAY|XDG_RUNTIME_DIR|DBUS_SESSION_BUS_ADDRESS|\
      PULSE_SERVER|PIPEWIRE_REMOTE|LANG|LANGUAGE|LC_*|TZ|USER|LOGNAME)
        canary_environment+=("${environment_entry}")
        ;;
      VK_DRIVER_FILES|VK_ICD_FILENAMES|MESA_LOADER_DRIVER_OVERRIDE|DRI_PRIME|\
      __NV_PRIME_RENDER_OFFLOAD|__VK_LAYER_NV_optimus|\
      __GLX_VENDOR_LIBRARY_NAME)
        canary_environment+=("${environment_entry}")
        ;;
      FAKE_*|EXPECTED_*|REAL_*|CANARY_MARKER|LIVE_CONFIG_ROOT)
        [[ "${MOCKTAIL_UPDATE_TEST_ENV_PASSTHROUGH:-0}" == 1 ]] &&
          canary_environment+=("${environment_entry}")
        ;;
    esac
  done < <(env -0)

  local canary_xauthority=""
  if [[ -n "${XAUTHORITY:-}" && -f "${XAUTHORITY}" &&
        ! -L "${XAUTHORITY}" ]]; then
    canary_xauthority="${CANARY_ROOT}/config/Xauthority"
    cp -- "${XAUTHORITY}" "${canary_xauthority}"
    chmod 0600 -- "${canary_xauthority}"
    canary_environment+=("XAUTHORITY=${canary_xauthority}")
  fi

  local canary_path=/usr/bin:/bin
  if [[ "${DEFAULT_METADATA_ROOT}" == "${PROJECT_ROOT}/metadata" ]]; then
    local packaged_android_tools="${PROJECT_ROOT}/runtime/android-tools/bin"
    local packaged_runtime_bin="${PROJECT_ROOT}/runtime/bin"
    if [[ -d "${packaged_android_tools}" &&
          ! -L "${packaged_android_tools}" ]]; then
      canary_path="${packaged_android_tools}:${canary_path}"
    fi
    if [[ -d "${packaged_runtime_bin}" &&
          ! -L "${packaged_runtime_bin}" ]]; then
      canary_path="${packaged_runtime_bin}:${canary_path}"
    fi
  fi

  local compatibility_manifest="${COMPATIBILITY_PATH}"
  local -a probation_environment=()
  local -a smoke_arguments=(C)
  if [[ "${CANDIDATE_IS_PROBATION}" == true ]]; then
    compatibility_manifest="${CANDIDATE_COMPATIBILITY}"
    probation_environment+=(
      MOCKTAIL_HOST_ABI_PROFILE_FILE="${CANDIDATE_PROFILE}"
      MOCKTAIL_HOST_ABI_CANARY=1
      MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI=1
    )
    smoke_arguments+=(--allow-unverified-build)
  fi

  Progress "Testing Roblox..."
  Log "running isolated real Vulkan Tier C canary ${run_index} for ${payload_id}"
  local canary_status
  set +e
  env -i "${canary_environment[@]}" \
    HOME="${CANARY_ROOT}/home" \
    XDG_DATA_HOME="${CANARY_ROOT}/data" \
    XDG_CACHE_HOME="${CANARY_ROOT}/cache" \
    XDG_STATE_HOME="${CANARY_ROOT}/state" \
    XDG_CONFIG_HOME="${CANARY_ROOT}/config" \
    PATH="${canary_path}" \
    "${probation_environment[@]}" \
    ROBLOX_LIB_PATH="${payload_dir}/libroblox.so" \
    MOCKTAIL_ASSET_ROOT="${payload_dir}/assets" \
    MOCKTAIL_ASSET_PATH="${payload_dir}/assets/content" \
    MOCKTAIL_DATA_ROOT="${CANARY_ROOT}/data" \
    MOCKTAIL_CACHE_ROOT="${CANARY_ROOT}/cache" \
    MOCKTAIL_STATE_ROOT="${CANARY_ROOT}/state" \
    MOCKTAIL_CONFIG_ROOT="${CANARY_ROOT}/config" \
    MOCKTAIL_LOG_DIR="${canary_log_dir}" \
    MOCKTAIL_BIN="${CANARY_BINARY}" \
    MOCKTAIL_COMPATIBILITY_MANIFEST="${compatibility_manifest}" \
    MOCKTAIL_SKIP_LIBROBLOX_CTORS=0 \
    MOCKTAIL_HOST_JNI_SINGLETON_SEED=0 \
    MOCKTAIL_SKIP_UPDATE_CHECK=1 \
    MOCKTAIL_ISOLATED_CANARY=1 \
    MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP=1 \
    MOCKTAIL_IGNORE_WINDOW_CLOSE=1 \
    MOCKTAIL_DISABLE_SUPPORT_BUNDLE=1 \
    MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS=5000 \
    timeout --signal=TERM --kill-after=5s "${timeout_seconds}s" \
      "${SMOKE_SCRIPT}" "${smoke_arguments[@]}" 8>&-
  canary_status=$?
  set -e
  CANARY_LOG="$(find "${canary_log_dir}" -maxdepth 1 -type f \
    -name 'tierC_*.log' -printf '%f\n' 2>/dev/null | sort | tail -n 1)"
  [[ -z "${CANARY_LOG}" ]] || CANARY_LOG="${canary_log_dir}/${CANARY_LOG}"
  rm -rf -- "${CANARY_ROOT}"
  CANARY_ROOT=""
  if (( canary_status != 0 )); then
    Log "candidate ${payload_id} failed canary with status ${canary_status}; current payload was not changed"
    SUPPORT_CANDIDATE_METADATA="${payload_dir}/roblox_payload.json"
    SUPPORT_SOURCE_LOG="${CANARY_LOG}"
    SUPPORT_FAILURE_REASON=canary-failed
    CollectUpdaterSupportBundle "${canary_status}"
    return "${canary_status}"
  fi
  SUPPORT_SOURCE_LOG="${CANARY_LOG}"
  if [[ "${CANDIDATE_IS_PROBATION}" == true ]]; then
    if [[ -z "${CANARY_LOG}" || ! -f "${CANARY_LOG}" ||
          -L "${CANARY_LOG}" || ! -s "${CANARY_LOG}" ]]; then
      SUPPORT_FAILURE_REASON=canary-readiness-log-missing
      Log "candidate ${payload_id} returned success without a readiness log"
      CollectUpdaterSupportBundle 5
      return 5
    fi
    local -r log_sha256="$(sha256sum \
      "${CANARY_LOG}" | awk '{print $1}')"
    local -r profile_sha256="$(sha256sum \
      "${CANDIDATE_PROFILE}" | awk '{print $1}')"
    local -r compatibility_sha256="$(sha256sum \
      "${CANDIDATE_COMPATIBILITY}" | awk '{print $1}')"
    local attestation_path
    attestation_path="${PROBATION_ROOT}/canary-${run_index}.json"
    jq -n --argjson run "${run_index}" \
      --arg run_id "${payload_id}-${run_index}-$(date +%s)-$$" \
      --arg payload_id "${payload_id}" \
      --arg payload_fingerprint "${candidate_fingerprint}" \
      --arg profile_sha256 "${profile_sha256}" \
      --arg compatibility_sha256 "${compatibility_sha256}" \
      --arg runtime_sha256 "${RUNTIME_FINGERPRINT}" \
      --arg runtime_build_id "${RUNTIME_BUILD_ID}" \
      --arg readiness_log_sha256 "${log_sha256}" \
      '{schema_version:1,status:"passed",canary_tier:"C",run:$run,
        run_id:$run_id,payload_id:$payload_id,
        payload_fingerprint:$payload_fingerprint,
        profile_sha256:$profile_sha256,
        compatibility_manifest_sha256:$compatibility_sha256,
        runtime_sha256:$runtime_sha256,
        runtime_build_id:$runtime_build_id,
        readiness_log_sha256:$readiness_log_sha256}' > "${attestation_path}"
    chmod 0400 -- "${attestation_path}"
    CANARY_ATTESTATIONS+=("${attestation_path}")
  fi
  CANARY_LOG=""
}

RunCandidateCanaries() {
  local -r payload_dir="$1"
  local -r payload_id="$2"
  local -r candidate_fingerprint="$3"
  CANARY_ATTESTATIONS=()
  local canary_runs=1 run_index
  [[ "${CANDIDATE_IS_PROBATION}" == false ]] || canary_runs=2
  for ((run_index = 1; run_index <= canary_runs; ++run_index)); do
    RunCandidateCanary "${payload_dir}" "${payload_id}" "${run_index}" \
      "${candidate_fingerprint}" || return $?
  done
}

LaunchCurrent() {
  SUPPORT_FAILURE_REASON=updater-launch-failed
  local current_payload_id
  current_payload_id="$(CurrentPayloadId)"
  [[ "${current_payload_id}" != none ]] || Die "no current payload is available"
  local payload_path
  payload_path="$(jq -er '.payload_path | select(type == "string")' \
    "${DATA_ROOT}/current.json")" || Die "current payload manifest is missing"
  [[ "${payload_path}" =~ ^payloads/[0-9]+-[0-9a-f]{40}$ ]] ||
    Die "current payload path is invalid"
  local -r payload_dir="${DATA_ROOT}/${payload_path}"
  local -a approval_environment=()
  local approval_path profile_path compatibility_path
  approval_path="$(jq -r '.approval_path // empty' \
    "${DATA_ROOT}/current.json")"
  profile_path="$(jq -r '.host_abi_profile_path // empty' \
    "${DATA_ROOT}/current.json")"
  compatibility_path="$(jq -r '.compatibility_manifest_path // empty' \
    "${DATA_ROOT}/current.json")"
  if [[ -n "${approval_path}${profile_path}${compatibility_path}" ]]; then
    local approval_generation=""
    if [[ "${approval_path}" =~ ^approvals/${current_payload_id}-([0-9a-f]{40})\.json$ ]]; then
      approval_generation="${BASH_REMATCH[1]}"
    fi
    [[ -n "${approval_generation}" &&
       "${profile_path}" == \
         "host_abi_profiles/${current_payload_id}-${approval_generation}.json" &&
       "${compatibility_path}" == \
         "compatibility_profiles/${current_payload_id}-${approval_generation}.json" ]] ||
      Die "current probation approval paths are invalid"
    local approval_file profile_file compatibility_file
    approval_file="${DATA_ROOT}/${approval_path}"
    profile_file="${DATA_ROOT}/${profile_path}"
    compatibility_file="${DATA_ROOT}/${compatibility_path}"
    local approval_artifact
    for approval_artifact in "${approval_file}" "${profile_file}" \
        "${compatibility_file}"; do
      [[ -f "${approval_artifact}" && ! -L "${approval_artifact}" ]] ||
        Die "current probation approval artifact is invalid"
    done
    ResolveCanaryRuntime || Die "cannot identify the launch runtime"
    ApprovalMatchesResolvedRuntime "${approval_file}" ||
      Die "current probation approval belongs to a different Mocktail runtime"
    approval_environment+=(
      MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT="${approval_file}"
      MOCKTAIL_HOST_ABI_PROFILE_FILE="${profile_file}"
      MOCKTAIL_COMPATIBILITY_MANIFEST="${compatibility_file}"
    )
  fi
  Log "launching current payload from ${payload_dir}"
  Cleanup
  ReleaseUpdaterLock
  exec env -u MOCKTAIL_HOST_ABI_CANARY \
    -u MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI \
    -u MOCKTAIL_HOST_ABI_PROFILE_FILE \
    -u MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT \
    -u MOCKTAIL_COMPATIBILITY_MANIFEST \
    -u MOCKTAIL_HOST_ALLOCATOR_BRIDGES \
    -u MOCKTAIL_PATCH_HOSTILE_CANARY \
    -u MOCKTAIL_SKIP_LIBROBLOX_CTORS \
    -u MOCKTAIL_HOST_JNI_SINGLETON_SEED \
    -u MOCKTAIL_ROBLOX_COOKIES \
    -u BASH_ENV -u ENV -u LD_PRELOAD -u LD_AUDIT \
    -u RIPGREP_CONFIG_PATH \
    "${approval_environment[@]}" \
    ROBLOX_LIB_PATH="${payload_dir}/libroblox.so" \
    MOCKTAIL_ASSET_ROOT="${payload_dir}/assets" \
    MOCKTAIL_ASSET_PATH="${payload_dir}/assets/content" \
    MOCKTAIL_SKIP_UPDATE_CHECK=1 \
    MOCKTAIL_SKIP_LIBROBLOX_CTORS=0 \
    MOCKTAIL_HOST_JNI_SINGLETON_SEED=0 \
    MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS=0 \
    "${SMOKE_SCRIPT}" C
}

ResetCandidateProbation() {
  if [[ -n "${PROBATION_ROOT}" ]]; then
    rm -rf -- "${PROBATION_ROOT}"
  fi
  PROBATION_ROOT=""
  CANDIDATE_PROFILE=""
  CANDIDATE_COMPATIBILITY=""
  CANDIDATE_IS_PROBATION=false
  CANARY_ATTESTATIONS=()
}

BuildCandidateRejectionContext() {
  local -r payload_id="$1"
  local -r payload_fingerprint="$2"
  CANDIDATE_REJECTION_PROFILE_SHA256="$(sha256sum \
    "${CANDIDATE_PROFILE}" | awk '{print $1}')"
  CANDIDATE_REJECTION_COMPATIBILITY_SHA256="$(sha256sum \
    "${CANDIDATE_COMPATIBILITY}" | awk '{print $1}')"
  CANDIDATE_REJECTION_REFERENCE_SHA256="$(jq -Sc '.reference' \
    "${CANDIDATE_PROFILE}" | sha256sum | awk '{print $1}')"
  CANDIDATE_REJECTION_DERIVER_SHA256="$(sha256sum \
    "${PROFILE_DERIVER}" | awk '{print $1}')"
  CANDIDATE_REJECTION_SCHEMA_VERSION="$(jq -er '.schema_version |
    select(type == "number" and floor == . and . >= 1)' \
    "${CANDIDATE_PROFILE}")"
  CANDIDATE_REJECTION_KEY="$(printf '%s\n' \
      "payload_id=${payload_id}" \
      "payload_fingerprint=${payload_fingerprint}" \
      "profile=${CANDIDATE_REJECTION_PROFILE_SHA256}" \
      "compatibility=${CANDIDATE_REJECTION_COMPATIBILITY_SHA256}" \
      "runtime_build_id=${RUNTIME_BUILD_ID}" \
      "runtime=${RUNTIME_FINGERPRINT}" \
      "reference=${CANDIDATE_REJECTION_REFERENCE_SHA256}" \
      "deriver=${CANDIDATE_REJECTION_DERIVER_SHA256}" \
      "schema=${CANDIDATE_REJECTION_SCHEMA_VERSION}" |
    sha256sum | awk '{print $1}')"
  [[ "${CANDIDATE_REJECTION_KEY}" =~ ^[0-9a-f]{64}$ ]]
}

CandidateRejectionMemoPath() {
  printf '%s/rejected_candidates/%s.json\n' "${STATE_ROOT}" "$1"
}

CandidateRejectionIsActive() {
  local -r payload_id="$1"
  [[ "${MOCKTAIL_UPDATE_RETRY_REJECTED:-0}" != 1 ]] || return 1
  local -r memo_path="$(CandidateRejectionMemoPath "${payload_id}")"
  [[ -f "${memo_path}" && ! -L "${memo_path}" ]] || return 1
  local -r now="$(date +%s)"
  local rejected_at
  rejected_at="$(jq -er --arg payload_id "${payload_id}" \
    --arg key "${CANDIDATE_REJECTION_KEY}" '
      select(.schema_version == 1 and .payload_id == $payload_id and
        .rejection_key == $key) |
      .rejected_at_epoch |
      select(type == "number" and floor == . and . >= 0)
    ' "${memo_path}" 2>/dev/null)" || return 1
  local ttl_seconds="${MOCKTAIL_UPDATE_REJECTION_BACKOFF_S:-21600}"
  [[ "${ttl_seconds}" =~ ^[1-9][0-9]*$ ]] || return 1
  (( 10#${ttl_seconds} <= 604800 &&
     10#${rejected_at} <= 10#${now} &&
     10#${now} - 10#${rejected_at} < 10#${ttl_seconds} ))
}

RecordCandidateRejection() {
  local -r payload_id="$1"
  local -r canary_status="$2"
  local -r memo_path="$(CandidateRejectionMemoPath "${payload_id}")"
  mkdir -p -- "${memo_path%/*}"
  local temporary
  temporary="$(mktemp "${memo_path%/*}/.rejection.XXXXXX")"
  if ! jq -n \
      --arg payload_id "${payload_id}" \
      --arg rejection_key "${CANDIDATE_REJECTION_KEY}" \
      --arg profile_sha256 "${CANDIDATE_REJECTION_PROFILE_SHA256}" \
      --arg compatibility_sha256 \
        "${CANDIDATE_REJECTION_COMPATIBILITY_SHA256}" \
      --arg runtime_build_id "${RUNTIME_BUILD_ID}" \
      --arg runtime_sha256 "${RUNTIME_FINGERPRINT}" \
      --arg reference_sha256 "${CANDIDATE_REJECTION_REFERENCE_SHA256}" \
      --arg deriver_sha256 "${CANDIDATE_REJECTION_DERIVER_SHA256}" \
      --argjson analyzer_schema_version \
        "${CANDIDATE_REJECTION_SCHEMA_VERSION}" \
      --argjson canary_status "${canary_status}" \
      --argjson rejected_at_epoch "$(date +%s)" '
        {schema_version:1,payload_id:$payload_id,
         rejection_key:$rejection_key,profile_sha256:$profile_sha256,
         compatibility_manifest_sha256:$compatibility_sha256,
         runtime_build_id:$runtime_build_id,runtime_sha256:$runtime_sha256,
         reference_profile_sha256:$reference_sha256,
         deriver_sha256:$deriver_sha256,
         analyzer_schema_version:$analyzer_schema_version,
         canary_status:$canary_status,rejected_at_epoch:$rejected_at_epoch}
      ' > "${temporary}" ||
     ! chmod 0600 -- "${temporary}" ||
     ! mv -f -- "${temporary}" "${memo_path}"; then
    rm -f -- "${temporary}"
    Log "warning: failed to persist candidate rejection backoff" || true
  fi
}

ClearCandidateRejection() {
  local -r payload_id="$1"
  rm -f -- "$(CandidateRejectionMemoPath "${payload_id}")"
}

StagePlannedPayload() {
  SELECTED_PAYLOAD_ID=""
  SELECTED_PAYLOAD_DIR=""
  SELECTED_PAYLOAD_METADATA=""
  if [[ -n "${STAGED_PAYLOAD_ID}" ]]; then
    Progress "Preparing Roblox..."
    SELECTED_PAYLOAD_ID="${STAGED_PAYLOAD_ID}"
    STAGED_PAYLOAD_ID=""
  else
    local -a fetch_arguments=(
      --source "${SOURCE}"
      --staging-root "${DOWNLOAD_STAGING_ROOT}"
    )
    [[ -z "${VERSION}" ]] || fetch_arguments+=(--version "${VERSION}")
    SUPPORT_FAILURE_REASON=updater-download-failed
    Progress "Downloading Roblox..."
    if [[ -n "${VERSION}" ]]; then
      Log "downloading pinned provider payload ${VERSION}"
    else
      Log "downloading latest provider payload"
    fi
    local bundle_dir fetch_status=0
    bundle_dir="$("${FETCH_SCRIPT}" "${fetch_arguments[@]}")" ||
      fetch_status=$?
    if (( fetch_status != 0 )); then
      Log "provider download failed"
      return "${fetch_status}"
    fi
    DOWNLOADED_BUNDLE_DIR="${bundle_dir}"
    SUPPORT_FAILURE_REASON=updater-validation-failed
    Progress "Installing Roblox..."
    Log "validating downloaded x86_64 payload"
    local validator_output validator_status=0
    validator_output="$("${VALIDATOR_SCRIPT}" \
        --base "${bundle_dir}/base.apk" \
        --x86-64 "${bundle_dir}/split_config.x86_64.apk" \
        --source "direct:${SOURCE}" --store-root "${DATA_ROOT}")" ||
      validator_status=$?
    if (( validator_status != 0 )); then
      CleanupDownloadedBundle
      Log "downloaded payload validation failed"
      return "${validator_status}"
    fi
    SELECTED_PAYLOAD_ID="$(tail -n 1 <<<"${validator_output}")"
    CleanupDownloadedBundle
  fi
  [[ "${SELECTED_PAYLOAD_ID}" =~ ^[0-9]+-[0-9a-f]{40}$ ]] || {
    Log "validator returned an invalid payload ID: ${SELECTED_PAYLOAD_ID}"
    return 21
  }
  SELECTED_PAYLOAD_DIR="${DATA_ROOT}/payloads/${SELECTED_PAYLOAD_ID}"
  SELECTED_PAYLOAD_METADATA="${SELECTED_PAYLOAD_DIR}/roblox_payload.json"
  [[ -f "${SELECTED_PAYLOAD_METADATA}" &&
     ! -L "${SELECTED_PAYLOAD_METADATA}" ]] || return 21
  local selected_version_code metadata_version_code
  selected_version_code="${SELECTED_PAYLOAD_ID%%-*}"
  metadata_version_code="$(jq -er '.version_code |
    select(type == "number" and floor == . and . >= 0) | tostring' \
    "${SELECTED_PAYLOAD_METADATA}" 2>/dev/null || true)"
  if [[ -z "${metadata_version_code}" ||
        "${selected_version_code}" != "${metadata_version_code}" ]]; then
    Log "validated payload metadata does not match its immutable payload ID"
    return 22
  fi
  if [[ -n "${EXPECTED_PAYLOAD_VERSION_CODE}" &&
        "${metadata_version_code}" != "${EXPECTED_PAYLOAD_VERSION_CODE}" ]]; then
    Log "provider payload versionCode ${metadata_version_code} does not match expected ${EXPECTED_PAYLOAD_VERSION_CODE}; refusing probation"
    return 22
  fi
  SUPPORT_CANDIDATE_METADATA="${SELECTED_PAYLOAD_METADATA}"
}

PromoteCanaryApprovedCandidate() {
  local -r payload_id="$1"
  local -r candidate_fingerprint="$2"
  local -r baseline_payload_id="$3"
  local -a store_arguments=(
    --root "${DATA_ROOT}"
    --compatibility "${COMPATIBILITY_PATH}"
    --expected-current "${baseline_payload_id}"
    --expected-payload-fingerprint "${candidate_fingerprint}"
  )
  local store_command=promote
  if [[ "${CANDIDATE_IS_PROBATION}" == true ]]; then
    (( ${#CANARY_ATTESTATIONS[@]} == 2 )) || return 6
    store_arguments+=(
      --candidate-profile "${CANDIDATE_PROFILE}"
      --candidate-compatibility "${CANDIDATE_COMPATIBILITY}"
      --runtime-fingerprint "${RUNTIME_FINGERPRINT}"
      --runtime-build-id "${RUNTIME_BUILD_ID}"
      --canary-attestation "${CANARY_ATTESTATIONS[0]}"
      --canary-attestation "${CANARY_ATTESTATIONS[1]}"
    )
    store_command=promote-probation
  fi

  SUPPORT_FAILURE_REASON=updater-promotion-failed
  Progress "Installing Roblox..."
  Log "promoting canary-approved candidate"
  if "${PAYLOAD_STORE_SCRIPT}" "${store_arguments[@]}" \
      "${store_command}" "${payload_id}"; then
    return 0
  fi

  local actual_payload_id
  actual_payload_id="$(CurrentPayloadId)"
  if [[ "${actual_payload_id}" == "${payload_id}" &&
        "${baseline_payload_id}" != none ]]; then
    Log "promotion reported failure after switching current; rolling back to ${baseline_payload_id}"
    if ! "${PAYLOAD_STORE_SCRIPT}" --root "${DATA_ROOT}" \
        --compatibility "${COMPATIBILITY_PATH}" \
        --expected-current "${payload_id}" \
        --expected-rollback-target "${baseline_payload_id}" rollback; then
      Log "promotion and rollback to ${baseline_payload_id} both failed"
      return 8
    fi
    actual_payload_id="$(CurrentPayloadId)"
  fi
  if [[ "${actual_payload_id}" == "${payload_id}" &&
        "${baseline_payload_id}" == none ]]; then
    local committed_payload_path committed_fingerprint verified_payload_id
    committed_payload_path="$(jq -er '.payload_path |
      select(type == "string")' "${DATA_ROOT}/current.json" 2>/dev/null ||
      true)"
    committed_fingerprint="$("${PAYLOAD_STORE_SCRIPT}" \
      --root "${DATA_ROOT}" --compatibility "${COMPATIBILITY_PATH}" \
      fingerprint "${payload_id}" 2>/dev/null || true)"
    verified_payload_id="$(VerifiedCurrentPayloadId 2>/dev/null || true)"
    [[ "${committed_payload_path}" == "payloads/${payload_id}" &&
       "${committed_fingerprint}" == "${candidate_fingerprint}" &&
       "${verified_payload_id}" == "${payload_id}" ]] ||
      return 8
    Log "initial promotion committed despite a nonzero store status" || true
    return 0
  fi
  [[ "${actual_payload_id}" == "${baseline_payload_id}" ]] || return 8
  Log "candidate ${payload_id} was not promoted; current payload ${baseline_payload_id} is preserved"
  return 7
}

AttemptCandidate() {
  local -r payload_id="$1"
  local -r payload_dir="${DATA_ROOT}/payloads/${payload_id}"
  local -r metadata="${payload_dir}/roblox_payload.json"
  ResetCandidateProbation
  CANDIDATE_NEEDS_REFERENCE=false
  CANDIDATE_REJECTION_DEFERRED=false
  SUPPORT_CANDIDATE_METADATA="${metadata}"
  local baseline_payload_id
  baseline_payload_id="$(CurrentPayloadId)"
  LAST_ATTEMPT_BASELINE_PAYLOAD_ID="${baseline_payload_id}"

  if ! IsSupportedCandidate "${metadata}"; then
    local derive_status=0
    DeriveCandidateProfile "${payload_dir}" "${payload_id}" ||
      derive_status=$?
    if (( derive_status != 0 )); then
      [[ "${derive_status}" != 3 ]] || CANDIDATE_NEEDS_REFERENCE=true
      return "${derive_status}"
    fi
  fi

  BuildRuntime || return 22
  ResolveCanaryRuntime || return 22
  SUPPORT_FAILURE_REASON=updater-fingerprint-failed
  Log "fingerprinting candidate payload"
  local candidate_fingerprint
  if ! candidate_fingerprint="$("${PAYLOAD_STORE_SCRIPT}" \
      --root "${DATA_ROOT}" --compatibility "${COMPATIBILITY_PATH}" \
      fingerprint "${payload_id}")"; then
    return 23
  fi
  [[ "${candidate_fingerprint}" =~ ^[0-9a-f]{64}$ ]] || return 23
  if [[ "${CANDIDATE_IS_PROBATION}" == true ]]; then
    BuildCandidateRejectionContext "${payload_id}" \
      "${candidate_fingerprint}" || return 23
    if CandidateRejectionIsActive "${payload_id}"; then
      Log "candidate ${payload_id} matches a recent failed canary; retry deferred for up to 6 hours"
      CANDIDATE_REJECTION_DEFERRED=true
      return 24
    fi
  fi
  local canary_status=0
  RunCandidateCanaries "${payload_dir}" "${payload_id}" \
    "${candidate_fingerprint}" || canary_status=$?
  if (( canary_status != 0 )); then
    [[ "${CANDIDATE_IS_PROBATION}" != true ]] ||
      RecordCandidateRejection "${payload_id}" "${canary_status}"
    return "${canary_status}"
  fi
  PromoteCanaryApprovedCandidate "${payload_id}" "${candidate_fingerprint}" \
    "${baseline_payload_id}" || return $?
  ClearCandidateRejection "${payload_id}"
  Log "candidate ${payload_id} passed readiness and is now current" || true
}

PreserveCurrentAfterUpdateFailure() {
  local -r reason="$1"
  local -r expected_payload_id="${2:-none}"
  local actual_payload_id
  actual_payload_id="$(CurrentPayloadId)"
  if [[ "${actual_payload_id}" != "${expected_payload_id}" ]]; then
    Log "${reason}; current payload is uncertain: expected ${expected_payload_id}, found ${actual_payload_id}"
    return 2
  fi
  if [[ "${expected_payload_id}" != none ]]; then
    CollectUpdaterSupportBundle 1
    Log "${reason}; keeping the previous working payload"
    if [[ "${LAUNCH}" == true ]]; then
      BuildRuntime || return $?
      LaunchCurrent
    fi
    return 0
  fi
  return 1
}

RollbackAfterApprovalRefreshFailure() {
  local -r current_payload_id="${1:-}"
  local -r previous_manifest="${DATA_ROOT}/previous_good.json"
  local previous_payload_id="" previous_is_runnable=false
  if [[ -f "${previous_manifest}" && ! -L "${previous_manifest}" ]]; then
    previous_payload_id="$(jq -er '.payload_id |
      select(type == "string" and test("^[0-9]+-[0-9a-f]{40}$"))' \
      "${previous_manifest}" 2>/dev/null || true)"
  fi
  if [[ -n "${previous_payload_id}" ]]; then
    local -r previous_metadata="${DATA_ROOT}/payloads/${previous_payload_id}/roblox_payload.json"
    if IsSupportedCandidate "${previous_metadata}"; then
      previous_is_runnable=true
    elif BuildRuntime && ResolveCanaryRuntime; then
      local previous_approval_path previous_generation=""
      previous_approval_path="$(jq -r '.approval_path // empty' \
        "${previous_manifest}" 2>/dev/null || true)"
      if [[ "${previous_approval_path}" =~ ^approvals/${previous_payload_id}-([0-9a-f]{40})\.json$ ]]; then
        previous_generation="${BASH_REMATCH[1]}"
      fi
      local previous_approval_file="${DATA_ROOT}/${previous_approval_path}"
      if [[ -n "${previous_generation}" ]] &&
         ApprovalMatchesResolvedRuntime "${previous_approval_file}"; then
        previous_is_runnable=true
      fi
    fi
  fi

  if [[ "${previous_is_runnable}" == true ]]; then
    Log "rolling back to validated ${previous_payload_id} after approval refresh failed"
    local -a rollback_arguments=(
      --root "${DATA_ROOT}"
      --compatibility "${COMPATIBILITY_PATH}"
      --expected-rollback-target "${previous_payload_id}"
    )
    if [[ "${current_payload_id}" =~ ^[0-9]+-[0-9a-f]{40}$ ]]; then
      rollback_arguments+=(--expected-current "${current_payload_id}")
    fi
    if "${PAYLOAD_STORE_SCRIPT}" "${rollback_arguments[@]}" rollback &&
       [[ "$(VerifiedCurrentPayloadId 2>/dev/null || true)" == \
          "${previous_payload_id}" ]]; then
      return 0
    fi
  fi

  [[ "${current_payload_id}" =~ ^[0-9]+-[0-9a-f]{40}$ ]] || return 1
  local -r current_version_code="${current_payload_id%%-*}"
  STAGED_PAYLOAD_ID=""
  local fallback_selection_status=0
  FindNewestSupportedStagedCandidate 0 "${current_version_code}" ||
    fallback_selection_status=$?
  (( fallback_selection_status == 0 )) || return 1
  local -r fallback_payload_id="${STAGED_PAYLOAD_ID}"
  [[ "${fallback_payload_id}" != "${current_payload_id}" ]] || return 1

  SUPPORT_FAILURE_REASON=updater-recovery-failed
  local fallback_fingerprint
  fallback_fingerprint="$("${PAYLOAD_STORE_SCRIPT}" \
    --root "${DATA_ROOT}" --compatibility "${COMPATIBILITY_PATH}" \
    fingerprint "${fallback_payload_id}")" || return 1
  [[ "${fallback_fingerprint}" =~ ^[0-9a-f]{64}$ ]] || return 1
  Log "activating exact-supported local fallback ${fallback_payload_id} after approval refresh failed"
  "${PAYLOAD_STORE_SCRIPT}" --root "${DATA_ROOT}" \
    --compatibility "${COMPATIBILITY_PATH}" \
    --expected-current "${current_payload_id}" \
    --expected-payload-fingerprint "${fallback_fingerprint}" \
    promote "${fallback_payload_id}" || return 1
  [[ "$(VerifiedCurrentPayloadId 2>/dev/null || true)" == \
     "${fallback_payload_id}" ]]
}

RefreshApprovedCurrentForRuntime() {
  if [[ ! -e "${DATA_ROOT}/current.json" &&
        ! -L "${DATA_ROOT}/current.json" ]]; then
    return 0
  fi
  local current_payload_id current_payload_path approval_path
  local profile_path compatibility_path
  if ! current_payload_id="$(VerifiedCurrentPayloadId)"; then
    Log "current payload failed integrity or approval verification"
    RollbackAfterApprovalRefreshFailure || return 2
    return 1
  fi
  [[ "${current_payload_id}" != none ]] || return 0
  current_payload_path="$(jq -er '.payload_path' \
    "${DATA_ROOT}/current.json" 2>/dev/null || true)"
  [[ "${current_payload_path}" == "payloads/${current_payload_id}" ]] ||
    return 2
  local -r metadata="${DATA_ROOT}/${current_payload_path}/roblox_payload.json"
  if IsSupportedCandidate "${metadata}"; then
    if jq -e 'has("approval_path") or has("host_abi_profile_path") or
        has("compatibility_manifest_path")' \
        "${DATA_ROOT}/current.json" >/dev/null; then
      Log "removing obsolete probation references from supported current payload"
      "${PAYLOAD_STORE_SCRIPT}" --root "${DATA_ROOT}" \
        --compatibility "${COMPATIBILITY_PATH}" \
        --expected-current "${current_payload_id}" \
        promote "${current_payload_id}" || return 2
      [[ "$(VerifiedCurrentPayloadId 2>/dev/null || true)" == \
         "${current_payload_id}" ]] || return 2
    fi
    return 0
  fi
  approval_path="$(jq -r '.approval_path // empty' \
    "${DATA_ROOT}/current.json")"
  profile_path="$(jq -r '.host_abi_profile_path // empty' \
    "${DATA_ROOT}/current.json")"
  compatibility_path="$(jq -r '.compatibility_manifest_path // empty' \
    "${DATA_ROOT}/current.json")"
  local approval_generation=""
  if [[ "${approval_path}" =~ ^approvals/${current_payload_id}-([0-9a-f]{40})\.json$ ]]; then
    approval_generation="${BASH_REMATCH[1]}"
  fi
  [[ -n "${approval_generation}" &&
     "${profile_path}" == \
       "host_abi_profiles/${current_payload_id}-${approval_generation}.json" &&
     "${compatibility_path}" == \
       "compatibility_profiles/${current_payload_id}-${approval_generation}.json" ]] ||
    return 2
  local -r approval_file="${DATA_ROOT}/${approval_path}"
  local -r persisted_profile="${DATA_ROOT}/${profile_path}"
  local -r persisted_compatibility="${DATA_ROOT}/${compatibility_path}"
  local approval_artifact
  for approval_artifact in "${approval_file}" "${persisted_profile}" \
      "${persisted_compatibility}"; do
    [[ -f "${approval_artifact}" && ! -L "${approval_artifact}" ]] ||
      return 2
  done

  BuildRuntime || return 2
  ResolveCanaryRuntime || return 2
  ApprovalMatchesResolvedRuntime "${approval_file}" && return 0

  Log "runtime identity changed; re-canarying approved payload ${current_payload_id}"
  ResetCandidateProbation
  PROBATION_ROOT="$(mktemp -d \
    "${CACHE_ROOT}/.update-probation.${current_payload_id}.XXXXXX")"
  CANDIDATE_PROFILE="${PROBATION_ROOT}/host_abi_profile.json"
  CANDIDATE_COMPATIBILITY="${PROBATION_ROOT}/compatibility.json"
  cp -- "${persisted_profile}" "${CANDIDATE_PROFILE}"
  cp -- "${persisted_compatibility}" "${CANDIDATE_COMPATIBILITY}"
  CANDIDATE_IS_PROBATION=true
  SUPPORT_CANDIDATE_METADATA="${metadata}"
  SUPPORT_FAILURE_REASON=updater-fingerprint-failed
  local candidate_fingerprint
  if ! candidate_fingerprint="$("${PAYLOAD_STORE_SCRIPT}" \
      --root "${DATA_ROOT}" --compatibility "${COMPATIBILITY_PATH}" \
      fingerprint "${current_payload_id}")" ||
     [[ ! "${candidate_fingerprint}" =~ ^[0-9a-f]{64}$ ]]; then
    RollbackAfterApprovalRefreshFailure "${current_payload_id}" || return 2
    return 1
  fi
  BuildCandidateRejectionContext "${current_payload_id}" \
    "${candidate_fingerprint}" || return 2
  if CandidateRejectionIsActive "${current_payload_id}"; then
    Log "runtime reapproval matches a recent failed canary; rolling back without retry"
    RollbackAfterApprovalRefreshFailure "${current_payload_id}" || return 2
    return 1
  fi
  local canary_status=0
  RunCandidateCanaries "${DATA_ROOT}/${current_payload_path}" \
    "${current_payload_id}" "${candidate_fingerprint}" ||
    canary_status=$?
  if (( canary_status != 0 )); then
    RecordCandidateRejection "${current_payload_id}" "${canary_status}"
    RollbackAfterApprovalRefreshFailure "${current_payload_id}" || return 2
    return 1
  fi
  if ! PromoteCanaryApprovedCandidate "${current_payload_id}" \
      "${candidate_fingerprint}" "${current_payload_id}"; then
    RollbackAfterApprovalRefreshFailure "${current_payload_id}" || return 2
    return 1
  fi
  ClearCandidateRejection "${current_payload_id}"
  [[ "$(VerifiedCurrentPayloadId 2>/dev/null || true)" == \
     "${current_payload_id}" ]] || return 2
  Log "approval receipt refreshed for runtime Build ID ${RUNTIME_BUILD_ID}"
}

Main() {
  SUPPORT_FAILURE_REASON=updater-dependency-failed
  command -v jq >/dev/null 2>&1 || Die "jq is required"
  command -v timeout >/dev/null 2>&1 || Die "GNU timeout is required"
  command -v flock >/dev/null 2>&1 || Die "flock is required"
  command -v readelf >/dev/null 2>&1 || Die "readelf is required"
  SUPPORT_FAILURE_REASON=updater-config-failed
  LoadYamlDefaults
  ParseArguments "$@"
  [[ "${SOURCE}" == apk-pure ]] || Die "update source must be apk-pure"
  if [[ "${SCHEDULED}" == true &&
        "$(jq -r 'if has("automatic") then .automatic else true end' \
          <<<"${UPDATE_CONFIG_JSON}")" != true ]]; then
    Log "scheduled update is disabled by ${CONFIG_FILE}"
    return 0
  fi
  SUPPORT_FAILURE_REASON=updater-state-failed
  mkdir -p "${CACHE_ROOT}/downloads" "${DATA_ROOT}" \
    "${STATE_ROOT}/logs/updater"
  UPDATER_LOG="$(mktemp "${STATE_ROOT}/logs/updater/update.XXXXXX.log")"
  chmod 0600 -- "${UPDATER_LOG}"
  AcquireUpdaterLock
  if [[ "${STARTUP_PREFLIGHT}" == true ]] && SkipRemoteUpdateForStartup; then
    return 0
  fi

  SUPPORT_FAILURE_REASON=updater-version-check-failed
  if SkipDownloadWhenCurrent; then
    Cleanup
    if [[ "${LAUNCH}" == true && -f "${DATA_ROOT}/current.json" ]]; then
      BuildRuntime
      LaunchCurrent
    fi
    return 0
  fi

  local pre_stage_payload_id
  pre_stage_payload_id="$(CurrentPayloadId)"
  local stage_status=0
  StagePlannedPayload || stage_status=$?
  if (( stage_status != 0 )); then
    [[ -z "${VERSION}" ]] || return "${stage_status}"
    PreserveCurrentAfterUpdateFailure "latest payload acquisition failed" \
      "${pre_stage_payload_id}" &&
      return 0
    local bootstrap_ceiling=""
    if [[ "${BOOTSTRAP_SELECTED_VERSION_CODE}" =~ ^[0-9]+$ ]]; then
      (( 10#${BOOTSTRAP_SELECTED_VERSION_CODE} > 1 )) ||
        return "${stage_status}"
      bootstrap_ceiling="$((10#${BOOTSTRAP_SELECTED_VERSION_CODE} - 1))"
    fi
    stage_status=0
    StageSupportedBootstrapFallback "${bootstrap_ceiling}" || stage_status=$?
    (( stage_status == 0 )) || return "${stage_status}"
  fi
  local latest_payload_id="${SELECTED_PAYLOAD_ID}"
  LATEST_STAGED_PAYLOAD_ID="${latest_payload_id}"
  local attempt_status=0
  AttemptCandidate "${latest_payload_id}" || attempt_status=$?
  local -r latest_needs_reference="${CANDIDATE_NEEDS_REFERENCE}"
  local -r latest_rejection_deferred="${CANDIDATE_REJECTION_DEFERRED}"
  if (( attempt_status == 0 )); then
    [[ "${LAUNCH}" == false ]] || LaunchCurrent
    return 0
  fi
  # A configured version is an explicit request, not a preference. Do not
  # replace it with a newer or older bootstrap candidate when it cannot run.
  [[ -z "${VERSION}" ]] || return "${attempt_status}"

  local had_current=false
  [[ -f "${DATA_ROOT}/current.json" &&
     ! -L "${DATA_ROOT}/current.json" ]] && had_current=true
  if [[ "${latest_rejection_deferred}" == true &&
        "${had_current}" == true ]]; then
    Log "recently rejected candidate remains staged; keeping the current payload"
    if [[ "${LAUNCH}" == true ]]; then
      BuildRuntime || return $?
      LaunchCurrent
    fi
    return 0
  fi
  if [[ "${latest_needs_reference}" != true && "${had_current}" == true ]]; then
    PreserveCurrentAfterUpdateFailure \
      "latest candidate ${latest_payload_id} failed probation" \
      "${LAST_ATTEMPT_BASELINE_PAYLOAD_ID}"
    return 0
  fi

  local candidate_version="${latest_payload_id%%-*}"
  local fallback_ceiling=""
  if [[ "${candidate_version}" =~ ^[0-9]+$ ]] &&
     (( 10#${candidate_version} > 1 )); then
    fallback_ceiling="$((10#${candidate_version} - 1))"
  fi
  if ! LatestSupportedBootstrapProfile "${fallback_ceiling}" \
      >/dev/null 2>&1; then
    return "${attempt_status}"
  fi
  Log "installing an exact-supported seed after latest candidate probation failed"
  local pre_fallback_payload_id
  pre_fallback_payload_id="$(CurrentPayloadId)"
  local fallback_stage_status=0
  StageSupportedBootstrapFallback "${fallback_ceiling}" ||
    fallback_stage_status=$?
  if (( fallback_stage_status != 0 )); then
    PreserveCurrentAfterUpdateFailure "supported seed acquisition failed" \
      "${pre_fallback_payload_id}" &&
      return 0
    return "${fallback_stage_status}"
  fi
  local fallback_payload_id="${SELECTED_PAYLOAD_ID}"
  local fallback_status=0
  AttemptCandidate "${fallback_payload_id}" || fallback_status=$?
  if (( fallback_status != 0 )); then
    PreserveCurrentAfterUpdateFailure "supported seed failed readiness" \
      "${LAST_ATTEMPT_BASELINE_PAYLOAD_ID}" &&
      return 0
    return "${fallback_status}"
  fi

  if [[ "${latest_needs_reference}" == true ]] &&
     [[ "${latest_payload_id}" != "${fallback_payload_id}" ]]; then
    Log "retrying staged latest candidate with the verified seed profile"
    attempt_status=0
    AttemptCandidate "${latest_payload_id}" || attempt_status=$?
    if (( attempt_status != 0 )); then
      PreserveCurrentAfterUpdateFailure \
        "latest candidate retry failed probation" \
        "${LAST_ATTEMPT_BASELINE_PAYLOAD_ID}"
      return 0
    fi
  fi

  [[ "${LAUNCH}" == false ]] || LaunchCurrent
}

trap 'OnExit $?' EXIT
Main "$@"
