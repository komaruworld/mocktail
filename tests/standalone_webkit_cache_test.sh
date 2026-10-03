#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"
readonly TEST_ROOT="$(mktemp -d)"
trap 'rm -rf -- "${TEST_ROOT}"' EXIT

source "${ROOT}/scripts/package_standalone_webkit.sh"
GDK_PIXBUF_SOURCE="${TEST_ROOT}/source"
STAGING="${TEST_ROOT}/staging"
readonly CACHE_REL=lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders.cache
mkdir -p -- "${GDK_PIXBUF_SOURCE}" "${STAGING}/${CACHE_REL%/*}"
cat >"${GDK_PIXBUF_SOURCE}/loaders.cache" <<'EOF'
# LoaderDir = /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders
"/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/io-wmf.so"
"wmf" 0 "gdk-pixbuf" "Windows Metafile" "LGPL"
"/custom/loaders/extra-loader.so"
"xpm" 0 "gdk-pixbuf" "XPM" "LGPL"
"/* XPM */" "" 100
EOF
cat >"${TEST_ROOT}/expected.cache" <<'EOF'
# LoaderDir = lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders
"lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders/io-wmf.so"
"wmf" 0 "gdk-pixbuf" "Windows Metafile" "LGPL"
"lib/plugins/gdk-pixbuf-2.0/2.10.0/loaders/extra-loader.so"
"xpm" 0 "gdk-pixbuf" "XPM" "LGPL"
"/* XPM */" "" 100
EOF

WriteClassicPixbufCache
cmp -- "${TEST_ROOT}/expected.cache" "${STAGING}/${CACHE_REL}"

# Unknown absolute entries must also be rejected outside the usual /usr tree.
printf '"/custom/unrecognized-loader"\n' >"${GDK_PIXBUF_SOURCE}/loaders.cache"
if bash -c 'source "$1"; GDK_PIXBUF_SOURCE="$2"; STAGING="$3"; WriteClassicPixbufCache' \
    bash "${ROOT}/scripts/package_standalone_webkit.sh" \
    "${GDK_PIXBUF_SOURCE}" "${STAGING}" >"${TEST_ROOT}/rejected.log" 2>&1; then
  printf 'absolute GdkPixbuf loader path was accepted\n' >&2
  exit 1
fi
grep -Fq 'absolute loader path' "${TEST_ROOT}/rejected.log"
