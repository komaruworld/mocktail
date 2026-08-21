#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

set -Eeuo pipefail

AUTO_UPDATE="${1:?auto-update script is required}"
TEMP_DIR="$(mktemp -d)"
LOCK_HOLDER_PID=""
CleanupTest() {
  if [[ -n "${LOCK_HOLDER_PID}" ]]; then
    kill "${LOCK_HOLDER_PID}" 2>/dev/null || true
    wait "${LOCK_HOLDER_PID}" 2>/dev/null || true
  fi
  rm -rf -- "${TEMP_DIR}"
}
trap CleanupTest EXIT
mkdir -p "${TEMP_DIR}/bin" "${TEMP_DIR}/bundle"
touch "${TEMP_DIR}/bundle/base.apk" "${TEMP_DIR}/bundle/split_config.x86_64.apk"

cat > "${TEMP_DIR}/bin/fetch" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
source_name=""
staging_root=""
requested_version=""
while (( $# > 0 )); do
  case "$1" in
    --source) source_name="$2"; shift 2 ;;
    --staging-root) staging_root="$2"; shift 2 ;;
    --version) requested_version="$2"; shift 2 ;;
    *) echo "unexpected fetch argument: $1" >&2; exit 1 ;;
  esac
done
printf 'fetch\n' >> "${FAKE_FETCH_CALLS}"
printf '%s|%s\n' "${source_name}" "${requested_version}" >> "${FAKE_FETCH_ARGUMENTS}"
if [[ -n "${FAKE_FETCH_LAST_VERSION:-}" ]]; then
  printf '%s\n' "${requested_version}" > "${FAKE_FETCH_LAST_VERSION}"
fi
bundle_dir="${staging_root}/com.roblox.client-9999-111111111111-222222222222"
mkdir -p "${bundle_dir}"
touch "${bundle_dir}/base.apk" "${bundle_dir}/split_config.x86_64.apk"
printf '%s\n' "${bundle_dir}"
EOF

cat > "${TEMP_DIR}/bin/validator" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
[[ "${FAKE_VALIDATOR_FAIL:-false}" != true ]] || exit 43
data_root=""
while (( $# > 0 )); do
  if [[ "$1" == --store-root ]]; then
    data_root="$2"
    shift 2
  else
    shift
  fi
done
version_code="${FAKE_VERSION_CODE}"
build_id="${FAKE_BUILD_ID}"
if [[ "${FAKE_UNKNOWN_EXACT_THEN_FALLBACK:-false}" == true ]]; then
  requested_version="$(cat "${FAKE_FETCH_LAST_VERSION}")"
  case "${requested_version}" in
    failure-candidate)
      version_code=8888
      build_id=abababababababababababababababababababab
      ;;
    2.725.1142)
      version_code=2546
      build_id=d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21
      ;;
    *) exit 44 ;;
  esac
fi
payload_id="${version_code}-${build_id}"
payload_dir="${data_root}/payloads/${payload_id}"
mkdir -p "${payload_dir}/assets/content"
touch "${payload_dir}/libroblox.so"
jq -n --arg build_id "${build_id}" \
  --argjson version_code "${version_code}" \
  --arg version_name "2.999.${version_code}" \
  '{schema_version:1, version_name:$version_name,
    version_code:$version_code, elf_build_id:$build_id}' \
  > "${payload_dir}/roblox_payload.json"
printf '%s\n' "${payload_id}"
EOF

cat > "${TEMP_DIR}/bin/store" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
root=""
compatibility=""
expected_current=""
expected_fingerprint=""
expected_rollback_target=""
candidate_profile=""
candidate_compatibility=""
runtime_fingerprint=""
runtime_build_id=""
canary_attestations=()
command_name=""
payload_id=""
while (( $# > 0 )); do
  case "$1" in
    --root) root="$2"; shift 2 ;;
    --compatibility) compatibility="$2"; shift 2 ;;
    --expected-current) expected_current="$2"; shift 2 ;;
    --expected-payload-fingerprint) expected_fingerprint="$2"; shift 2 ;;
    --expected-rollback-target) expected_rollback_target="$2"; shift 2 ;;
    --candidate-profile) candidate_profile="$2"; shift 2 ;;
    --candidate-compatibility) candidate_compatibility="$2"; shift 2 ;;
    --runtime-fingerprint) runtime_fingerprint="$2"; shift 2 ;;
    --runtime-build-id) runtime_build_id="$2"; shift 2 ;;
    --canary-attestation) canary_attestations+=("$2"); shift 2 ;;
    promote|promote-probation|fingerprint)
      command_name="$1"; payload_id="$2"; shift 2 ;;
    verify-current) command_name="$1"; shift ;;
    rollback) command_name="$1"; shift ;;
    *) echo "unexpected store argument: $1" >&2; exit 2 ;;
  esac
done
fingerprint=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
if [[ "${command_name}" == fingerprint ]]; then
  [[ -f "${root}/payloads/${payload_id}/libroblox.so" ]] || exit 1
  printf '%s\n' "${fingerprint}"
  exit 0
fi
actual_current=none
if [[ -f "${root}/current.json" ]]; then
  actual_current="$(jq -r '.payload_id // empty' "${root}/current.json")"
fi
if [[ "${command_name}" == verify-current ]]; then
  if [[ "${actual_current}" == none ]]; then
    printf 'none\n'
    exit 0
  fi
  [[ "${actual_current}" =~ ^[0-9]+-[0-9a-f]{40}$ &&
     "$(jq -r '.payload_path // empty' "${root}/current.json")" == \
       "payloads/${actual_current}" &&
     -f "${root}/payloads/${actual_current}/libroblox.so" &&
     ! -L "${root}/payloads/${actual_current}/libroblox.so" &&
     -f "${root}/payloads/${actual_current}/roblox_payload.json" ]] || exit 81
  approval_path="$(jq -r '.approval_path // empty' "${root}/current.json")"
  profile_path="$(jq -r '.host_abi_profile_path // empty' "${root}/current.json")"
  compatibility_path="$(jq -r '.compatibility_manifest_path // empty' "${root}/current.json")"
  if [[ -n "${approval_path}${profile_path}${compatibility_path}" ]]; then
    generation=""
    if [[ "${approval_path}" =~ ^approvals/${actual_current}-([0-9a-f]{40})\.json$ ]]; then
      generation="${BASH_REMATCH[1]}"
    fi
    [[ -n "${generation}" &&
       "${profile_path}" == "host_abi_profiles/${actual_current}-${generation}.json" &&
       "${compatibility_path}" == "compatibility_profiles/${actual_current}-${generation}.json" &&
       -f "${root}/${approval_path}" &&
       -f "${root}/${profile_path}" &&
       -f "${root}/${compatibility_path}" &&
       -f "${root}/approvals/${actual_current}-${generation}.canary-1.json" &&
       -f "${root}/approvals/${actual_current}-${generation}.canary-2.json" ]] || exit 82
  fi
  printf '%s\n' "${actual_current}"
  exit 0
fi
[[ -z "${expected_current}" || "${expected_current}" == "${actual_current}" ]] || exit 75
if [[ "${command_name}" == rollback ]]; then
  [[ "${FAKE_STORE_MODE:-pass}" != \
     fail_after_commit_and_rollback ]] || exit 80
  [[ -f "${root}/previous_good.json" ]] || exit 1
  [[ -z "${expected_rollback_target}" ||
     "$(jq -r .payload_id "${root}/previous_good.json")" == \
       "${expected_rollback_target}" ]] || exit 78
  cp -- "${root}/previous_good.json" "${root}/current.json"
  printf 'rollback:%s\n' "$(jq -r .payload_id "${root}/current.json")" >> "${FAKE_CALLS}"
  exit 0
fi
[[ ( "${command_name}" == promote ||
      "${command_name}" == promote-probation ) &&
   "${expected_fingerprint}" == "${fingerprint}" ]] || exit 74
if [[ "${command_name}" == promote-probation ]]; then
  [[ -f "${candidate_profile}" && -f "${candidate_compatibility}" &&
     "${runtime_fingerprint}" =~ ^[0-9a-f]{64}$ &&
     "${runtime_build_id}" =~ ^[0-9a-f]{40}$ &&
     "${#canary_attestations[@]}" -eq 2 &&
     -f "${canary_attestations[0]}" &&
     -f "${canary_attestations[1]}" ]] || exit 79
  [[ "$(jq -r .run "${canary_attestations[0]}")" == 1 &&
     "$(jq -r .run "${canary_attestations[1]}")" == 2 ]] || exit 79
  printf 'promote-probation:%s\n' "${payload_id}" >> "${FAKE_CALLS}"
else
  printf 'promote:%s\n' "${payload_id}" >> "${FAKE_CALLS}"
fi
if [[ "${actual_current}" == "${payload_id}" &&
      "${command_name}" != promote-probation ]]; then
  jq -n --arg payload_id "${payload_id}" \
    --arg payload_path "payloads/${payload_id}" \
    '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path}' \
    > "${root}/current.json"
  exit 0
