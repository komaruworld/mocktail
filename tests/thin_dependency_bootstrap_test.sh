#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

set -euo pipefail

ROOT="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALLER="${ROOT}/scripts/install_thin_dependencies.sh"
BOOTSTRAP="${ROOT}/packaging/run.sh"
TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mocktail-thin-deps.XXXXXX")"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT

MakeOsRelease() {
  local id="$1"
  local destination="$2"
  printf 'NAME=Test\nID="%s"\n' "${id}" >"${destination}"
}

MakeFakeId() {
  local directory="$1"
  cat >"${directory}/id" <<'EOF'
#!/bin/sh
if [ "${1:-}" = -u ]; then
  printf '1000\n'
  exit 0
fi
exit 2
EOF
  chmod 0755 "${directory}/id"
}

MakeFakePacman() {
  local directory="$1"
  cat >"${directory}/pacman" <<'EOF'
#!/bin/sh
set -eu
case ${1:-} in
  -Qq)
    grep -Fqx -- "$2" "${FAKE_INSTALLED_FILE}" 2>/dev/null
    ;;
  -S)
    printf 'pacman' >>"${FAKE_MANAGER_LOG}"
    shift
    for argument in "$@"; do
      case $argument in
        --needed|--noconfirm) ;;
        *)
          printf ' %s' "$argument" >>"${FAKE_MANAGER_LOG}"
          printf '%s\n' "$argument" >>"${FAKE_INSTALLED_FILE}"
          ;;
      esac
    done
    printf '\n' >>"${FAKE_MANAGER_LOG}"
    ;;
  *) exit 2 ;;
esac
EOF
  chmod 0755 "${directory}/pacman"
}

MakeFakeXbps() {
  local directory="$1"
  cat >"${directory}/xbps-install" <<'EOF'
#!/bin/sh
exit 0
EOF
  cat >"${directory}/xbps-query" <<'EOF'
#!/bin/sh
grep -Fqx -- "$3" "${FAKE_INSTALLED_FILE}" 2>/dev/null
EOF
  chmod 0755 "${directory}/xbps-install" "${directory}/xbps-query"
}

MakeFakeApk() {
  local directory="$1"
  cat >"${directory}/apk" <<'EOF'
#!/bin/sh
case ${1:-} in
  info) grep -Fqx -- "$3" "${FAKE_INSTALLED_FILE}" 2>/dev/null ;;
  add) exit 0 ;;
  *) exit 2 ;;
esac
EOF
  chmod 0755 "${directory}/apk"
}

MakeElevator() {
  local directory="$1"
  local name="$2"
  local status="$3"
  cat >"${directory}/${name}" <<EOF
#!/bin/sh
printf '%s\\n' '${name}' >>"\${FAKE_ELEVATOR_LOG}"
if [ '${status}' -ne 0 ]; then
  exit '${status}'
fi
exec "\$@"
EOF
  chmod 0755 "${directory}/${name}"
}

fake_bin="${TEMP_DIR}/fake-bin"
mkdir -p "${fake_bin}"
MakeFakeId "${fake_bin}"
MakeFakePacman "${fake_bin}"
MakeOsRelease arch "${TEMP_DIR}/arch-os-release"
: >"${TEMP_DIR}/installed"
: >"${TEMP_DIR}/manager.log"
: >"${TEMP_DIR}/elevator.log"

common_env=(
  PATH="${fake_bin}:/usr/bin:/bin"
  MOCKTAIL_OS_RELEASE_FILE="${TEMP_DIR}/arch-os-release"
  FAKE_INSTALLED_FILE="${TEMP_DIR}/installed"
  FAKE_MANAGER_LOG="${TEMP_DIR}/manager.log"
  FAKE_ELEVATOR_LOG="${TEMP_DIR}/elevator.log"
)

