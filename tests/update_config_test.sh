#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

set -Eeuo pipefail
umask 077

READER="${1:?update config reader is required}"
EXAMPLE="${2:?shipped example config is required}"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT
export HOME="${TEMP_DIR}/home"
mkdir -p -- "${HOME}"

[[ "$(python3 "${READER}" "${TEMP_DIR}/missing.yaml")" == '{}' ]]
example="$(python3 "${READER}" "${EXAMPLE}")"
[[ "$(jq -r .automatic <<<"${example}")" == true ]]
[[ "$(jq -r .source <<<"${example}")" == apk-pure ]]
[[ "$(jq -r .launch_after_update <<<"${example}")" == false ]]
[[ "$(jq -r 'has("version")' <<<"${example}")" == false ]]
[[ "$(jq -r 'has("testing_latest_only")' <<<"${example}")" == false ]]

cat > "${TEMP_DIR}/config.yaml" <<'EOF'
version: 1
updates:
  automatic: true
  testing_latest_only: false
EOF
deprecated_stderr="${TEMP_DIR}/deprecated.stderr"
deprecated="$(python3 "${READER}" "${TEMP_DIR}/config.yaml" \
  2>"${deprecated_stderr}")"
[[ "$(jq -r 'has("testing_latest_only")' <<<"${deprecated}")" == false ]]
grep -Fq \
  'updates.testing_latest_only is no longer supported and is ignored' \
  "${deprecated_stderr}"

cat > "${TEMP_DIR}/config.yaml" <<'EOF'
version: 1
updates:
  version: 2.734.917
EOF
pinned="$(python3 "${READER}" "${TEMP_DIR}/config.yaml")"
[[ "$(jq -r .version <<<"${pinned}")" == 2.734.917 ]]

cat > "${TEMP_DIR}/config.yaml" <<'EOF'
version: 1
updates:
  version: invalid version
EOF
if python3 "${READER}" "${TEMP_DIR}/config.yaml" >/dev/null 2>&1; then
  echo "invalid pinned Roblox version was accepted" >&2
  exit 1
fi

printf '%s\n' 'version: 1' >"${TEMP_DIR}/symlink-target.yaml"
ln -s "${TEMP_DIR}/symlink-target.yaml" "${TEMP_DIR}/symlink.yaml"
if python3 "${READER}" "${TEMP_DIR}/symlink.yaml" >/dev/null 2>&1; then
  echo "symlink updater configuration was accepted" >&2
  exit 1
fi
cat > "${TEMP_DIR}/config.yaml" <<'EOF'
version: 1
updates:
  source: unknown-mirror
EOF
if python3 "${READER}" "${TEMP_DIR}/config.yaml" >/dev/null 2>&1; then
  echo "invalid updater source was accepted" >&2
  exit 1
fi

cat > "${TEMP_DIR}/config.yaml" <<'EOF'
!!python/object/apply:os.system ["false"]
EOF
if python3 "${READER}" "${TEMP_DIR}/config.yaml" >/dev/null 2>&1; then
  echo "unsafe YAML tag was accepted" >&2
  exit 1
fi

echo "updater YAML configuration checks passed"