fi
[[ "${FAKE_STORE_MODE:-pass}" != fail_before_commit ]] || exit 76
mkdir -p "${root}"
if [[ "${actual_current}" != none &&
      "${actual_current}" != "${payload_id}" ]]; then
  cp -- "${root}/current.json" "${root}/previous_good.json"
fi
if [[ "${command_name}" == promote-probation ]]; then
  mkdir -p "${root}/approvals" "${root}/host_abi_profiles" \
    "${root}/compatibility_profiles"
  canary_one_sha256="$(sha256sum "${canary_attestations[0]}" | awk '{print $1}')"
  canary_two_sha256="$(sha256sum "${canary_attestations[1]}" | awk '{print $1}')"
  generation="$(printf '%s\n' \
      "runtime_build_id=${runtime_build_id}" \
      "runtime_sha256=${runtime_fingerprint}" \
      "canary_1=${canary_one_sha256}" \
      "canary_2=${canary_two_sha256}" |
    sha256sum | awk '{print substr($1, 1, 40)}')"
  cp -- "${candidate_profile}" \
    "${root}/host_abi_profiles/${payload_id}-${generation}.json"
  cp -- "${candidate_compatibility}" \
    "${root}/compatibility_profiles/${payload_id}-${generation}.json"
  cp -- "${canary_attestations[0]}" \
    "${root}/approvals/${payload_id}-${generation}.canary-1.json"
  cp -- "${canary_attestations[1]}" \
    "${root}/approvals/${payload_id}-${generation}.canary-2.json"
  jq -n --arg runtime_build_id "${runtime_build_id}" \
    --arg runtime_sha256 "${runtime_fingerprint}" \
    '{runtime_build_id:$runtime_build_id,
      runtime_sha256:$runtime_sha256,
      canary_runtime_sha256:$runtime_sha256}' \
    > "${root}/approvals/${payload_id}-${generation}.json"
  jq -n --arg payload_id "${payload_id}" \
    --arg payload_path "payloads/${payload_id}" \
    --arg approval_path "approvals/${payload_id}-${generation}.json" \
    --arg profile_path "host_abi_profiles/${payload_id}-${generation}.json" \
    --arg compatibility_path "compatibility_profiles/${payload_id}-${generation}.json" \
    '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path,
      approval_path:$approval_path,host_abi_profile_path:$profile_path,
      compatibility_manifest_path:$compatibility_path}' \
    > "${root}/current.json"
else
  jq -n --arg payload_id "${payload_id}" \
    --arg payload_path "payloads/${payload_id}" \
    '{schema_version:1, payload_id:$payload_id, payload_path:$payload_path}' \
    > "${root}/current.json"
fi
[[ "${FAKE_STORE_MODE:-pass}" != fail_after_commit &&
   "${FAKE_STORE_MODE:-pass}" != fail_after_commit_and_rollback ]] || exit 77
EOF

cat > "${TEMP_DIR}/bin/smoke" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
[[ "${MOCKTAIL_SKIP_UPDATE_CHECK:-0}" == 1 ]] || exit 1
[[ -f "${ROBLOX_LIB_PATH:-}" && ! -L "${ROBLOX_LIB_PATH}" ]] || exit 1
[[ -d "${MOCKTAIL_ASSET_ROOT:-}" && ! -L "${MOCKTAIL_ASSET_ROOT}" ]] || exit 1
[[ -d "${MOCKTAIL_ASSET_PATH:-}" && ! -L "${MOCKTAIL_ASSET_PATH}" ]] || exit 1
[[ "${MOCKTAIL_ASSET_PATH}" == "${MOCKTAIL_ASSET_ROOT}/content" ]] || exit 1
if [[ "${MOCKTAIL_ISOLATED_CANARY:-0}" == 1 ]]; then
  [[ "${MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS:-}" == 5000 ]] || exit 1
else
  [[ "${MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS:-}" == 0 ]] || exit 1
fi
if [[ "${MOCKTAIL_HOST_ABI_CANARY:-0}" == 1 ]]; then
  [[ "${MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI:-0}" == 1 &&
     -f "${MOCKTAIL_HOST_ABI_PROFILE_FILE:-}" &&
     "${MOCKTAIL_COMPATIBILITY_MANIFEST:-}" != \
       "${FAKE_EXPECTED_COMPATIBILITY_PATH:?}" ]] || exit 1
  [[ " $* " == *" --allow-unverified-build "* ]] || exit 1
elif [[ -n "${MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT:-}" ]]; then
  [[ -f "${MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT}" &&
     -f "${MOCKTAIL_HOST_ABI_PROFILE_FILE:-}" &&
     -f "${MOCKTAIL_COMPATIBILITY_MANIFEST:-}" &&
     "${MOCKTAIL_COMPATIBILITY_MANIFEST}" != \
       "${FAKE_EXPECTED_COMPATIBILITY_PATH:?}" &&
     -z "${MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI+x}" ]] || exit 1
else
  [[ "${MOCKTAIL_COMPATIBILITY_MANIFEST:-}" == \
     "${FAKE_EXPECTED_COMPATIBILITY_PATH:?}" ]] || exit 1
  [[ -z "${MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI+x}" ]] || exit 1
fi
[[ "${MOCKTAIL_SKIP_LIBROBLOX_CTORS:-}" == 0 ]] || exit 1
[[ "${MOCKTAIL_HOST_JNI_SINGLETON_SEED:-}" == 0 ]] || exit 1
[[ -z "${MOCKTAIL_HOST_ALLOCATOR_BRIDGES+x}" ]] || exit 1
[[ -z "${MOCKTAIL_PATCH_HOSTILE_CANARY+x}" ]] || exit 1
[[ -z "${MOCKTAIL_ROBLOX_COOKIES+x}" ]] || exit 1
if [[ "${MOCKTAIL_ISOLATED_CANARY:-0}" == 1 ]]; then
  [[ -z "${MOCKTAIL_COOKIE_FILE+x}" &&
     "${MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP:-0}" == 1 &&
     "${MOCKTAIL_IGNORE_WINDOW_CLOSE:-0}" == 1 ]] || exit 1
fi
[[ "${VK_DRIVER_FILES:-}" == /test/mocktail-vk.json &&
   "${VK_ICD_FILENAMES:-}" == /test/mocktail-icd.json &&
   "${MESA_LOADER_DRIVER_OVERRIDE:-}" == mocktail-test-driver &&
   "${DRI_PRIME:-}" == 1 &&
   "${__NV_PRIME_RENDER_OFFLOAD:-}" == 1 &&
   "${__VK_LAYER_NV_optimus:-}" == NVIDIA_only &&
   "${__GLX_VENDOR_LIBRARY_NAME:-}" == nvidia ]] || exit 1
if [[ "${MOCKTAIL_ISOLATED_CANARY:-0}" == 1 ]]; then
  [[ -z "${BASH_ENV+x}" && -z "${LD_PRELOAD+x}" &&
     -z "${LD_AUDIT+x}" && -z "${RIPGREP_CONFIG_PATH+x}" ]] || exit 1
  [[ "$(type -t mocktail_hostile_canary || true)" != function ]] || exit 1
  [[ -n "${FAKE_LIVE_DATA_ROOT:-}" &&
     "${MOCKTAIL_DATA_ROOT:-}" != "${FAKE_LIVE_DATA_ROOT}" ]] || exit 1
  [[ "${HOME}" != "${FAKE_ORIGINAL_HOME:?}" &&
     "${XDG_DATA_HOME}" == "${MOCKTAIL_DATA_ROOT}" &&
     "${XDG_CACHE_HOME}" == "${MOCKTAIL_CACHE_ROOT}" &&
     "${XDG_STATE_HOME}" == "${MOCKTAIL_STATE_ROOT}" &&
     "${XDG_CONFIG_HOME}" == "${MOCKTAIL_CONFIG_ROOT}" ]] || exit 1
  mkdir -p "${MOCKTAIL_DATA_ROOT}"
  printf 'isolated\n' > "${MOCKTAIL_DATA_ROOT}/canary-marker"
else
  [[ "${MOCKTAIL_DATA_ROOT:-}" == "${FAKE_LIVE_DATA_ROOT:?}" ]] || exit 1
fi
MOCKTAIL_LOG_DIR="${MOCKTAIL_LOG_DIR:-${FAKE_LIVE_DATA_ROOT}/launch-logs}"
mkdir -p "${MOCKTAIL_LOG_DIR}"
mode="${FAKE_SMOKE_MODE:-pass}"
if [[ "${mode}" != no_log ]]; then
  printf '%s\n' \
    '[window] graphics backend=direct-vulkan video=x11 egl=system gles=system' \
    '[window] first Roblox Vulkan frame presented' \
    '[input] typed production input ready: mouse=1 touch=1 keyboard=1' \
    '[main] Roblox lifecycle shutdown: Stopped' \
    > "${MOCKTAIL_LOG_DIR}/tierC_fake.log"
