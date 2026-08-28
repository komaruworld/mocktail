#!/bin/sh
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

set -eu
set -f

PROGRAM=${0##*/}
OS_RELEASE_FILE=${MOCKTAIL_OS_RELEASE_FILE:-/etc/os-release}
ACTION=
DISTRO=
MANAGER=
PACKAGES=
INSTALL_ARGS=
MISSING_PACKAGES=

Die() {
  printf '%s: %s\n' "$PROGRAM" "$*" >&2
  exit 2
}

Usage() {
  cat <<'EOF'
Usage: install_thin_dependencies.sh --check|--print|--install

  --check    Exit successfully when every declared host package is installed.
  --print    Print the detected distribution and exact package set.
  --install  Interactively install missing packages through pkexec or sudo.

Set MOCKTAIL_AUTO_INSTALL_DEPS=1 to permit installation without a terminal.
Physical GPU drivers and Vulkan ICD packages are never selected automatically.
EOF
}

StripOsReleaseValue() {
  value=$1
  case $value in
    \"*\") value=${value#\"}; value=${value%\"} ;;
    \'*\') value=${value#\'}; value=${value%\'} ;;
  esac
  printf '%s\n' "$value"
}

ReadDistributionId() {
  [ -f "$OS_RELEASE_FILE" ] && [ -r "$OS_RELEASE_FILE" ] || return 1
  while IFS='=' read -r key value; do
    case $key in
      ID)
        StripOsReleaseValue "$value"
        return 0
        ;;
    esac
  done <"$OS_RELEASE_FILE"
  return 1
}

DetectDistribution() {
  distro_id=$(ReadDistributionId 2>/dev/null || true)
  case $distro_id in
    arch|archlinux) DISTRO=arch; MANAGER=pacman ;;
    void) DISTRO=void; MANAGER=xbps-install ;;
    alpine) DISTRO=alpine; MANAGER=apk ;;
    *)
      manager_count=0
      if command -v pacman >/dev/null 2>&1; then
        DISTRO=arch; MANAGER=pacman; manager_count=$((manager_count + 1))
      fi
      if command -v xbps-install >/dev/null 2>&1; then
        DISTRO=void; MANAGER=xbps-install; manager_count=$((manager_count + 1))
      fi
      if command -v apk >/dev/null 2>&1; then
        DISTRO=alpine; MANAGER=apk; manager_count=$((manager_count + 1))
      fi
      [ "$manager_count" -eq 1 ] ||
        Die "unsupported or ambiguous Linux distribution: ${distro_id:-unknown}"
      ;;
  esac
  command -v "$MANAGER" >/dev/null 2>&1 ||
    Die "$MANAGER is unavailable on detected $DISTRO host"
}

HostUsesMusl() {
  case ${MOCKTAIL_HOST_LIBC:-} in
    musl) return 0 ;;
    glibc) return 1 ;;
    '') ;;
    *) Die "MOCKTAIL_HOST_LIBC must be glibc or musl" ;;
  esac
  [ -e /lib/ld-musl-x86_64.so.1 ] ||
    [ -e /lib64/ld-musl-x86_64.so.1 ]
}

LoadPackageManifest() {
  case $DISTRO in
    arch)
      PACKAGES='bash
bubblewrap
xdg-dbus-proxy
vulkan-icd-loader
libadwaita
webkitgtk-6.0
jq
unzip
file
binutils
jre-openjdk-headless
sdl3
sdl3_ttf
libutf8proc
libelf
capstone
curl
libyaml
fontconfig
minizip
libplacebo
util-linux'
      INSTALL_ARGS='-S --needed --noconfirm'
      ;;
    void)
      PACKAGES='bash
bubblewrap
xdg-dbus-proxy
vulkan-loader
libadwaita
libwebkitgtk60
jq
unzip
file
binutils
openjdk17-jre
SDL3
SDL3_ttf
libutf8proc
libelf
capstone
libcurl
libyaml
fontconfig
minizip
libplacebo
util-linux'
      if HostUsesMusl; then
        PACKAGES="$PACKAGES gcompat"
      fi
      INSTALL_ARGS='-Sy'
      ;;
    alpine)
      PACKAGES='bash
bubblewrap
xdg-dbus-proxy
vulkan-loader
libadwaita
webkit2gtk-6.0
jq
unzip
file
binutils
openjdk17-jre-headless
sdl3
sdl3_ttf
utf8proc
libelf
capstone
curl
libyaml
fontconfig
minizip
libplacebo
coreutils
findutils
util-linux'
      PACKAGES="$PACKAGES gcompat"
      INSTALL_ARGS='add --no-interactive'
      ;;
    *) Die "internal error: no package manifest for $DISTRO" ;;
  esac
}

