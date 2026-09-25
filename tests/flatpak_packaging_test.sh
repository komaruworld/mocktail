#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"
readonly MANIFEST="${ROOT}/packaging/flatpak/space.bigrat.mocktail.json"
readonly BUILD_HELPER="${ROOT}/scripts/build_flatpak.sh"
readonly PAGES_HELPER="${ROOT}/scripts/assemble_flatpak_pages.sh"
readonly GITHUB_CI="${ROOT}/.github/workflows/flatpak.yml"
readonly TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT

Fail() {
  printf 'Flatpak packaging test failed: %s\n' "$*" >&2
  exit 1
}

python3 - "${ROOT}/CMakeLists.txt" \
    "${ROOT}/packaging/space.bigrat.mocktail.metainfo.xml" <<'PY'
import re
import sys
import xml.etree.ElementTree as ElementTree

with open(sys.argv[1], encoding="utf-8") as source:
    project = re.search(r"project\(Mocktail\s+VERSION\s+([0-9.]+)", source.read())
assert project, "CMakeLists.txt has no project VERSION"
releases = ElementTree.parse(sys.argv[2]).getroot().find("releases")
assert releases is not None and len(releases), "metainfo has no releases"
newest = releases[0].get("version")
assert newest == project.group(1), (
    f"metainfo newest release {newest} does not match project version "
    f"{project.group(1)}; add a <release> entry")
PY

python3 - "${MANIFEST}" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    manifest = json.load(source)

assert manifest["app-id"] == "space.bigrat.mocktail"
assert manifest["runtime"] == "org.gnome.Platform"
assert manifest["runtime-version"] == "50"
assert manifest["sdk"] == "org.gnome.Sdk"
assert manifest["command"] == "mocktail"
assert "sdk-extensions" not in manifest

finish_args = set(manifest["finish-args"])
required_permissions = {
    "--share=network",
    "--socket=wayland",
    "--socket=fallback-x11",
    "--socket=pulseaudio",
    "--device=dri",
    "--filesystem=xdg-run/discord-ipc-0:rw",
}
assert required_permissions <= finish_args
assert not any(argument.startswith("--filesystem=host") for argument in finish_args)
assert not any(argument == "--filesystem=home" for argument in finish_args)

modules = {module["name"]: module for module in manifest["modules"]}
for required in (
    "sdl3",
    "sdl3-ttf",
    "nlohmann-json",
    "utf8proc",
    "minizip",
    "capstone",
    "libplacebo",
    "mocktail",
):
    assert required in modules
for forbidden in (
    "python-updater-dependencies",
    "ripgrep",
    "binutils-readelf",
    "java-runtime",
    "android-build-tools",
):
    assert forbidden not in modules

sdl_source = modules["sdl3"]["sources"][0]
assert "3.4." in sdl_source["url"]
assert re.fullmatch(r"[0-9a-f]{64}", sdl_source["sha256"])

sdl_options = set(modules["sdl3"]["config-opts"])
assert "-DCMAKE_INSTALL_LIBDIR=lib" in sdl_options

sdl_ttf_options = set(modules["sdl3-ttf"]["config-opts"])
assert "-DCMAKE_INSTALL_LIBDIR=lib" in sdl_ttf_options
assert "-DSDLTTF_VENDORED=OFF" in sdl_ttf_options
assert "-DSDLTTF_SAMPLES=OFF" in sdl_ttf_options

capstone_options = set(modules["capstone"]["config-opts"])
assert modules["capstone"]["builddir"] is True
assert "-DCMAKE_INSTALL_LIBDIR=lib" in capstone_options
assert "-DBUILD_SHARED_LIBS=ON" in capstone_options
assert "-DCAPSTONE_X86_SUPPORT=ON" in capstone_options
assert "-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF" in capstone_options

libplacebo_options = set(modules["libplacebo"]["config-opts"])
assert "--libdir=lib" in libplacebo_options

assert manifest["build-options"]["strip"] is True
assert manifest["build-options"]["no-debuginfo"] is True
assert "env" not in manifest["build-options"]

project_sources = modules["mocktail"]["sources"]
assert project_sources == [
    {"type": "git", "path": "../..", "branch": "main"}
]
mocktail_options = set(modules["mocktail"]["config-opts"])
assert "-DBUILD_TESTING=OFF" in mocktail_options
assert "-DMOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER=OFF" in mocktail_options
assert "-DCMAKE_INSTALL_LIBDIR=lib" in mocktail_options
assert (
    "-DMOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST="
    "/app/share/mocktail/metadata/roblox_compatibility.json"
) in mocktail_options
assert (
    "-DMOCKTAIL_DEFAULT_SIGNING_TRUST_MANIFEST="
    "/app/share/mocktail/metadata/roblox_signing_certificates.json"
) in mocktail_options