fi
printf 'smoke:%s\n' "${mode}" >> "${FAKE_CALLS}"
case "${mode}" in
  pass|no_log) ;;
  fail) exit 42 ;;
  exit3) exit 3 ;;
  crash)
    ulimit -c 0
    kill -SEGV "$$"
    ;;
  hang)
    printf '%s\n' "$$" > "${FAKE_HANG_PID_PATH}"
    exec sleep 60
    ;;
  race)
    jq -n \
      --arg payload_id 7777-cccccccccccccccccccccccccccccccccccccccc \
      --arg payload_path payloads/7777-cccccccccccccccccccccccccccccccccccccccc \
      '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path}' \
      > "${FAKE_LIVE_DATA_ROOT}/current.json"
    ;;
  *) exit 2 ;;
esac
EOF
cat > "${TEMP_DIR}/bin/check" <<'EOF'
#!/usr/bin/env bash
[[ "${FAKE_CHECK_FAIL:-false}" != true ]] || exit 42
if [[ -n "${FAKE_CHECK_CALLS:-}" ]]; then
  printf '%s\n' "$1" >> "${FAKE_CHECK_CALLS}"
fi
jq -nc --argjson version_code "${FAKE_REMOTE_VERSION}" \
  '{version_code:$version_code,version_name:"test"}'
EOF
cat > "${TEMP_DIR}/bin/derive.py" <<'EOF'
#!/usr/bin/env python3
import argparse
import hashlib
import json
import os

parser = argparse.ArgumentParser()
parser.add_argument("--reference-lib", required=True)
parser.add_argument("--reference-profile", required=True)
parser.add_argument("--candidate-lib", required=True)
parser.add_argument("--payload-metadata", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--compatibility-output", required=True)
args = parser.parse_args()
with open(args.payload_metadata, "r", encoding="utf-8") as source:
    metadata = json.load(source)
build_id = metadata["elf_build_id"].lower()
version_code = metadata["version_code"]
payload_id = f"{version_code}-{build_id}"
with open(args.candidate_lib, "rb") as source:
    payload_sha256 = hashlib.sha256(source.read()).hexdigest()
profile = {
    "schema_version": 1,
    "payload_id": payload_id,
    "payload_path": f"payloads/{payload_id}",
    "elf_build_id": build_id,
    "payload_sha256": payload_sha256,
    "reference": {"elf_build_id": "1" * 40, "payload_sha256": "2" * 64},
    "profile": {},
}
compatibility = {
    "schema_version": 1,
    "profiles": [{
        "version_name": metadata["version_name"],
        "version_code": version_code,
        "elf_build_id": build_id,
        "status": "experimental",
        "default_allowed": True,
        "allow_legacy_binary_patches": False,
        "allow_host_abi_bridges": True,
        "allow_host_constructor_replay": True,
    }],
}
with open(args.output, "w", encoding="utf-8") as output:
    json.dump(profile, output)
with open(args.compatibility_output, "w", encoding="utf-8") as output:
    json.dump(compatibility, output)
with open(os.environ["FAKE_DERIVER_CALLS"], "a", encoding="utf-8") as output:
    output.write(f"{args.reference_profile}|{payload_id}\n")
EOF
chmod +x "${TEMP_DIR}/bin/"*

export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/data"
export MOCKTAIL_CACHE_ROOT="${TEMP_DIR}/cache"
export MOCKTAIL_STATE_ROOT="${TEMP_DIR}/state"
export MOCKTAIL_UPDATE_FETCH_SCRIPT="${TEMP_DIR}/bin/fetch"
export MOCKTAIL_UPDATE_VALIDATOR_SCRIPT="${TEMP_DIR}/bin/validator"
export MOCKTAIL_UPDATE_STORE_SCRIPT="${TEMP_DIR}/bin/store"
export MOCKTAIL_UPDATE_SMOKE_SCRIPT="${TEMP_DIR}/bin/smoke"
export MOCKTAIL_UPDATE_CHECK_SCRIPT="${TEMP_DIR}/bin/check"
export MOCKTAIL_CONFIG_FILE="${TEMP_DIR}/config.yaml"
export MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${TEMP_DIR}/compatibility.json"
export MOCKTAIL_BOOTSTRAP_SOURCES_PATH="${TEMP_DIR}/bootstrap-sources.json"
export FAKE_BUNDLE_DIR="${TEMP_DIR}/bundle"
export FAKE_CALLS="${TEMP_DIR}/calls"
export FAKE_FETCH_CALLS="${TEMP_DIR}/fetch-calls"
export FAKE_FETCH_ARGUMENTS="${TEMP_DIR}/fetch-arguments"
export FAKE_FETCH_LAST_VERSION="${TEMP_DIR}/fetch-last-version"
export FAKE_HANG_PID_PATH="${TEMP_DIR}/hang-pid"
export FAKE_DERIVER_CALLS="${TEMP_DIR}/deriver-calls"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
export FAKE_EXPECTED_COMPATIBILITY_PATH="${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
export MOCKTAIL_UPDATE_TEST_ENV_PASSTHROUGH=1
export FAKE_ORIGINAL_HOME="${HOME}"
export MOCKTAIL_HOST_ALLOCATOR_BRIDGES=1
export MOCKTAIL_PATCH_HOSTILE_CANARY=1
export MOCKTAIL_SKIP_LIBROBLOX_CTORS=1
export MOCKTAIL_COMPATIBILITY_MANIFEST="${TEMP_DIR}/hostile-compatibility.json"
export MOCKTAIL_ROBLOX_COOKIES=hostile-secret-cookie
export BASH_ENV=/dev/null
export LD_PRELOAD=
export LD_AUDIT=
export RIPGREP_CONFIG_PATH=/dev/null
export VK_DRIVER_FILES=/test/mocktail-vk.json
export VK_ICD_FILENAMES=/test/mocktail-icd.json
export MESA_LOADER_DRIVER_OVERRIDE=mocktail-test-driver
export DRI_PRIME=1
export __NV_PRIME_RENDER_OFFLOAD=1
export __VK_LAYER_NV_optimus=NVIDIA_only
export __GLX_VENDOR_LIBRARY_NAME=nvidia
mocktail_hostile_canary() { :; }
export -f mocktail_hostile_canary
printf '%s\n' 'version: 1' > "${MOCKTAIL_CONFIG_FILE}"
jq -n '{schema_version:1,sources:[
  {package:"com.roblox.client",version_name:"2.725.1142",version_code:2546,
   provider:"uptodown"}
]}' > "${MOCKTAIL_BOOTSTRAP_SOURCES_PATH}"
jq -n \
  --arg current d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21 \
  --arg seed 1686400865ae0e408cd7bd67de7a439625c6fd13 \
  --arg failure bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb \
  --arg candidate aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
  '{schema_version:1, profiles:[
    {version_name:"2.725.1142",version_code:2546,elf_build_id:$current,
     status:"supported",default_allowed:true,
     allow_legacy_binary_patches:false},
    {version_name:"2.727.1199",version_code:2628,elf_build_id:$seed,
     status:"supported",default_allowed:true,
     allow_legacy_binary_patches:false},
    {version_name:"failure-candidate",version_code:8888,elf_build_id:$failure,
     status:"supported",default_allowed:true,
     allow_legacy_binary_patches:false},
    {version_name:"candidate",version_code:9999,elf_build_id:$candidate,
     status:"experimental",default_allowed:false,
     allow_legacy_binary_patches:false}
  ]}' > "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"

# MOCKTAIL_CONFIG_ROOT is the runtime override used before path exports.
# The updater must read the same file instead of silently falling back to the
# default XDG path.
saved_config_file="${MOCKTAIL_CONFIG_FILE}"
unset MOCKTAIL_CONFIG_FILE
export MOCKTAIL_CONFIG_ROOT="${TEMP_DIR}/typed-config-root"
export FAKE_CHECK_CALLS="${TEMP_DIR}/config-root-check-calls"
mkdir -p "${MOCKTAIL_CONFIG_ROOT}"
cat > "${MOCKTAIL_CONFIG_ROOT}/config.yaml" <<'EOF'
version: 1
updates:
  automatic: false
EOF
"${AUTO_UPDATE}" --skip-build --scheduled
[[ ! -e "${FAKE_CHECK_CALLS}" ]] || {
  echo "scheduled updater ignored MOCKTAIL_CONFIG_ROOT" >&2
  exit 1
}
unset MOCKTAIL_CONFIG_ROOT FAKE_CHECK_CALLS
export MOCKTAIL_CONFIG_FILE="${saved_config_file}"

