#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"
readonly CXX="${2:-c++}"
readonly TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mocktail-anylinux-test.XXXXXX")"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT

source "${ROOT}/scripts/package_anylinux.sh"
mkdir -p -- "${TEMP_DIR}/fixtures"

# Compile the real adapter loader and updater canary. The fake runtime
# deliberately has no Android adapters next to its relocated executable.
"${CXX}" -std=c++17 -I"${ROOT}/include" -x c++ - \
  "${ROOT}/src/graphics/bionic_egl_bridge.cc" \
  "${ROOT}/src/runtime/supported_launch_policy.cc" \
  "${ROOT}/src/update/readiness_canary.cc" -ldl -pthread \
  -o "${TEMP_DIR}/fixtures/mocktail" <<'EOF'
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include "mocktail/graphics/bionic_egl_bridge.h"
#include "runtime/supported_launch_policy.h"
#include "update/readiness_canary.h"
int main(int argc, char** argv) {
  std::string error;
  if (!mocktail::runtime::ApplySupportedLaunchPolicy(false, &error)) return 4;
  const char* manifest = std::getenv("MOCKTAIL_COMPATIBILITY_MANIFEST");
  if (!manifest || std::string(manifest).empty()) return 5;
  std::cout << "manifest=" << manifest << std::endl;
  mocktail::graphics::BionicEglBridge bridge;
  if (!bridge.Load()) {
    std::cerr << bridge.error() << '\n';
    return 1;
  }
  const char* directory = std::getenv("MOCKTAIL_RUNTIME_LIBRARY_DIR");
  if (!directory || bridge.library_path() !=
      std::filesystem::path(directory) / "libEGL.so") return 2;
  std::cout << "adapter=" << bridge.library_path() << std::endl;
  if (argc == 2 && std::string(argv[1]) == "--help") {
    mocktail::update::CanaryOptions options;
    options.runtime_binary = argv[0];
    options.payload_directory = directory;
    options.compatibility_manifest = std::filesystem::path(directory) / "candidate.json";
    options.cache_root = std::getenv("MOCKTAIL_TEST_CANARY_ROOT");
    options.state_root = options.cache_root / "state";
    options.timeout_seconds = 5;
    const auto result = mocktail::update::RunReadinessCanary(options);
    std::ifstream log(result.log_path);
    const std::string contents((std::istreambuf_iterator<char>(log)), {});
    std::cout << contents;
    // Only test loading through the real sanitized child environment;
    // synthetic output is not proof of rendered-frame readiness.
    return result.exit_code == 0 &&
        contents.find("manifest=" + options.compatibility_manifest.string()) != std::string::npos &&
        contents.find("canary adapter loaded") != std::string::npos ? 0 : 3;
  }
  std::cout << "canary adapter loaded\n";
}
EOF
"${CXX}" -std=c++17 -shared -fPIC "${ROOT}/stubs/libegl_stub.cc" \
  -ldl -o "${TEMP_DIR}/fixtures/libEGL.so"