for module in manifest["modules"]:
    for source in module.get("sources", []):
        assert source["type"] != "dir", "broad worktree copies can include credentials"
        if "url" in source:
            assert re.fullmatch(r"[0-9a-f]{64}", source.get("sha256", ""))
PY

[[ ! -e "${ROOT}/packaging/flatpak/mocktail-flatpak-launcher.sh" ]] ||
  Fail 'Flatpak still uses a shell launcher'
grep -Fq 'mocktail_updater' "${ROOT}/CMakeLists.txt" ||
  Fail 'native updater is not installed with Mocktail'
grep -Fq 'CleanupStaleBuilderMounts' "${BUILD_HELPER}" ||
  Fail 'build helper does not recover a stale sandboxed builder mount'

[[ -f "${GITHUB_CI}" && ! -L "${GITHUB_CI}" ]] ||
  Fail 'GitHub Actions configuration is missing or unsafe'
[[ ! -e "${ROOT}/.gitlab-ci.yml" ]] ||
  Fail 'obsolete GitLab CI configuration is still present'
grep -Fq 'name: Flatpak' "${GITHUB_CI}" ||
  Fail 'GitHub Actions workflow has no stable display name'
validates_main=false
if grep -Fq 'branches: [main]' "${GITHUB_CI}"; then
  validates_main=true
elif grep -Fq 'workflow_run:' "${GITHUB_CI}" &&
     grep -Fq 'workflows: [Native packages]' "${GITHUB_CI}" &&
     grep -Fq "head_branch == 'main'" "${GITHUB_CI}"; then
  validates_main=true
fi
[[ "${validates_main}" == true ]] ||
  Fail 'GitHub Actions does not validate the main branch'
grep -Fq 'pull_request:' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not validate pull requests'
grep -Fq 'workflow_dispatch:' "${GITHUB_CI}" ||
  Fail 'GitHub Actions cannot be started manually'
grep -Fq "'assets/screenshots/**'" "${GITHUB_CI}" ||
  Fail 'Flatpak workflow does not react to screenshot changes'
grep -Fq "'packaging/discord-join.html'" "${GITHUB_CI}" ||
  Fail 'Flatpak workflow does not publish Discord join-page changes'
grep -Fq 'contents: read' "${GITHUB_CI}" ||
  Fail 'GitHub Actions permissions are not read-only'
grep -Fq 'submodules: recursive' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not initialize pinned source dependencies'
grep -Fq 'runs-on: ubuntu-24.04' "${GITHUB_CI}" ||
  Fail 'GitHub Actions runner is not pinned'
grep -Fq -- '--jobs=4' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not cap Flatpak build parallelism'
grep -Fq -- '--install-deps-from=flathub' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not install manifest dependencies from Flathub'
grep -Fq -- '--default-branch=stable' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not produce the stable Flatpak branch'
grep -Fq 'Mocktail-x86_64.flatpak' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not build the installable Flatpak bundle'
grep -Fq 'Mocktail-aarch64.flatpak' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not build the aarch64 Flatpak bundle'
grep -Fq 'ubuntu-24.04-arm' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not build Flatpak on an ARM64 runner'
grep -Fq 'flatpak build-import-bundle' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not merge both architectures into one repository'
grep -Fq 'actions/upload-artifact@' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not publish the Flatpak artifact'
grep -Fq 'actions/upload-pages-artifact@' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not publish the signed repository to Pages'
grep -Fq 'actions/deploy-pages@' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not deploy the Pages artifact'
grep -Fq 'FLATPAK_GPG_PRIVATE_KEY' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not import the Flatpak signing key'
grep -Fq 'flatpak build-update-repo' "${GITHUB_CI}" ||
  Fail 'GitHub Actions does not finalize the signed Flatpak repository'
grep -Fq 'retention-days: 14' "${GITHUB_CI}" ||
  Fail 'GitHub Actions artifact retention is not bounded'
if grep -Fq 'pull_request_target:' "${GITHUB_CI}"; then
  Fail 'GitHub Actions uses the unsafe pull_request_target trigger'
fi

dry_run="$(
  make -C "${ROOT}" --no-print-directory --dry-run flatpak \
    FLATPAK_BUILD_DIR=build-flatpak-test | tr -d '"'
)"
grep -Fq './scripts/build_flatpak.sh' <<<"${dry_run}" ||
  Fail 'make flatpak does not invoke the guarded Flatpak builder'
