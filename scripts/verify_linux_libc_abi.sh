#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

Usage() {
  cat >&2 <<EOF
Usage:
  ${0##*/} --detect ELF
  ${0##*/} --libc glibc|musl ELF...
EOF
}

Die() {
  printf 'ABI check: %s\n' "$*" >&2
  exit 2
}

ReadInterpreter() {
  LC_ALL=C readelf --program-headers "$1" 2>/dev/null |
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p'
}

DetectLibc() {
  local elf="$1"
  local interpreter needed version_info
  local interpreter_libc="" needed_libc="" version_libc=""

  [[ -f "${elf}" ]] || return 1
  LC_ALL=C readelf --file-header "${elf}" >/dev/null 2>&1 || return 1
  interpreter="$(ReadInterpreter "${elf}")"
  case "${interpreter}" in
    *ld-musl-*.so.1) interpreter_libc=musl ;;
    *ld-linux*.so.*) interpreter_libc=glibc ;;
  esac

  needed="$(LC_ALL=C readelf --dynamic "${elf}" 2>/dev/null |
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')"
  if grep -Eq '^(libc\.so\.6|ld-linux[^[:space:]]*\.so(\.[0-9]+)*)$' \
      <<<"${needed}"; then
    needed_libc=glibc
  fi
  if grep -Eq '^(libc\.so|ld-musl-[^[:space:]]*\.so\.1)$' \
      <<<"${needed}"; then
    [[ -z "${needed_libc}" || "${needed_libc}" == musl ]] || {
      printf mixed
      return 0
    }
    needed_libc=musl
  fi

  version_info="$(LC_ALL=C readelf --version-info "${elf}" 2>/dev/null || true)"
  if grep -Eq 'GLIBC_[0-9]' <<<"${version_info}"; then
    version_libc=glibc
  fi

  local detected=""
  local candidate
  for candidate in "${interpreter_libc}" "${needed_libc}" "${version_libc}"; do
    [[ -n "${candidate}" ]] || continue
    if [[ -n "${detected}" && "${candidate}" != "${detected}" ]]; then
      printf mixed
      return 0
    fi
    detected="${candidate}"
  done
  printf '%s' "${detected:-unknown}"
}

mode=""
expected_libc=""
if (( $# > 0 )); then
  mode="$1"
fi
case "${mode}" in
  --detect)
    (( $# == 2 )) || { Usage; exit 2; }
    detected="$(DetectLibc "$2")" || Die "cannot inspect ELF: $2"
    printf '%s\n' "${detected}"
    [[ "${detected}" == glibc || "${detected}" == musl ]]
    exit
    ;;
  --libc)
    (( $# >= 3 )) || { Usage; exit 2; }
    expected_libc="$2"
    shift 2
    ;;
  -h|--help)
    Usage
    exit 0
    ;;
  *)
    Usage
    exit 2
    ;;
esac

[[ "${expected_libc}" == glibc || "${expected_libc}" == musl ]] || {
  Usage
  exit 2
}

status=0
for elf in "$@"; do
  if [[ ! -f "${elf}" ]]; then
    printf 'ABI check: missing ELF: %s\n' "${elf}" >&2
    status=1
    continue
  fi
  machine="$(LC_ALL=C readelf --file-header "${elf}" 2>/dev/null |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
  case "$(uname -m)" in
    x86_64) expected_machine='X86-64' ;;
    aarch64) expected_machine='AArch64' ;;
    *) expected_machine='X86-64' ;;
  esac
  if [[ "${machine}" != *"${expected_machine}"* ]]; then
    printf 'ABI check: %s is not %s (%s)\n' "${elf}" "${expected_machine}" \
      "${machine}" >&2
    status=1
  fi

  detected="$(DetectLibc "${elf}")" || detected=unknown
  if [[ "${detected}" != "${expected_libc}" ]]; then
    interpreter="$(ReadInterpreter "${elf}")"
    printf 'ABI check: %s is %s, expected %s (interpreter=%s)\n' \
      "${elf}" "${detected}" "${expected_libc}" \
      "${interpreter:-none}" >&2
    status=1
  fi
done

exit "${status}"
