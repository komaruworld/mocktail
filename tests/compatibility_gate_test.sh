#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

set -Eeuo pipefail

if (( $# != 3 )); then
  printf 'usage: %s MOCKTAIL UNKNOWN_ELF COMPATIBILITY_MANIFEST\n' "$0" >&2
  exit 2
fi

mocktail_binary="$1"
unknown_elf="$2"
compatibility_manifest="$3"

temporary_root="$(mktemp -d)"
trap 'rm -rf "${temporary_root}"' EXIT

set +e
output="$(
  env -i \
    PATH="${PATH}" \
    HOME="${HOME}" \
    MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP=1 \
    MOCKTAIL_CONFIG_ROOT="${temporary_root}/config" \
    MOCKTAIL_DATA_ROOT="${temporary_root}/data" \
    MOCKTAIL_CACHE_ROOT="${temporary_root}/cache" \
    MOCKTAIL_STATE_ROOT="${temporary_root}/state" \
    MOCKTAIL_AUTH_ROOT="${temporary_root}/auth" \
    MOCKTAIL_COOKIE_FILE="${temporary_root}" \
    ROBLOX_LIB_PATH="${unknown_elf}" \
    MOCKTAIL_COMPATIBILITY_MANIFEST="${compatibility_manifest}" \
    "${mocktail_binary}" 2>&1
)"
status=$?
set -e

if (( status == 0 )); then
  printf 'unknown payload unexpectedly succeeded\n%s\n' "${output}" >&2
  exit 1
fi
if ! rg --fixed-strings --quiet 'Unsupported Roblox Build ID' \
    <<<"${output}"; then
  printf 'missing fail-closed Build-ID diagnostic\n%s\n' "${output}" >&2
  exit 1
fi
if rg --fixed-strings --quiet \
    'unknown Roblox fixture reached native loading' <<<"${output}"; then
  printf 'unknown payload reached native loading\n%s\n' "${output}" >&2
  exit 1
fi

printf '%s\n' "${output}"