# The packaging contract uses a recording quick-sharun, without changing
# the host /usr or downloading toolchain dependencies during a unit test.
cat >"${TEMP_DIR}/quick-sharun" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
if [[ "$1" == --make-appimage ]]; then
  [[ "$#" == 1 && -f "${APPDIR}/AppRun.sh" ]]
  [[ "${APPIMAGETOOL}" == /* && -x "${APPIMAGETOOL}" ]]
  printf 'make-appimage\n' >>"${MOCKTAIL_PACKAGING_FIXTURES}/calls"
  mkdir -p -- "${OUTPATH}"
  cp -- "$(type -P true)" "${OUTPATH}/${OUTNAME}"
  exit 0
fi
[[ "$#" == 7 && "$1" == /usr/bin/mocktail &&
   "$2" == /usr/lib/mocktail/mocktail_webview_helper &&
   "$3" == /usr/lib/mocktail/mocktail_failure_dialog &&
   "$4" == /usr/lib/mocktail/mocktail_updater &&
   "$5" == /usr/lib/mocktail && "$6" == /usr/share/mocktail &&
   "$7" == /usr/bin/bash ]]
[[ "${DESKTOP}" == /usr/share/applications/space.bigrat.mocktail.desktop ]]
[[ "${MAIN_BIN}" == mocktail && "${STRACE_MODE}" == 1 ]]
[[ "${STRACE_BINARY}" == mocktail && "${STRACE_FLAGS}" == --help ]]
[[ -z "${NO_STRIP:-}" ]]
if [[ -n "${SHARUN_LINK:-}" ]]; then
  wget -qO "${MOCKTAIL_PACKAGING_FIXTURES}/downloaded-sharun" "${SHARUN_LINK}"
  cmp "${MOCKTAIL_ANYLINUX_SHARUN}" "${MOCKTAIL_PACKAGING_FIXTURES}/downloaded-sharun"
  wget -qO "${MOCKTAIL_PACKAGING_FIXTURES}/downloaded-source" "${ANYLINUX_LIB_SOURCE}"
  cmp "${MOCKTAIL_ANYLINUX_C_SOURCE}" "${MOCKTAIL_PACKAGING_FIXTURES}/downloaded-source"
fi
# Only runtime metadata may be preseeded: no libs, custom AppRun, or bundle.
[[ "$(find "${APPDIR}" -mindepth 1 -maxdepth 1 -printf '%f\n')" == .env ]]
printf 'deploy-usr\n' >>"${MOCKTAIL_PACKAGING_FIXTURES}/calls"
mkdir -p "${APPDIR}/bin" "${APPDIR}/lib/mocktail" \
  "${APPDIR}/lib/webkitgtk-6.0/injected-bundle" "${APPDIR}/share/mocktail/metadata"
cp -- "${MOCKTAIL_PACKAGING_FIXTURES}/mocktail" "${APPDIR}/bin/mocktail"
cp -- "${MOCKTAIL_PACKAGING_FIXTURES}/libEGL.so" "${APPDIR}/lib/mocktail/libEGL.so"
cp -- "${MOCKTAIL_PACKAGING_FIXTURES}/libEGL.so" "${APPDIR}/lib/mocktail/libvulkan.so"
cp -- "${MOCKTAIL_PACKAGING_FIXTURES}/libEGL.so" "${APPDIR}/lib/mocktail/libmediandk.so"
for metadata in roblox_compatibility roblox_bootstrap_sources \
    roblox_host_abi_reference roblox_signing_certificates; do
  printf '{}\n' >"${APPDIR}/share/mocktail/metadata/${metadata}.json"
done
for name in mocktail_updater mocktail_failure_dialog mocktail_webview_helper \
    WebKitNetworkProcess WebKitWebProcess WebKitGPUProcess; do
  cp -- "$(type -P true)" "${APPDIR}/bin/${name}"
done
for name in WebKitNetworkProcess WebKitWebProcess WebKitGPUProcess; do
  ln -s -- "../../bin/${name}" "${APPDIR}/lib/webkitgtk-6.0/${name}"
done
printf 'injected bundle fixture\n' \
  >"${APPDIR}/lib/webkitgtk-6.0/injected-bundle/libwebkitgtkinjectedbundle.so"
printf '#!/bin/sh\n' >"${APPDIR}/AppRun.sh"
printf '#!/bin/sh\n' >"${APPDIR}/AppRun.lib"
EOF
chmod 0755 "${TEMP_DIR}/quick-sharun"
mkdir -p "${TEMP_DIR}/fake-bin" "${TEMP_DIR}/local inputs"
cat >"${TEMP_DIR}/fake-bin/wget" <<'EOF'
#!/usr/bin/env bash
printf 'unadapted wget must not be used for local inputs\n' >&2
exit 1
EOF
chmod 0755 "${TEMP_DIR}/fake-bin/wget"
cp -- "$(type -P true)" "${TEMP_DIR}/fake-bin/fixture-appimagetool"
cp -- "$(type -P true)" "${TEMP_DIR}/local inputs/sharun"
printf 'local source fixture\n' >"${TEMP_DIR}/local inputs/anylinux.c"
export PATH="${TEMP_DIR}/fake-bin:${PATH}"
export MOCKTAIL_ANYLINUX_SHARUN="${TEMP_DIR}/local inputs/sharun"
export MOCKTAIL_ANYLINUX_C_SOURCE="${TEMP_DIR}/local inputs/anylinux.c"
export MOCKTAIL_PACKAGING_FIXTURES="${TEMP_DIR}/fixtures"
ANYLINUX_TOOL="${TEMP_DIR}/quick-sharun"
ANYLINUX_APPIMAGETOOL="${TEMP_DIR}/fake-bin/fixture-appimagetool"
ANYLINUX_WORK="${TEMP_DIR}/packaging"
ANYLINUX_OUTPUT="${TEMP_DIR}/Mocktail.AppImage"
mkdir -p -- "${ANYLINUX_WORK}"
AnyLinuxVerifyInstalled() { :; }
AnyLinuxDeploy
[[ -x "${ANYLINUX_OUTPUT}" ]]
[[ "$(<"${TEMP_DIR}/fixtures/calls")" == $'deploy-usr\nmake-appimage' ]]

mv -- "${ANYLINUX_WORK}/AppDir" "${TEMP_DIR}/relocated AppDir"
app_dir="${TEMP_DIR}/relocated AppDir"
AnyLinuxVerifyAppDir "${app_dir}"
output="$(
  export SHARUN_DIR="${app_dir}" MOCKTAIL_TEST_CANARY_ROOT="${TEMP_DIR}/canary"
  export MOCKTAIL_RUNTIME_LIBRARY_DIR="${TEMP_DIR}/stale-runtime"
  export XDG_CACHE_HOME="${TEMP_DIR}/user-cache"
  export MOCKTAIL_COMPATIBILITY_MANIFEST="${TEMP_DIR}/candidate-override.json"
  set -a
  source "${app_dir}/.env"
  set +a
  [[ "${USE_HOST_XDG_CACHE_HOME}" == 1 &&
     "${XDG_CACHE_HOME}" == "${TEMP_DIR}/user-cache" ]]
  [[ "${MOCKTAIL_WEBVIEW_HELPER}" == "${app_dir}/bin/mocktail_webview_helper" ]]
  [[ "${MOCKTAIL_UPDATE_SIGNING_TRUST_PATH}" == \
     "${app_dir}/share/mocktail/metadata/roblox_signing_certificates.json" ]]
  [[ "${MOCKTAIL_COMPATIBILITY_MANIFEST}" == "${TEMP_DIR}/candidate-override.json" ]]
  [[ "${MOCKTAIL_PACKAGED_COMPATIBILITY_MANIFEST}" == \
     "${app_dir}/share/mocktail/metadata/roblox_compatibility.json" ]]
  "${MOCKTAIL_BIN}" --help
)"
grep -Fxq "adapter=${app_dir}/lib/mocktail/libEGL.so" <<<"${output}"
grep -Fxq 'canary adapter loaded' <<<"${output}"

mkdir "${app_dir}/usr"
if (AnyLinuxVerifyAppDir "${app_dir}") >"${TEMP_DIR}/unexpected-usr.log" 2>&1; then
  printf 'packager accepted an old /usr-prefixed AppDir\n' >&2
  exit 1
fi
rmdir "${app_dir}/usr"
mv -- "${app_dir}/bin/WebKitNetworkProcess" "${TEMP_DIR}/WebKitNetworkProcess"
if (AnyLinuxVerifyAppDir "${app_dir}") >"${TEMP_DIR}/missing-process.log" 2>&1; then
  printf 'packager accepted a missing WebKit process\n' >&2
  exit 1
fi
grep -Fq 'WebKitNetworkProcess' "${TEMP_DIR}/missing-process.log"
mv -- "${TEMP_DIR}/WebKitNetworkProcess" "${app_dir}/bin/WebKitNetworkProcess"
mv -- "${app_dir}/share/mocktail/metadata/roblox_signing_certificates.json" \
  "${TEMP_DIR}/signing-certificates.json"
if (AnyLinuxVerifyAppDir "${app_dir}") >"${TEMP_DIR}/missing-metadata.log" 2>&1; then
  printf 'packager accepted missing signature verification metadata\n' >&2
  exit 1
fi
grep -Fq 'roblox_signing_certificates.json' "${TEMP_DIR}/missing-metadata.log"

prefix="${TEMP_DIR}/install/usr"
mkdir -p -- "${prefix}/lib/mocktail"
printf 'FreeBSD helper fixture\n' >"${prefix}/lib/mocktail/mocktail_freebsd_socket_helper"
AnyLinuxPrepareInstallTree "${prefix}"
[[ ! -e "${prefix}/lib/mocktail/mocktail_freebsd_socket_helper" ]]
grep -Fxq 'FreeBSD helper fixture' \
  "${prefix}/share/mocktail/helpers/mocktail_freebsd_socket_helper"

# Exercise public AnyLinux dispatch and the unprivileged install path. A fake CMake
# installer refuses to write without DESTDIR; the real bwrap overlay must
# make those files visible under /usr to the real installed-layout checks.
if command -v bwrap >/dev/null &&
    bwrap --ro-bind / / -- /usr/bin/true 2>/dev/null; then
  mkdir -p "${TEMP_DIR}/fake-bin" "${TEMP_DIR}/build"
  touch "${TEMP_DIR}/build/cmake_install.cmake"
  cat >"${TEMP_DIR}/fake-bin/cmake" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
[[ "$#" == 4 && "$1" == --install && "$3" == --prefix && "$4" == /usr ]]
[[ -n "${DESTDIR:-}" && "${DESTDIR}" == */install ]]
prefix="${DESTDIR}/usr"
mkdir -p "${prefix}/bin" "${prefix}/lib/mocktail" "${prefix}/share/mocktail/metadata"
cp -- "${MOCKTAIL_PACKAGING_FIXTURES}/mocktail" "${prefix}/bin/mocktail"
for helper in mocktail_updater mocktail_failure_dialog mocktail_webview_helper; do
  cp -- "$(type -P true)" "${prefix}/lib/mocktail/${helper}"