# A fresh installation downloads and stages provider latest before considering
# a supported fallback. This fixture makes that latest download resolve to an
# exact-supported payload, so no fallback mirror request is necessary.
export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/bootstrap-data"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
export FAKE_VERSION_CODE=2628
export FAKE_BUILD_ID=1686400865ae0e408cd7bd67de7a439625c6fd13
export FAKE_REMOTE_VERSION=2628
export MOCKTAIL_UPDATE_LOCAL_VERSION=none
progress_packets="${TEMP_DIR}/first-run-progress"
exec 7>"${progress_packets}"
MOCKTAIL_UPDATE_PROGRESS_FD=7 \
  "${AUTO_UPDATE}" --skip-build --startup-preflight
exec 7>&-
[[ "$(cat "${progress_packets}")" == \
  'PDownloading Roblox...PInstalling Roblox...PTesting Roblox...PInstalling Roblox...' ]] || {
  echo "first-run progress did not follow updater stages" >&2
  exit 1
}
[[ "$(cat "${FAKE_FETCH_ARGUMENTS}")" == 'apk-pure|' ]] || {
  echo "first-run did not request provider latest before fallback" >&2
  exit 1
}
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "2628-${FAKE_BUILD_ID}" ]]
if find "${MOCKTAIL_CACHE_ROOT}/downloads/apk-staging" -mindepth 1 \
    -maxdepth 1 -type d -print -quit | grep -q .; then
  echo "first-run bootstrap retained a duplicate downloaded APK bundle" >&2
  exit 1
fi

# Once a runnable local payload exists, interactive startup does not contact
# the provider or repeat candidate canaries. The user timer owns remote work.
startup_fetch_calls="$(wc -l < "${FAKE_FETCH_CALLS}")"
export FAKE_CHECK_CALLS="${TEMP_DIR}/startup-check-calls"
export FAKE_REMOTE_VERSION=9999
rm -f -- "${FAKE_CHECK_CALLS}" "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --startup-preflight
[[ ! -e "${FAKE_CHECK_CALLS}" && ! -e "${FAKE_CALLS}" ]]
[[ "$(wc -l < "${FAKE_FETCH_CALLS}")" -eq "${startup_fetch_calls}" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "2628-${FAKE_BUILD_ID}" ]]
unset FAKE_CHECK_CALLS

# A second installation can share the same managed payload root while the
# first one is still updating it. Interactive startup waits for that updater
# and then validates its result instead of reporting a false crash and
# collecting a useless lock-contention support bundle.
lock_ready="${TEMP_DIR}/startup-lock-ready"
lock_release="${TEMP_DIR}/startup-lock-release"
[[ ! -e "${MOCKTAIL_STATE_ROOT}/support" ]]
(
  exec 9>"${MOCKTAIL_DATA_ROOT}/.auto-update.lock"
  flock 9
  touch "${lock_ready}"
  while [[ ! -e "${lock_release}" ]]; do
    sleep 0.01
  done
) &
LOCK_HOLDER_PID=$!
for _ in {1..500}; do
  [[ ! -e "${lock_ready}" ]] || break
  sleep 0.01
done
[[ -e "${lock_ready}" ]]

waiting_progress="${TEMP_DIR}/waiting-progress"
waiting_stderr="${TEMP_DIR}/waiting-stderr"
exec 7>"${waiting_progress}"
MOCKTAIL_UPDATE_PROGRESS_FD=7 \
  "${AUTO_UPDATE}" --skip-build --startup-preflight \
  >"${TEMP_DIR}/waiting-stdout" 2>"${waiting_stderr}" &
waiting_updater_pid=$!
for _ in {1..500}; do
  grep -Fq 'PWaiting for Roblox update...' "${waiting_progress}" && break
  kill -0 "${waiting_updater_pid}" 2>/dev/null
  sleep 0.01
done
grep -Fq 'PWaiting for Roblox update...' "${waiting_progress}"
kill -0 "${waiting_updater_pid}" 2>/dev/null
touch "${lock_release}"
wait "${LOCK_HOLDER_PID}"
LOCK_HOLDER_PID=""
wait "${waiting_updater_pid}"
exec 7>&-
grep -Fq \
  'another Roblox update is already running; waiting for its result' \
  "${waiting_stderr}"
grep -Fq \
  'the running Roblox update finished; validating its result' \
  "${waiting_stderr}"
[[ ! -e "${MOCKTAIL_STATE_ROOT}/support" ]]

# Existing short configs predate updates.*. The documented automatic=true
# default still enables the timer instead of silently disabling background
# updates for those users.
export FAKE_CHECK_CALLS="${TEMP_DIR}/default-scheduled-check-calls"
export FAKE_REMOTE_VERSION=2628
export MOCKTAIL_UPDATE_LOCAL_VERSION=2628
"${AUTO_UPDATE}" --skip-build --scheduled
[[ -s "${FAKE_CHECK_CALLS}" ]]
unset FAKE_CHECK_CALLS
export MOCKTAIL_UPDATE_LOCAL_VERSION=none

# A temporary metadata outage on a new installation still bootstraps the
# newest explicitly supported profile instead of downloading unknown latest
# bytes that cannot pass the compatibility gate.
export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/offline-bootstrap-data"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
export FAKE_VERSION_CODE=8888
export FAKE_BUILD_ID=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
export FAKE_CHECK_FAIL=true
rm -f -- "${FAKE_CALLS}" "${FAKE_FETCH_CALLS}" "${FAKE_FETCH_ARGUMENTS}"
"${AUTO_UPDATE}" --skip-build
[[ "$(cat "${FAKE_FETCH_ARGUMENTS}")" == 'apk-pure|failure-candidate' ]] || {
  echo "offline first-run bootstrap did not request the newest supported profile" >&2
  exit 1
}
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "8888-${FAKE_BUILD_ID}" ]]
unset FAKE_CHECK_FAIL

# A fresh install must not choose one of two supported Build IDs that share
# the newest fallback versionCode. It skips that ambiguous version and uses
# the next unique exact profile.
PRIMARY_COMPATIBILITY_PATH="${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
AMBIGUOUS_BOOTSTRAP_BUILD=cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd
jq --arg build_id "${AMBIGUOUS_BOOTSTRAP_BUILD}" '
  .profiles += [{version_name:"failure-candidate-alt",version_code:8888,
    elf_build_id:$build_id,status:"supported",default_allowed:true,
    allow_legacy_binary_patches:false}]
' "${PRIMARY_COMPATIBILITY_PATH}" > "${TEMP_DIR}/compatibility.bootstrap-ambiguous.json"
export MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${TEMP_DIR}/compatibility.bootstrap-ambiguous.json"
export FAKE_EXPECTED_COMPATIBILITY_PATH="${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/ambiguous-bootstrap-data"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
for ambiguous_build in \
    bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb \
    "${AMBIGUOUS_BOOTSTRAP_BUILD}"; do
  ambiguous_dir="${MOCKTAIL_DATA_ROOT}/payloads/8888-${ambiguous_build}"
  mkdir -p "${ambiguous_dir}/assets/content"
  touch "${ambiguous_dir}/libroblox.so"
  jq -n --arg build_id "${ambiguous_build}" \
    '{schema_version:1,version_code:8888,elf_build_id:$build_id}' \
    > "${ambiguous_dir}/roblox_payload.json"
done
export FAKE_VERSION_CODE=2546
export FAKE_BUILD_ID=d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21
export FAKE_REMOTE_VERSION=9999
rm -f -- "${FAKE_CALLS}" "${FAKE_FETCH_CALLS}" "${FAKE_FETCH_ARGUMENTS}"
"${AUTO_UPDATE}" --skip-build
mapfile -t ambiguous_bootstrap_fetches < "${FAKE_FETCH_ARGUMENTS}"
[[ "${ambiguous_bootstrap_fetches[0]}" == 'apk-pure|' &&
   "${ambiguous_bootstrap_fetches[1]}" == 'apk-pure|2.727.1199' &&
   "${#ambiguous_bootstrap_fetches[@]}" -eq 2 ]] || {
  echo "fresh install did not bind provider payloads to fallback versions" >&2
  exit 1
}
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "2546-${FAKE_BUILD_ID}" ]]

# A provider can reuse a versionCode for a different ELF Build ID. If that
# happens on first run, keep the unknown bytes staged and retry one lower
# unique compatibility profile instead of leaving the installation inert.
export MOCKTAIL_UPDATE_COMPATIBILITY_PATH="${PRIMARY_COMPATIBILITY_PATH}"
export FAKE_EXPECTED_COMPATIBILITY_PATH="${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/unknown-exact-bootstrap-data"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
export FAKE_REMOTE_VERSION=8888
export MOCKTAIL_UPDATE_LOCAL_VERSION=none
export FAKE_UNKNOWN_EXACT_THEN_FALLBACK=true
rm -f -- "${FAKE_CALLS}" "${FAKE_FETCH_CALLS}" \
  "${FAKE_FETCH_ARGUMENTS}" "${FAKE_FETCH_LAST_VERSION}"