arch_manifest="$(env "${common_env[@]}" "${INSTALLER}" --print)"
grep -Fq 'distribution=arch' <<<"${arch_manifest}"
grep -Fq 'manager=pacman' <<<"${arch_manifest}"
grep -Fq ' libadwaita ' <<<" ${arch_manifest} "
grep -Fq ' webkitgtk-6.0 ' <<<" ${arch_manifest} "
! grep -Eq 'webkit2gtk-4\.1|gtk3|python-gobject' <<<"${arch_manifest}"
grep -Fq ' sdl3 ' <<<" ${arch_manifest} "
! grep -Eq 'python-(yaml|requests|capstone)' <<<"${arch_manifest}"
grep -Fq ' capstone ' <<<" ${arch_manifest} "
grep -Fq 'install_command=pacman -S --needed --noconfirm' \
  <<<"${arch_manifest}"
grep -Fq 'gpu_driver=manual' <<<"${arch_manifest}"
! grep -Eq 'nvidia|vulkan-radeon|vulkan-intel|mesa-vulkan' \
  <<<"${arch_manifest}"

set +e
env "${common_env[@]}" "${INSTALLER}" --check \
  >"${TEMP_DIR}/check.stdout" 2>"${TEMP_DIR}/check.stderr"
check_status=$?
set -e
[[ "${check_status}" -eq 1 ]]
grep -Fq 'missing arch packages:' "${TEMP_DIR}/check.stderr"

MakeElevator "${fake_bin}" pkexec 0
MakeElevator "${fake_bin}" sudo 0
env "${common_env[@]}" MOCKTAIL_AUTO_INSTALL_DEPS=1 \
  "${INSTALLER}" --install
[[ "$(cat "${TEMP_DIR}/elevator.log")" == pkexec ]]
[[ ! -s "${TEMP_DIR}/manager.log" ]] ||
  grep -Fq 'pacman' "${TEMP_DIR}/manager.log"
env "${common_env[@]}" "${INSTALLER}" --check

: >"${TEMP_DIR}/installed"
: >"${TEMP_DIR}/manager.log"
: >"${TEMP_DIR}/elevator.log"
MakeElevator "${fake_bin}" pkexec 23
env "${common_env[@]}" MOCKTAIL_AUTO_INSTALL_DEPS=1 \
  "${INSTALLER}" --install
[[ "$(cat "${TEMP_DIR}/elevator.log")" == $'pkexec\nsudo' ]]
grep -Fq 'pacman' "${TEMP_DIR}/manager.log"

: >"${TEMP_DIR}/installed"
manual_bin="${TEMP_DIR}/manual-bin"
mkdir -p "${manual_bin}"
cp -- "${fake_bin}/id" "${fake_bin}/pacman" "${manual_bin}/"
ln -s -- "$(command -v grep)" "${manual_bin}/grep"
set +e
PATH="${manual_bin}" \
  MOCKTAIL_OS_RELEASE_FILE="${TEMP_DIR}/arch-os-release" \
  FAKE_INSTALLED_FILE="${TEMP_DIR}/installed" \
  FAKE_MANAGER_LOG="${TEMP_DIR}/manager.log" \
  "${INSTALLER}" --install \
  >"${TEMP_DIR}/manual.stdout" 2>"${TEMP_DIR}/manual.stderr"
manual_status=$?
set -e
[[ "${manual_status}" -ne 0 ]]
grep -Fq 'requires an interactive terminal' "${TEMP_DIR}/manual.stderr"
grep -Fq 'Run this command as root:' "${TEMP_DIR}/manual.stderr"
grep -Fq 'pacman -S --needed --noconfirm' "${TEMP_DIR}/manual.stderr"

void_bin="${TEMP_DIR}/void-bin"
mkdir -p "${void_bin}"
MakeFakeXbps "${void_bin}"
MakeOsRelease void "${TEMP_DIR}/void-os-release"
void_manifest="$(PATH="${void_bin}:/usr/bin:/bin" \
  MOCKTAIL_OS_RELEASE_FILE="${TEMP_DIR}/void-os-release" \
  MOCKTAIL_HOST_LIBC=musl FAKE_INSTALLED_FILE="${TEMP_DIR}/installed" \
  "${INSTALLER}" --print)"