grep -Fq -- '--manifest packaging/flatpak/space.bigrat.mocktail.json' \
  <<<"${dry_run}" || Fail 'make flatpak lost the canonical manifest'
grep -Fq -- '--jobs 4' <<<"${dry_run}" ||
  Fail 'make flatpak does not cap local build parallelism'

mkdir -p -- "${TEMP_DIR}/bin"
cat >"${TEMP_DIR}/bin/uname" <<'EOF'
#!/usr/bin/env bash
printf 'x86_64\n'
EOF
cat >"${TEMP_DIR}/bin/flatpak" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod 0755 "${TEMP_DIR}/bin/uname" "${TEMP_DIR}/bin/flatpak"

set +e
PATH="${TEMP_DIR}/bin:/usr/bin:/bin" \
  "${BUILD_HELPER}" --build-dir "${ROOT}/build-flatpak-test" \
  >"${TEMP_DIR}/stdout" 2>"${TEMP_DIR}/stderr"
helper_status=$?
set -e
[[ "${helper_status}" -ne 0 ]] || Fail 'helper accepted a missing builder'
grep -Fq 'flatpak-builder is unavailable' "${TEMP_DIR}/stderr" ||
  Fail 'helper did not explain how to install flatpak-builder'

cat >"${TEMP_DIR}/bin/uname" <<'EOF'
#!/usr/bin/env bash
printf 'aarch64\n'
EOF
cat >"${TEMP_DIR}/bin/flatpak-builder" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"${MOCKTAIL_TEST_BUILDER_ARGS}"
EOF
chmod 0755 "${TEMP_DIR}/bin/flatpak-builder"
PATH="${TEMP_DIR}/bin:/usr/bin:/bin" \
  MOCKTAIL_TEST_BUILDER_ARGS="${TEMP_DIR}/builder-args" \
  "${BUILD_HELPER}" --build-dir "${ROOT}/build-flatpak-test"
grep -Fxq -- '--arch=aarch64' "${TEMP_DIR}/builder-args" ||
  Fail 'local Flatpak helper did not select aarch64'

mkdir -p -- "${TEMP_DIR}/repo/objects"
mkdir -p -- \
  "${TEMP_DIR}/native-repositories/apt" \
  "${TEMP_DIR}/native-repositories/rpm" \
  "${TEMP_DIR}/native-repositories/downloads"
printf 'bundle\n' >"${TEMP_DIR}/Mocktail-x86_64.flatpak"
printf 'bundle\n' >"${TEMP_DIR}/Mocktail-aarch64.flatpak"
printf 'public-key\n' >"${TEMP_DIR}/mocktail-flatpak.gpg"
printf 'public-key\n' \
  >"${TEMP_DIR}/native-repositories/mocktail-packages.gpg"
"${PAGES_HELPER}" \
  "${TEMP_DIR}/repo" \
  "${TEMP_DIR}/Mocktail-x86_64.flatpak" \
  "${TEMP_DIR}/Mocktail-aarch64.flatpak" \
  "${TEMP_DIR}/mocktail-flatpak.gpg" \
  "${TEMP_DIR}/public" \
  "${TEMP_DIR}/native-repositories"
grep -Fq 'Url=https://mocktail.bigrat.space/repo/' \
  "${TEMP_DIR}/public/mocktail.flatpakrepo" ||
  Fail 'published Flatpak repository URL is incorrect'
grep -Fq 'Name=space.bigrat.mocktail' \
  "${TEMP_DIR}/public/mocktail.flatpakref" ||
  Fail 'published Flatpak ref has the wrong application ID'
[[ -f "${TEMP_DIR}/public/Mocktail-x86_64.flatpak" &&
   -f "${TEMP_DIR}/public/Mocktail-aarch64.flatpak" ]] ||
  Fail 'Pages output does not include both Flatpak bundles'
grep -Fq 'flatpak install --user' "${TEMP_DIR}/public/index.html" ||
  Fail 'Pages landing page has no direct installation command'
grep -Fq 'Mocktail-aarch64.flatpak' "${TEMP_DIR}/public/index.html" ||
  Fail 'Pages landing page has no aarch64 Flatpak bundle link'
grep -Fq 'roblox://experiences/start' "${TEMP_DIR}/public/join.html" ||
  Fail 'Pages output has no Discord join bridge'
[[ -d "${TEMP_DIR}/public/apt" &&
   -d "${TEMP_DIR}/public/rpm" &&
   -d "${TEMP_DIR}/public/downloads" ]] ||
  Fail 'Pages output has no native package repositories'

printf 'Flatpak packaging contract test passed\n'