"${AUTO_UPDATE}" --skip-build
mapfile -t fallback_fetches < "${FAKE_FETCH_ARGUMENTS}"
[[ "${fallback_fetches[0]}" == 'apk-pure|' ]]
[[ "${fallback_fetches[1]}" == 'apk-pure|failure-candidate' ]]
[[ "${fallback_fetches[2]}" == 'apk-pure|2.727.1199' ]]
[[ "${fallback_fetches[3]}" == 'uptodown|2.725.1142' ]]
[[ "${#fallback_fetches[@]}" -eq 4 ]]
[[ -d "${MOCKTAIL_DATA_ROOT}/payloads/8888-abababababababababababababababababababab" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21 ]]
unset FAKE_UNKNOWN_EXACT_THEN_FALLBACK

export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/data"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
rm -f -- "${FAKE_CALLS}" "${FAKE_FETCH_CALLS}" "${FAKE_FETCH_ARGUMENTS}"

export FAKE_VERSION_CODE=2546
export FAKE_BUILD_ID=d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21
export FAKE_REMOTE_VERSION=2546
export MOCKTAIL_UPDATE_LOCAL_VERSION=none
export FAKE_STORE_MODE=fail_after_commit
"${AUTO_UPDATE}" --skip-build
unset FAKE_STORE_MODE
[[ "$(sed -n '1p' "${FAKE_CALLS}")" == smoke:pass ]]
[[ "$(sed -n '2p' "${FAKE_CALLS}")" == \
  "promote:2546-${FAKE_BUILD_ID}" ]]
[[ "$(wc -l < "${FAKE_CALLS}")" -eq 2 ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "2546-${FAKE_BUILD_ID}" ]]

# A distinct supported update exercises the actual 2546 -> candidate path.
# A regular readiness failure, native crash, or watchdog timeout must preserve
# both live manifests byte-for-byte and must not invoke payload promotion.
PREVIOUS_ID=2400-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee
previous_payload_dir="${MOCKTAIL_DATA_ROOT}/payloads/${PREVIOUS_ID}"
mkdir -p "${previous_payload_dir}/assets/content"
touch "${previous_payload_dir}/libroblox.so"
jq -n '{schema_version:1,version_name:"2.700.2400",version_code:2400,
  elf_build_id:"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}' \
  > "${previous_payload_dir}/roblox_payload.json"
jq '.profiles += [{version_name:"2.700.2400",version_code:2400,
  elf_build_id:"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
  status:"supported",default_allowed:true,
  allow_legacy_binary_patches:false}]' \
  "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}" > "${TEMP_DIR}/compatibility.previous.json"
mv -- "${TEMP_DIR}/compatibility.previous.json" \
  "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
jq -n --arg payload_id "${PREVIOUS_ID}" \
  --arg payload_path "payloads/${PREVIOUS_ID}" \
  '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path}' \
  > "${MOCKTAIL_DATA_ROOT}/previous_good.json"
cp -- "${MOCKTAIL_DATA_ROOT}/current.json" "${TEMP_DIR}/current.before-canary"
cp -- "${MOCKTAIL_DATA_ROOT}/previous_good.json" \
  "${TEMP_DIR}/previous.before-canary"
export FAKE_VERSION_CODE=8888
export FAKE_BUILD_ID=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
export FAKE_REMOTE_VERSION=8888
export MOCKTAIL_UPDATE_LOCAL_VERSION=2546
for smoke_mode in fail exit3 crash hang; do
  rm -f -- "${FAKE_CALLS}" "${FAKE_HANG_PID_PATH}"
  export FAKE_SMOKE_MODE="${smoke_mode}"
  started_at="${SECONDS}"
  set +e
  if [[ "${smoke_mode}" == hang ]]; then
    MOCKTAIL_UPDATE_CANARY_TIMEOUT_S=1 \
      "${AUTO_UPDATE}" --skip-build >"${TEMP_DIR}/${smoke_mode}.stdout" \
        2>"${TEMP_DIR}/${smoke_mode}.stderr"
  else
    "${AUTO_UPDATE}" --skip-build >"${TEMP_DIR}/${smoke_mode}.stdout" \
      2>"${TEMP_DIR}/${smoke_mode}.stderr"
  fi
  canary_status=$?
  set -e
  [[ "${canary_status}" -eq 0 ]] || {
    echo "${smoke_mode} canary prevented the working payload from continuing" >&2
    exit 1
  }
  [[ "$(cat "${FAKE_CALLS}")" == "smoke:${smoke_mode}" ]] || {
    echo "${smoke_mode} canary reached payload promotion" >&2
    exit 1
  }
  cmp --silent "${TEMP_DIR}/current.before-canary" \
    "${MOCKTAIL_DATA_ROOT}/current.json" || {
    echo "${smoke_mode} canary changed the active payload manifest" >&2
    exit 1
  }
  cmp --silent "${TEMP_DIR}/previous.before-canary" \
    "${MOCKTAIL_DATA_ROOT}/previous_good.json" || {
    echo "${smoke_mode} canary changed previous-good before promotion" >&2
    exit 1
  }
  [[ ! -e "${MOCKTAIL_DATA_ROOT}/canary-marker" ]] || {
    echo "${smoke_mode} canary used the live data root" >&2
    exit 1
  }
  if compgen -G "${MOCKTAIL_CACHE_ROOT}/.update-canary.*" >/dev/null; then
    echo "${smoke_mode} canary left its isolated directory behind" >&2
    exit 1
  fi
  if [[ "${smoke_mode}" == hang ]]; then
    [[ $((SECONDS - started_at)) -lt 10 ]]
    hang_pid="$(cat "${FAKE_HANG_PID_PATH}")"
    if kill -0 "${hang_pid}" 2>/dev/null; then
      echo "timed-out canary left an orphan process ${hang_pid}" >&2
      exit 1
    fi
  fi
  if [[ "${smoke_mode}" == fail ]]; then
    support_archive="$(find "${MOCKTAIL_STATE_ROOT}/support" -maxdepth 1 \
      -type f -name 'mocktail-support-*.tar.gz' -print -quit)"
    [[ -n "${support_archive}" ]] || {
      echo "failed canary did not retain a support bundle" >&2
      exit 1
    }
    support_extract="${TEMP_DIR}/canary-support"
    rm -rf -- "${support_extract}"
    mkdir -p -- "${support_extract}"
    tar -C "${support_extract}" -xzf "${support_archive}"
    grep -Fxq 'context=updater' \
      "${support_extract}/mocktail-support/runtime.txt"
    grep -Fxq 'reason=canary-failed' \
      "${support_extract}/mocktail-support/runtime.txt"
    grep -Fxq 'exit_code=42' \
      "${support_extract}/mocktail-support/runtime.txt"
    grep -Fxq 'status=candidate' \
      "${support_extract}/mocktail-support/payload.txt"
    grep -Fxq 'version_code=8888' \
      "${support_extract}/mocktail-support/payload.txt"
    grep -Fxq "elf_build_id=${FAKE_BUILD_ID}" \
      "${support_extract}/mocktail-support/payload.txt"
    grep -Fxq 'actual_video_driver=x11' \
      "${support_extract}/mocktail-support/markers.txt"
  fi
done
unset FAKE_SMOKE_MODE

# A successful smoke followed by a store failure still preserves both
# manifests. If the store reports failure after its commit point, the updater
# observes candidate=current and performs a conditional rollback to 2546.
for store_mode in fail_before_commit fail_after_commit; do
  rm -f -- "${FAKE_CALLS}"
  export FAKE_STORE_MODE="${store_mode}"
  set +e
  "${AUTO_UPDATE}" --skip-build >"${TEMP_DIR}/${store_mode}.stdout" \
    2>"${TEMP_DIR}/${store_mode}.stderr"
  update_status=$?
  set -e
  [[ "${update_status}" -eq 0 ]]
  cmp --silent "${TEMP_DIR}/current.before-canary" \
    "${MOCKTAIL_DATA_ROOT}/current.json"
  [[ "$(sed -n '1p' "${FAKE_CALLS}")" == smoke:pass ]]
  [[ "$(sed -n '2p' "${FAKE_CALLS}")" == \
    "promote:8888-${FAKE_BUILD_ID}" ]]
  ! grep -Fq '[auto-update] support bundle:' \
    "${TEMP_DIR}/${store_mode}.stderr"
  promotion_archive="$(find "${MOCKTAIL_STATE_ROOT}/support" -maxdepth 1 \
    -type f -name 'mocktail-support-*.tar.gz' -printf '%T@ %p\n' | \
    sort -n | tail -n 1 | cut -d' ' -f2-)"
  [[ -f "${promotion_archive}" ]] || {
    echo "${store_mode} did not retain a promotion support bundle" >&2
    exit 1
  }
  promotion_extract="${TEMP_DIR}/promotion-support-${store_mode}"
  mkdir -p -- "${promotion_extract}"
  tar -C "${promotion_extract}" -xzf "${promotion_archive}"
  grep -Fxq 'reason=updater-promotion-failed' \
    "${promotion_extract}/mocktail-support/runtime.txt"
  grep -Fxq 'actual_video_driver=x11' \
    "${promotion_extract}/mocktail-support/markers.txt"
  if [[ "${store_mode}" == fail_before_commit ]]; then
    [[ "$(wc -l < "${FAKE_CALLS}")" -eq 2 ]]
    cmp --silent "${TEMP_DIR}/previous.before-canary" \
      "${MOCKTAIL_DATA_ROOT}/previous_good.json"
  else
    [[ "$(wc -l < "${FAKE_CALLS}")" -eq 3 ]]
    [[ "$(sed -n '3p' "${FAKE_CALLS}")" == \
      "rollback:2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21" ]]
    [[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/previous_good.json")" == \
      2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21 ]]
  fi