grep -Fq 'manager=xbps-install' <<<"${void_manifest}"
grep -Fq ' libadwaita ' <<<" ${void_manifest} "
grep -Fq ' libwebkitgtk60 ' <<<" ${void_manifest} "
! grep -Eq 'libwebkit2gtk41|gtk\+3|python3-gobject' <<<"${void_manifest}"
! grep -Eq 'python3-(yaml|requests)|capstone-python3' <<<"${void_manifest}"
grep -Fq ' capstone ' <<<" ${void_manifest} "
grep -Eq '(^|[[:space:]])gcompat([[:space:]]|$)' <<<"${void_manifest}"

alpine_bin="${TEMP_DIR}/alpine-bin"
mkdir -p "${alpine_bin}"
MakeFakeApk "${alpine_bin}"
MakeOsRelease alpine "${TEMP_DIR}/alpine-os-release"
alpine_manifest="$(PATH="${alpine_bin}:/usr/bin:/bin" \
  MOCKTAIL_OS_RELEASE_FILE="${TEMP_DIR}/alpine-os-release" \
  FAKE_INSTALLED_FILE="${TEMP_DIR}/installed" \
  "${INSTALLER}" --print)"
grep -Fq 'manager=apk' <<<"${alpine_manifest}"
grep -Fq ' libadwaita ' <<<" ${alpine_manifest} "
grep -Fq ' webkit2gtk-6.0 ' <<<" ${alpine_manifest} "
! grep -Eq 'webkit2gtk-4\.1|gtk\+3\.0|py3-gobject3' <<<"${alpine_manifest}"
! grep -Eq 'py3-(yaml|requests|capstone)' <<<"${alpine_manifest}"
grep -Fq ' capstone ' <<<" ${alpine_manifest} "
grep -Eq '(^|[[:space:]])gcompat([[:space:]]|$)' <<<"${alpine_manifest}"

bundle="${TEMP_DIR}/bundle with spaces"
mkdir -p "${bundle}/mocktail/scripts" \
  "${bundle}/mocktail/runtime/bin"
cp -- "${BOOTSTRAP}" "${bundle}/run.sh"
cat >"${bundle}/mocktail/scripts/portable_launcher.sh" <<'EOF'
launcher fixture
EOF
cat >"${bundle}/mocktail/runtime/bin/bash" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"${FAKE_BOOTSTRAP_LOG}"
EOF
chmod 0755 "${bundle}/run.sh" \
  "${bundle}/mocktail/runtime/bin/bash"
FAKE_BOOTSTRAP_LOG="${TEMP_DIR}/bootstrap.log" \
  "${bundle}/run.sh" --help 'argument with spaces'
grep -Fxq "${bundle}/mocktail/scripts/portable_launcher.sh" \
  "${TEMP_DIR}/bootstrap.log"
grep -Fxq 'argument with spaces' "${TEMP_DIR}/bootstrap.log"

rm -f -- "${bundle}/mocktail/runtime/bin/bash"
cat >"${bundle}/mocktail/scripts/install_thin_dependencies.sh" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" >>"${FAKE_INSTALLER_LOG}"
[ "$1" != --check ]
EOF
cat >"${fake_bin}/bash" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >"${FAKE_HOST_BASH_LOG}"
EOF
chmod 0755 "${bundle}/mocktail/scripts/install_thin_dependencies.sh" \
  "${fake_bin}/bash"
: >"${TEMP_DIR}/installer.log"
PATH="${fake_bin}:/usr/bin:/bin" \
  FAKE_INSTALLER_LOG="${TEMP_DIR}/installer.log" \
  FAKE_HOST_BASH_LOG="${TEMP_DIR}/host-bash.log" \
  "${bundle}/run.sh" --version
[[ "$(cat "${TEMP_DIR}/installer.log")" == $'--check\n--install' ]]
grep -Fxq "${bundle}/mocktail/scripts/portable_launcher.sh" \
  "${TEMP_DIR}/host-bash.log"
grep -Fxq -- '--version' "${TEMP_DIR}/host-bash.log"

! grep -Eq '(^|[^[:alnum:]_])eval([^[:alnum:]_]|$)' \
  "${INSTALLER}" "${BOOTSTRAP}"
! grep -Eq '/usr/bin/env|(^|[^[:alnum:]_])dirname([^[:alnum:]_]|$)' \
  "${BOOTSTRAP}"

printf 'thin dependency bootstrap test passed\n'
