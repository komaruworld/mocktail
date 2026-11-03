#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Regression guard: Mocktail ships with Roblox telemetry and crash uploads
# disabled. Fail if a tracking endpoint (experience signals, client event
# stream, time-spent pings, metrics) reappears in first-party sources, or if
# the JNI service-url fields that carry telemetry are ever set to a URL.

set -euo pipefail

ROOT="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LEGACY_RUNTIME="${ROOT}/src/legacy/legacy_runtime.cc"

# Tracking endpoint host/path fragments that must not appear in src/ or
# include/ (tests may quote them as blocked URLs, so tests are not scanned).
FORBIDDEN_FRAGMENTS=(
  "experience-signals-ingest"
  "ecsv2.roblox.com"
  "timespent"
  "metrics.roblox.com"
  "telemetryUrl\", \"http"
  "telemetryEndpoint\", \"http"
)

# JNI fields that must stay pinned to the empty kNoTelemetry constant.
FORBIDDEN_FIELDS=(
  "telemetryUrl"
  "telemetryEndpoint"
  "ecsv2Url"
  "ecsUrl"
  "eventStreamUrl"
  "timeSpentUrl"
)

status=0

while IFS= read -r -d '' file; do
  for fragment in "${FORBIDDEN_FRAGMENTS[@]}"; do
    if grep -qF -- "${fragment}" "${file}"; then
      echo "FAIL: telemetry fragment '${fragment}' found in ${file#"${ROOT}/"}" >&2
      status=1
    fi
  done
done < <(find "${ROOT}/src" "${ROOT}/include" -type f \
           \( -name '*.cc' -o -name '*.h' -o -name '*.c' \) -print0)

for field in "${FORBIDDEN_FIELDS[@]}"; do
  if ! grep -qF "SetStringField(env, object, \"${field}\", kNoTelemetry)" \
       "${LEGACY_RUNTIME}"; then
    echo "FAIL: ${field} is not pinned to kNoTelemetry in ${LEGACY_RUNTIME#"${ROOT}/"}" >&2
    status=1
  fi
  if grep -qE "SetStringField\(env, object, \"${field}\", k[A-Za-z]*Url\)" \
       "${LEGACY_RUNTIME}"; then
    echo "FAIL: ${field} is set to a URL constant in ${LEGACY_RUNTIME#"${ROOT}/"}" >&2
    status=1
  fi
done

if (( status != 0 )); then
  echo "no_telemetry_endpoints_test: FAILED" >&2
  exit 1
fi
echo "no_telemetry_endpoints_test passed"