done

# If both the post-commit promotion result and its conditional rollback fail,
# current is uncertain. The updater must return a hard failure instead of
# treating the candidate as the previous working payload.
rm -f -- "${FAKE_CALLS}"
export FAKE_STORE_MODE=fail_after_commit_and_rollback
set +e
"${AUTO_UPDATE}" --skip-build \
  >"${TEMP_DIR}/rollback-failure.stdout" \
  2>"${TEMP_DIR}/rollback-failure.stderr"
rollback_failure_status=$?
set -e
[[ "${rollback_failure_status}" -ne 0 ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "8888-${FAKE_BUILD_ID}" ]]
grep -Fq 'current payload is uncertain' \
  "${TEMP_DIR}/rollback-failure.stderr"
cp -- "${TEMP_DIR}/current.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/current.json"
cp -- "${TEMP_DIR}/previous.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"
unset FAKE_STORE_MODE
cp -- "${TEMP_DIR}/previous.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"

# A concurrent writer changing current during canary wins the CAS race; the
# updater must not overwrite that manifest with its candidate.
rm -f -- "${FAKE_CALLS}"
export FAKE_SMOKE_MODE=race
set +e
"${AUTO_UPDATE}" --skip-build >"${TEMP_DIR}/race.stdout" \
  2>"${TEMP_DIR}/race.stderr"
race_status=$?
set -e
  [[ "${race_status}" -ne 0 ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  7777-cccccccccccccccccccccccccccccccccccccccc ]]
[[ "$(cat "${FAKE_CALLS}")" == smoke:race ]]
cp -- "${TEMP_DIR}/current.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/current.json"
cmp --silent "${TEMP_DIR}/previous.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"
unset FAKE_SMOKE_MODE

export FAKE_VERSION_CODE=2546
export FAKE_BUILD_ID=d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21
export FAKE_REMOTE_VERSION=2546
current_library="${MOCKTAIL_DATA_ROOT}/payloads/2546-${FAKE_BUILD_ID}/libroblox.so"
rm -- "${current_library}"
unset MOCKTAIL_UPDATE_LOCAL_VERSION
"${AUTO_UPDATE}" --skip-build
[[ -f "${current_library}" ]] || {
  echo "missing current library did not trigger payload recovery" >&2
  exit 1
}
[[ "$(wc -l < "${FAKE_FETCH_CALLS}")" -eq 3 ]]

rm -f "${FAKE_CALLS}"
export FAKE_VERSION_CODE=9999
export FAKE_BUILD_ID=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
export FAKE_REMOTE_VERSION=9999
export MOCKTAIL_UPDATE_LOCAL_VERSION=2546
"${AUTO_UPDATE}" --skip-build
[[ -d "${MOCKTAIL_DATA_ROOT}/payloads/9999-${FAKE_BUILD_ID}" ]] || {
  echo "unknown Build ID was not retained as an isolated staged payload" >&2
  exit 1
}
[[ "$(sed -n '1p' "${FAKE_CALLS}")" == smoke:pass ]]
[[ "$(sed -n '2p' "${FAKE_CALLS}")" == \
  "promote:8888-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "8888-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/previous_good.json")" == \
  2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21 ]]

# The same selection must work without another download when the unsupported
# remote payload was already staged by an earlier check.
cp -- "${TEMP_DIR}/current.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/current.json"
cp -- "${TEMP_DIR}/previous.before-canary" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"
fetch_calls_before_staged_fallback="$(wc -l < "${FAKE_FETCH_CALLS}")"
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build
[[ "$(sed -n '1p' "${FAKE_CALLS}")" == smoke:pass ]]
[[ "$(sed -n '2p' "${FAKE_CALLS}")" == \
  "promote:8888-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]]
[[ "$(wc -l < "${FAKE_FETCH_CALLS}")" -eq \
  "${fetch_calls_before_staged_fallback}" ]] || {
  echo "staged supported fallback triggered another provider download" >&2
  exit 1
}

# Two exact-supported Build IDs at the same newest staged version are
# ambiguous. Neither may be selected automatically.
AMBIGUOUS_BUILD_A=1111111111111111111111111111111111111111
AMBIGUOUS_BUILD_B=2222222222222222222222222222222222222222
jq --arg first "${AMBIGUOUS_BUILD_A}" --arg second "${AMBIGUOUS_BUILD_B}" '
  .profiles += [
    {version_name:"ambiguous-a",version_code:9000,elf_build_id:$first,
     status:"supported",default_allowed:true,
     allow_legacy_binary_patches:false},
    {version_name:"ambiguous-b",version_code:9000,elf_build_id:$second,
     status:"supported",default_allowed:true,
     allow_legacy_binary_patches:false}
  ]' "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}" \
  > "${TEMP_DIR}/compatibility.ambiguous.json"
mv -- "${TEMP_DIR}/compatibility.ambiguous.json" \
  "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
for ambiguous_build in "${AMBIGUOUS_BUILD_A}" "${AMBIGUOUS_BUILD_B}"; do
  ambiguous_dir="${MOCKTAIL_DATA_ROOT}/payloads/9000-${ambiguous_build}"
  mkdir -p "${ambiguous_dir}/assets/content"
  touch "${ambiguous_dir}/libroblox.so"
  jq -n --arg build_id "${ambiguous_build}" \
    '{schema_version:1,version_code:9000,elf_build_id:$build_id}' \
    > "${ambiguous_dir}/roblox_payload.json"
done
export MOCKTAIL_UPDATE_LOCAL_VERSION=8888
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build
[[ "$(sed -n '1p' "${FAKE_CALLS}")" == smoke:pass ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "8888-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]]

# The ambiguity check must also apply when the provider remote version is the
# same version shared by both exact-supported staged Build IDs.
export FAKE_REMOTE_VERSION=9000
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build
[[ ! -e "${FAKE_CALLS}" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "8888-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" ]]
export FAKE_REMOTE_VERSION=9999

# Reconsider a staged candidate after its exact Build-ID profile is approved,
# without downloading the same payload again.
fetch_calls_before_approval="$(wc -l < "${FAKE_FETCH_CALLS}")"
jq '(.profiles[] | select(.version_code == 9999)) |=
      (.status = "supported" | .default_allowed = true)' \
  "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}" \
  > "${TEMP_DIR}/compatibility.approved.json"
mv -- "${TEMP_DIR}/compatibility.approved.json" \
  "${MOCKTAIL_UPDATE_COMPATIBILITY_PATH}"
rm -f -- "${FAKE_CALLS}"
export MOCKTAIL_UPDATE_LOCAL_VERSION=8888
"${AUTO_UPDATE}" --skip-build
[[ "$(sed -n '1p' "${FAKE_CALLS}")" == smoke:pass ]]
[[ "$(sed -n '2p' "${FAKE_CALLS}")" == \
  "promote:9999-${FAKE_BUILD_ID}" ]]
[[ "$(wc -l < "${FAKE_FETCH_CALLS}")" -eq \
  "${fetch_calls_before_approval}" ]] || {
  echo "approved staged candidate was downloaded again" >&2
  exit 1
}
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "9999-${FAKE_BUILD_ID}" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/previous_good.json")" == \
  8888-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ]]

# Stale provider metadata must never turn an update check into a downgrade.
export MOCKTAIL_UPDATE_LOCAL_VERSION=9999
export FAKE_REMOTE_VERSION=9000
fetch_calls_before_regression="$(wc -l < "${FAKE_FETCH_CALLS}")"
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build
[[ ! -e "${FAKE_CALLS}" ]]
[[ "$(wc -l < "${FAKE_FETCH_CALLS}")" -eq \
  "${fetch_calls_before_regression}" ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "9999-${FAKE_BUILD_ID}" ]]

# An exact derived sidecar is exercised twice in separate HOME/XDG roots. Only
# after both attestations bind the same candidate/profile/runtime fingerprint
# may the store receive promote-probation.
main_data_root="${MOCKTAIL_DATA_ROOT}"
probation_data_root="${TEMP_DIR}/probation-data"
export MOCKTAIL_DATA_ROOT="${probation_data_root}"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
seed_build_id=1686400865ae0e408cd7bd67de7a439625c6fd13
seed_payload_id="2628-${seed_build_id}"
seed_payload_dir="${MOCKTAIL_DATA_ROOT}/payloads/${seed_payload_id}"
mkdir -p "${seed_payload_dir}/assets/content"
printf 'seed\n' > "${seed_payload_dir}/libroblox.so"
jq -n --arg build_id "${seed_build_id}" \
  '{schema_version:1,version_name:"2.727.1199",version_code:2628,
    elf_build_id:$build_id}' > "${seed_payload_dir}/roblox_payload.json"
jq -n --arg payload_id "${seed_payload_id}" \
  --arg payload_path "payloads/${seed_payload_id}" \
  '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path}' \
  > "${MOCKTAIL_DATA_ROOT}/current.json"
printf '{}\n' > "${TEMP_DIR}/reference-profile.json"
export MOCKTAIL_UPDATE_REFERENCE_PROFILE="${TEMP_DIR}/reference-profile.json"
export MOCKTAIL_UPDATE_PROFILE_DERIVER="${TEMP_DIR}/bin/derive.py"
export MOCKTAIL_UPDATE_LOCAL_VERSION=2628
export FAKE_REMOTE_VERSION=10001
export FAKE_VERSION_CODE=10001
export FAKE_BUILD_ID=edededededededededededededededededededed
rm -f -- "${FAKE_CALLS}" "${FAKE_DERIVER_CALLS}"
"${AUTO_UPDATE}" --skip-build --launch
mapfile -t probation_calls < "${FAKE_CALLS}"
[[ "${probation_calls[0]}" == smoke:pass &&
   "${probation_calls[1]}" == smoke:pass &&
   "${probation_calls[2]}" == \
     "promote-probation:10001-${FAKE_BUILD_ID}" &&
   "${probation_calls[3]}" == smoke:pass &&
   "${#probation_calls[@]}" -eq 4 ]] || {
  echo "derived candidate did not run two canaries then approved launch" >&2
  exit 1
}
grep -Fq "${TEMP_DIR}/reference-profile.json|10001-${FAKE_BUILD_ID}" \
  "${FAKE_DERIVER_CALLS}"
[[ ! -e "${MOCKTAIL_DATA_ROOT}/canary-marker" ]]
if compgen -G "${MOCKTAIL_CACHE_ROOT}/.update-canary.*" >/dev/null; then
  echo "derived candidate left an isolated canary root behind" >&2
  exit 1
fi
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "10001-${FAKE_BUILD_ID}" ]]
deriver_calls_before_refresh="$(wc -l < "${FAKE_DERIVER_CALLS}")"
cp -- "${MOCKTAIL_DATA_ROOT}/previous_good.json" \
  "${TEMP_DIR}/probation-previous.before-refresh"
