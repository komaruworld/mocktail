#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

set -euo pipefail

if (( $# != 4 )); then
  printf 'usage: %s MOCKTAIL UNKNOWN_ELF RUNTIME_CONFIG_CC LEGACY_RUNTIME_CC\n' \
    "$0" >&2
  exit 2
fi

mocktail_binary="$1"
unknown_elf="$2"
runtime_config_cc="$3"
legacy_runtime_cc="$4"

unsafe_overrides=(
  MOCKTAIL_APP_BRIDGE_APP_START_THREAD
  MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT_THREAD
  MOCKTAIL_START_LUA_APP_DM_THREAD
  MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE_THREAD
  MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD
  MOCKTAIL_SEND_APP_READY_THREAD
  MOCKTAIL_SEND_GAME_LOADED_THREAD
)

for name in "${unsafe_overrides[@]}"; do
  if ! rg -Fq "\"${name}\"" "${runtime_config_cc}"; then
    printf 'typed runtime policy is missing %s\n' "${name}" >&2
    exit 1
  fi
  if ! rg -Fq "SetEnvDefault(\"${name}\", \"0\")" \
      "${legacy_runtime_cc}"; then
    printf 'safe runtime default is missing for %s\n' "${name}" >&2
    exit 1
  fi
  if ! rg -Fq "IsEnabled(\"${name}\")" "${legacy_runtime_cc}"; then
    printf 'research branch was removed for %s\n' "${name}" >&2
    exit 1
  fi
done

policy_line="$(
  rg -n -m1 'runtime_config\.has_unsafe_detached_thread_overrides\(\)' \
    "${legacy_runtime_cc}" | cut -d: -f1
)"
identity_line="$(
  rg -n -m1 'ReadElfBuildId\(library_path\)' "${legacy_runtime_cc}" |
    cut -d: -f1
)"
credential_binding_line="$(
  rg -n -m1 'ScopedRobloxCredentialBinding credential_binding' \
    "${legacy_runtime_cc}" | cut -d: -f1
)"
native_load_line="$(
  rg -n -m1 'linker::LoadLibrary\(library_path' "${legacy_runtime_cc}" |
    cut -d: -f1
)"
if [[ -z "${policy_line}" || -z "${credential_binding_line}" ||
      -z "${identity_line}" ||
      -z "${native_load_line}" ]] ||
   ! ((policy_line < credential_binding_line &&
       credential_binding_line < identity_line &&
       identity_line < native_load_line)); then
  printf 'unsafe detached-thread policy must reject before JNI binding, payload identity, and native load\n' >&2
  exit 1
fi

temporary_root="$(mktemp -d)"
trap 'rm -rf "${temporary_root}"' EXIT
sentinel='detached-policy-sensitive-value'

run_with_override() {
  local assignment="$1"
  env -i \
    PATH="${PATH}" \
    HOME="${HOME}" \
    MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP=1 \
    MOCKTAIL_CONFIG_ROOT="${temporary_root}/config" \
    MOCKTAIL_DATA_ROOT="${temporary_root}/data" \
    MOCKTAIL_CACHE_ROOT="${temporary_root}/cache" \
    MOCKTAIL_STATE_ROOT="${temporary_root}/state" \
    ROBLOX_LIB_PATH="${unknown_elf}" \
    "${assignment}" \
    "${mocktail_binary}" 2>&1
}

for name in "${unsafe_overrides[@]}"; do
  set +e
  output="$(run_with_override "${name}=${sentinel}")"
  status=$?
  set -e

  if (( status == 0 )); then
    printf '%s unexpectedly allowed an unsafe detached worker\n' "${name}" >&2
    exit 1
  fi
  if ! rg --fixed-strings --quiet \
      'Unsupported detached legacy thread overrides' <<<"${output}" ||
     ! rg --fixed-strings --quiet -- "  - ${name}" <<<"${output}"; then
    printf '%s did not reach the typed fail-closed policy\n%s\n' \
      "${name}" "${output}" >&2
    exit 1
  fi
  if rg --fixed-strings --quiet -- "${sentinel}" <<<"${output}"; then
    printf '%s leaked its environment value in diagnostics\n' "${name}" >&2
    exit 1
  fi
  if rg --fixed-strings --quiet 'Unsupported Roblox Build ID' \
      <<<"${output}"; then
    printf '%s was rejected after payload identity instead of before it\n' \
      "${name}" >&2
    exit 1
  fi
done

for allowed_value in '' 0; do
  set +e
  output="$(
    run_with_override \
      "MOCKTAIL_APP_BRIDGE_APP_START_THREAD=${allowed_value}"
  )"
  status=$?
  set -e

  if (( status == 0 )) ||
     rg --fixed-strings --quiet \
       'Unsupported detached legacy thread overrides' <<<"${output}" ||
     ! rg --fixed-strings --quiet 'Unsupported Roblox Build ID' \
       <<<"${output}"; then
    printf 'allowed detached-thread value %q did not reach the Build-ID gate\n%s\n' \
      "${allowed_value}" "${output}" >&2
    exit 1
  fi
done

printf 'detached legacy thread policy readiness passed\n'
