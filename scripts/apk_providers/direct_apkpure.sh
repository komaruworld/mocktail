#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PACKAGE=""
VERSION="2.734.917"
OUTPUT=""
CHECK=false
while (( $# > 0 )); do
  case "$1" in
    --package) PACKAGE="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    --check) CHECK=true; shift ;;
    --source) shift 2 ;;
    *) echo "direct APKPure provider: unknown option: $1" >&2; exit 1 ;;
  esac
done
arguments=(--package "${PACKAGE}" --arch x86_64)
if [[ "${CHECK}" == true ]]; then
  arguments+=(--check)
else
  arguments+=(--output "${OUTPUT}")
fi
[[ -z "${VERSION}" ]] || arguments+=(--version "${VERSION}")
exec python3 "${SCRIPT_DIR}/direct_apkpure.py" "${arguments[@]}"