stale_fingerprint_approval_ref="$(jq -r .approval_path \
  "${MOCKTAIL_DATA_ROOT}/current.json")"
approval_file="${MOCKTAIL_DATA_ROOT}/${stale_fingerprint_approval_ref}"
jq '.runtime_sha256 = ("0" * 64) |
    .canary_runtime_sha256 = ("0" * 64)' \
  "${approval_file}" > "${TEMP_DIR}/approval.wrong-runtime-sha.json"
mv -- "${TEMP_DIR}/approval.wrong-runtime-sha.json" "${approval_file}"
export MOCKTAIL_UPDATE_LOCAL_VERSION=10001
export FAKE_REMOTE_VERSION=10001
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --no-launch
mapfile -t fingerprint_refresh_calls < "${FAKE_CALLS}"
[[ "${fingerprint_refresh_calls[0]}" == smoke:pass &&
   "${fingerprint_refresh_calls[1]}" == smoke:pass &&
   "${fingerprint_refresh_calls[2]}" == \
     "promote-probation:10001-${FAKE_BUILD_ID}" &&
   "${#fingerprint_refresh_calls[@]}" -eq 3 ]] || {
  echo "runtime SHA change did not refresh approval through two canaries" >&2
  exit 1
}
old_approval_ref="$(jq -r .approval_path "${MOCKTAIL_DATA_ROOT}/current.json")"
[[ "${old_approval_ref}" != "${stale_fingerprint_approval_ref}" &&
   "$(jq -r .runtime_sha256 \
     "${MOCKTAIL_DATA_ROOT}/${old_approval_ref}")" != \
     "$(printf '0%.0s' {1..64})" ]]

export MOCKTAIL_UPDATE_RUNTIME_BUILD_ID=fefefefefefefefefefefefefefefefefefefefe
export MOCKTAIL_UPDATE_LOCAL_VERSION=10001
export FAKE_REMOTE_VERSION=10001
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --no-launch
mapfile -t refresh_calls < "${FAKE_CALLS}"
[[ "${refresh_calls[0]}" == smoke:pass &&
   "${refresh_calls[1]}" == smoke:pass &&
   "${refresh_calls[2]}" == \
     "promote-probation:10001-${FAKE_BUILD_ID}" &&
   "${#refresh_calls[@]}" -eq 3 ]] || {
  echo "runtime change did not refresh approval through two canaries" >&2
  exit 1
}
[[ "$(wc -l < "${FAKE_DERIVER_CALLS}")" -eq \
  "${deriver_calls_before_refresh}" ]] || {
  echo "approval refresh derived a profile from the candidate itself" >&2
  exit 1
}
new_approval_ref="$(jq -r .approval_path "${MOCKTAIL_DATA_ROOT}/current.json")"
[[ "${new_approval_ref}" != "${old_approval_ref}" &&
   -f "${MOCKTAIL_DATA_ROOT}/${old_approval_ref}" &&
   -f "${MOCKTAIL_DATA_ROOT}/${new_approval_ref}" ]]
[[ "$(jq -r .runtime_build_id \
  "${MOCKTAIL_DATA_ROOT}/${new_approval_ref}")" == \
  "${MOCKTAIL_UPDATE_RUNTIME_BUILD_ID}" ]]
cmp --silent "${TEMP_DIR}/probation-previous.before-refresh" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"

# Receipt refresh happens before the version API. If the new runtime canary
# fails, rollback to validated previous_good must complete even while that API
# is down; the stale dynamic approval is never launched.
cp -- "${MOCKTAIL_DATA_ROOT}/current.json" \
  "${TEMP_DIR}/probation-current.after-refresh"
export MOCKTAIL_UPDATE_RUNTIME_BUILD_ID=abababababababababababababababababababab
export FAKE_SMOKE_MODE=fail
export FAKE_CHECK_FAIL=true
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --no-launch
mapfile -t stale_api_calls < "${FAKE_CALLS}"
[[ "${stale_api_calls[0]}" == smoke:fail &&
   "${stale_api_calls[1]}" == "rollback:${seed_payload_id}" &&
   "${#stale_api_calls[@]}" -eq 2 ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "${seed_payload_id}" ]]

# A stale dynamic previous_good is not a usable rollback target after a runtime
# change. Recover through the newest staged exact-supported payload instead.
cp -- "${TEMP_DIR}/probation-current.after-refresh" \
  "${MOCKTAIL_DATA_ROOT}/current.json"
cp -- "${TEMP_DIR}/probation-current.after-refresh" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"
rm -f -- "${FAKE_CALLS}"
export MOCKTAIL_UPDATE_RUNTIME_BUILD_ID=cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd
"${AUTO_UPDATE}" --skip-build --no-launch \
  >"${TEMP_DIR}/stale-supported-fallback.stdout" \
  2>"${TEMP_DIR}/stale-supported-fallback.stderr"
mapfile -t supported_fallback_calls < "${FAKE_CALLS}"
[[ "${supported_fallback_calls[0]}" == smoke:fail &&
   "${supported_fallback_calls[1]}" == "promote:${seed_payload_id}" &&
   "${#supported_fallback_calls[@]}" -eq 2 ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "${seed_payload_id}" ]]
grep -Fq "activating exact-supported local fallback ${seed_payload_id}" \
  "${TEMP_DIR}/stale-supported-fallback.stderr"

# The rejection backoff must take the same recovery path without repeating the
# failed canary on every launch.
cp -- "${TEMP_DIR}/probation-current.after-refresh" \
  "${MOCKTAIL_DATA_ROOT}/current.json"
cp -- "${TEMP_DIR}/probation-current.after-refresh" \
  "${MOCKTAIL_DATA_ROOT}/previous_good.json"
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --no-launch \
  >"${TEMP_DIR}/rejected-supported-fallback.stdout" \
  2>"${TEMP_DIR}/rejected-supported-fallback.stderr"
[[ "$(cat "${FAKE_CALLS}")" == "promote:${seed_payload_id}" ]]
grep -Fq 'runtime reapproval matches a recent failed canary' \
  "${TEMP_DIR}/rejected-supported-fallback.stderr"
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "${seed_payload_id}" ]]

# With neither a runtime-valid previous_good nor a staged exact-supported
# payload, the updater still fails closed.
no_fallback_data_root="${TEMP_DIR}/no-fallback-data"
dynamic_payload_id="10001-${FAKE_BUILD_ID}"
mkdir -p "${no_fallback_data_root}/payloads"
cp -a -- "${probation_data_root}/payloads/${dynamic_payload_id}" \
  "${no_fallback_data_root}/payloads/"