done
for adapter in libEGL.so libvulkan.so libmediandk.so; do
  cp -- "${MOCKTAIL_PACKAGING_FIXTURES}/libEGL.so" "${prefix}/lib/mocktail/${adapter}"
done
for metadata in roblox_compatibility roblox_bootstrap_sources \
    roblox_host_abi_reference roblox_signing_certificates; do
  printf '{}\n' >"${prefix}/share/mocktail/metadata/${metadata}.json"
done
EOF
  chmod 0755 "${TEMP_DIR}/fake-bin/cmake"
  PATH="${TEMP_DIR}/fake-bin:${PATH}" \
    MOCKTAIL_APPIMAGE_FORMAT=anylinux \
    MOCKTAIL_ANYLINUX_PACKAGER="${TEMP_DIR}/quick-sharun" \
    MOCKTAIL_ANYLINUX_APPIMAGETOOL=fixture-appimagetool \
    MOCKTAIL_ANDROID_BUILD_TOOLS_ROOT="${TEMP_DIR}/not-installed" \
    "${ROOT}/scripts/package_portable.sh" --build-dir "${TEMP_DIR}/build" \
      --libc glibc --mode full --output "${TEMP_DIR}/no-portable-tree" \
      --appimage "${TEMP_DIR}/overlay.AppImage"
  [[ -x "${TEMP_DIR}/overlay.AppImage" ]]
  [[ ! -e "${TEMP_DIR}/no-portable-tree" ]]
else
  printf 'SKIP: private /usr overlay test needs unprivileged bwrap\n'
fi

printf 'AnyLinux packaging test passed\n'