PackageInstalled() {
  package=$1
  case $package in
    bash) command -v bash >/dev/null 2>&1 && return 0 ;;
    jre-openjdk-headless|openjdk17-jre|openjdk17-jre-headless)
      command -v java >/dev/null 2>&1 && return 0
      ;;
    binutils) command -v readelf >/dev/null 2>&1 && return 0 ;;
    jq|unzip|file) command -v "$package" >/dev/null 2>&1 && return 0 ;;
    util-linux) command -v flock >/dev/null 2>&1 && return 0 ;;
  esac
  case $DISTRO in
    arch) pacman -Qq "$package" >/dev/null 2>&1 ;;
    void) xbps-query -p pkgver "$package" >/dev/null 2>&1 ;;
    alpine) apk info -e "$package" >/dev/null 2>&1 ;;
    *) return 1 ;;
  esac
}

FindMissingPackages() {
  MISSING_PACKAGES=
  for package in $PACKAGES; do
    if ! PackageInstalled "$package"; then
      if [ -n "$MISSING_PACKAGES" ]; then
        MISSING_PACKAGES="$MISSING_PACKAGES $package"
      else
        MISSING_PACKAGES=$package
      fi
    fi
  done
}

PrintPackages() {
  printf 'distribution=%s\n' "$DISTRO"
  printf 'manager=%s\n' "$MANAGER"
  printf 'packages:'
  for package in $PACKAGES; do
    printf ' %s' "$package"
  done
  printf '\n'
  printf 'install_command=%s' "$MANAGER"
  for argument in $INSTALL_ARGS; do
    printf ' %s' "$argument"
  done
  for package in $PACKAGES; do
    printf ' %s' "$package"
  done
  printf '\n'
  printf 'gpu_driver=manual\n'
  printf 'Physical GPU driver and Vulkan ICD package selection remains manual.\n'
}

PrintManualCommand() {
  printf 'Run this command as root:\n  %s' "$MANAGER" >&2
  for argument in $INSTALL_ARGS; do
    printf ' %s' "$argument" >&2
  done
  for package in $MISSING_PACKAGES; do
    printf ' %s' "$package" >&2
  done
  printf '\n' >&2
}

ConfirmInteractiveInstall() {
  if [ "${MOCKTAIL_AUTO_INSTALL_DEPS:-0}" = 1 ]; then
    return 0
  fi
  [ -t 0 ] && [ -t 1 ] || {
    printf '%s: dependency installation requires an interactive terminal; ' \
      "$PROGRAM" >&2
    printf 'set MOCKTAIL_AUTO_INSTALL_DEPS=1 to authorize unattended installation\n' \
      >&2
    return 1
  }
  printf 'Mocktail needs these %s packages:%s\n' "$DISTRO" "$MISSING_PACKAGES"
  printf 'Install them now? [y/N] '
  IFS= read -r answer || return 1
  case $answer in
    y|Y|yes|YES|Yes) return 0 ;;
    *) return 1 ;;
  esac
}

RunManager() {
  elevator=$1
  shift
  set -- "$@" "$MANAGER"
  for argument in $INSTALL_ARGS; do
    set -- "$@" "$argument"
  done
  for package in $MISSING_PACKAGES; do
    set -- "$@" "$package"
  done
  if [ -n "$elevator" ]; then
    "$elevator" "$@"
  else
    "$@"
  fi
}

InstallMissingPackages() {
  [ -n "$MISSING_PACKAGES" ] || return 0
  ConfirmInteractiveInstall || {
    PrintManualCommand
    return 1
  }

  if [ "$(id -u)" -eq 0 ]; then
    RunManager ''
    return $?
  fi
  if command -v pkexec >/dev/null 2>&1; then
    if RunManager pkexec; then
      return 0
    fi
    printf '%s: pkexec failed; trying sudo\n' "$PROGRAM" >&2
  fi
  if command -v sudo >/dev/null 2>&1; then
    if RunManager sudo; then
      return 0
    fi
    printf '%s: sudo failed\n' "$PROGRAM" >&2
  fi
  PrintManualCommand
  return 1
}

[ "$#" -eq 1 ] || {
  Usage >&2
  exit 2
}
case $1 in
  --check|--print|--install) ACTION=$1 ;;
  -h|--help) Usage; exit 0 ;;
  *) Usage >&2; exit 2 ;;
esac

DetectDistribution
LoadPackageManifest

case $ACTION in
  --print)
    PrintPackages
    ;;
  --check)
    FindMissingPackages
    if [ -n "$MISSING_PACKAGES" ]; then
      printf '%s: missing %s packages:%s\n' "$PROGRAM" "$DISTRO" \
        "$MISSING_PACKAGES" >&2
      exit 1
    fi
    ;;
  --install)
    FindMissingPackages
    if [ -z "$MISSING_PACKAGES" ]; then
      printf 'Mocktail thin host dependencies are already installed.\n'
      exit 0
    fi
    InstallMissingPackages
    FindMissingPackages
    [ -z "$MISSING_PACKAGES" ] || {
      printf '%s: packages remain unavailable after installation:%s\n' \
        "$PROGRAM" "$MISSING_PACKAGES" >&2
      exit 1
    }
    ;;
esac