cp -a -- "${probation_data_root}/approvals" \
  "${probation_data_root}/host_abi_profiles" \
  "${probation_data_root}/compatibility_profiles" \
  "${no_fallback_data_root}/"
cp -- "${TEMP_DIR}/probation-current.after-refresh" \
  "${no_fallback_data_root}/current.json"
export MOCKTAIL_DATA_ROOT="${no_fallback_data_root}"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
rm -f -- "${FAKE_CALLS}"
set +e
"${AUTO_UPDATE}" --skip-build --no-launch \
  >"${TEMP_DIR}/stale-no-fallback.stdout" \
  2>"${TEMP_DIR}/stale-no-fallback.stderr"
stale_no_fallback_status=$?
set -e
[[ "${stale_no_fallback_status}" -ne 0 ]]
[[ ! -e "${FAKE_CALLS}" ]]
grep -Fq 'rollback failed' "${TEMP_DIR}/stale-no-fallback.stderr"
unset FAKE_SMOKE_MODE FAKE_CHECK_FAIL
unset MOCKTAIL_UPDATE_RUNTIME_BUILD_ID

# An exact rejected candidate is not re-canaryed on every normal launch. The
# key includes candidate/profile/runtime/analyzer evidence, and an explicit
# override remains available for manual retry.
backoff_data_root="${TEMP_DIR}/backoff-data"
export MOCKTAIL_DATA_ROOT="${backoff_data_root}"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
backoff_seed_dir="${MOCKTAIL_DATA_ROOT}/payloads/${seed_payload_id}"
mkdir -p "${backoff_seed_dir}/assets/content"
printf 'seed\n' > "${backoff_seed_dir}/libroblox.so"
cp -- "${seed_payload_dir}/roblox_payload.json" \
  "${backoff_seed_dir}/roblox_payload.json"
jq -n --arg payload_id "${seed_payload_id}" \
  --arg payload_path "payloads/${seed_payload_id}" \
  '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path}' \
  > "${MOCKTAIL_DATA_ROOT}/current.json"
export MOCKTAIL_UPDATE_RUNTIME_BUILD_ID=1212121212121212121212121212121212121212
export MOCKTAIL_UPDATE_LOCAL_VERSION=2628
export FAKE_REMOTE_VERSION=10002
export FAKE_VERSION_CODE=10002
export FAKE_BUILD_ID=1212121212121212121212121212121212121212
export FAKE_SMOKE_MODE=no_log
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --no-launch
[[ "$(cat "${FAKE_CALLS}")" == smoke:no_log ]]
backoff_support_count="$(find "${MOCKTAIL_STATE_ROOT}/support" -maxdepth 1 \
  -type f -name 'mocktail-support-*.tar.gz' | wc -l)"
"${AUTO_UPDATE}" --skip-build --no-launch
[[ "$(cat "${FAKE_CALLS}")" == smoke:no_log ]] || {
  echo "recent rejected candidate was canaryed again" >&2
  exit 1
}
[[ "$(find "${MOCKTAIL_STATE_ROOT}/support" -maxdepth 1 \
  -type f -name 'mocktail-support-*.tar.gz' | wc -l)" == \
  "${backoff_support_count}" ]]
[[ -f "${MOCKTAIL_STATE_ROOT}/rejected_candidates/10002-${FAKE_BUILD_ID}.json" ]]
MOCKTAIL_UPDATE_RETRY_REJECTED=1 \
  "${AUTO_UPDATE}" --skip-build --no-launch
[[ "$(grep -Fc smoke:no_log "${FAKE_CALLS}")" -eq 2 ]]
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "${seed_payload_id}" ]]
unset FAKE_SMOKE_MODE MOCKTAIL_UPDATE_RUNTIME_BUILD_ID
unset MOCKTAIL_UPDATE_PROFILE_DERIVER MOCKTAIL_UPDATE_REFERENCE_PROFILE
export MOCKTAIL_DATA_ROOT="${main_data_root}"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"

# The version returned by the latest API is part of the probation identity.
# A valid signed payload for a different version may remain staged, but it
# must never reach canary or replace the working current payload.
provider_mismatch_root="${TEMP_DIR}/provider-mismatch-data"
export MOCKTAIL_DATA_ROOT="${provider_mismatch_root}"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
mismatch_current_build=d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21
mismatch_current_id="2546-${mismatch_current_build}"
mismatch_current_dir="${MOCKTAIL_DATA_ROOT}/payloads/${mismatch_current_id}"
mkdir -p "${mismatch_current_dir}/assets/content"
touch "${mismatch_current_dir}/libroblox.so"
jq -n --arg build_id "${mismatch_current_build}" \
  '{schema_version:1,version_name:"2.725.1142",version_code:2546,
    elf_build_id:$build_id}' > "${mismatch_current_dir}/roblox_payload.json"
jq -n --arg payload_id "${mismatch_current_id}" \
  --arg payload_path "payloads/${mismatch_current_id}" \
  '{schema_version:1,payload_id:$payload_id,payload_path:$payload_path}' \
  > "${MOCKTAIL_DATA_ROOT}/current.json"
export MOCKTAIL_UPDATE_LOCAL_VERSION=2546
export FAKE_REMOTE_VERSION=11000
export FAKE_VERSION_CODE=10999
export FAKE_BUILD_ID=3434343434343434343434343434343434343434
rm -f -- "${FAKE_CALLS}"
"${AUTO_UPDATE}" --skip-build --no-launch \
  >"${TEMP_DIR}/provider-mismatch.stdout" \
  2>"${TEMP_DIR}/provider-mismatch.stderr"
[[ "$(jq -r .payload_id "${MOCKTAIL_DATA_ROOT}/current.json")" == \
  "${mismatch_current_id}" ]]
[[ ! -e "${FAKE_CALLS}" ]]
grep -Fq \
  'provider payload versionCode 10999 does not match expected 11000' \
  "${TEMP_DIR}/provider-mismatch.stderr"

export MOCKTAIL_DATA_ROOT="${main_data_root}"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"

# A failure outside the canary still produces a persistent bundle with a
# structured phase reason and a canonical updater event, not an empty log.
export MOCKTAIL_DATA_ROOT="${TEMP_DIR}/validation-failure-data"
export MOCKTAIL_CACHE_ROOT="${TEMP_DIR}/validation-failure-cache"
export MOCKTAIL_STATE_ROOT="${TEMP_DIR}/validation-failure-state"
export FAKE_LIVE_DATA_ROOT="${MOCKTAIL_DATA_ROOT}"
export FAKE_VALIDATOR_FAIL=true
export MOCKTAIL_UPDATE_SUPPORT_MARKER="${MOCKTAIL_STATE_ROOT}/.update-support.123.marker"
set +e
"${AUTO_UPDATE}" --skip-build --source apk-pure \
  >"${TEMP_DIR}/validation-failure.stdout" \
  2>"${TEMP_DIR}/validation-failure.stderr"
validation_status=$?
set -e
unset FAKE_VALIDATOR_FAIL
[[ "${validation_status}" -eq 43 ]] || {
  cat "${TEMP_DIR}/validation-failure.stderr" >&2
  echo "validator status ${validation_status} was not preserved" >&2
  exit 1
}
[[ -d "${MOCKTAIL_UPDATE_SUPPORT_MARKER}" ]]
unset MOCKTAIL_UPDATE_SUPPORT_MARKER
validation_archive="$(find "${MOCKTAIL_STATE_ROOT}/support" -maxdepth 1 \
  -type f -name 'mocktail-support-*.tar.gz' -print -quit)"
[[ -n "${validation_archive}" ]] || {
  echo "validator failure did not retain a support bundle" >&2
  exit 1
}
validation_extract="${TEMP_DIR}/validation-support"
mkdir -p -- "${validation_extract}"
tar -C "${validation_extract}" -xzf "${validation_archive}"
grep -Fxq 'reason=updater-validation-failed' \
  "${validation_extract}/mocktail-support/runtime.txt"
grep -Fxq '[auto-update] payload validation started' \
  "${validation_extract}/mocktail-support/recent.log"

unset MOCKTAIL_HOST_ALLOCATOR_BRIDGES MOCKTAIL_PATCH_HOSTILE_CANARY \
  MOCKTAIL_SKIP_LIBROBLOX_CTORS MOCKTAIL_COMPATIBILITY_MANIFEST \
  BASH_ENV LD_PRELOAD LD_AUDIT RIPGREP_CONFIG_PATH VK_DRIVER_FILES \
  VK_ICD_FILENAMES MESA_LOADER_DRIVER_OVERRIDE DRI_PRIME \
  __NV_PRIME_RENDER_OFFLOAD __VK_LAYER_NV_optimus \
  __GLX_VENDOR_LIBRARY_NAME
unset -f mocktail_hostile_canary

echo "automatic updater orchestration checks passed"
