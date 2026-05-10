#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

mkdir -p "${ROOT_DIR}/build/coverage-report"

gcovr \
  --root "${ROOT_DIR}" \
  --filter "${ROOT_DIR}/include" \
  --filter "${ROOT_DIR}/src" \
  --exclude "${ROOT_DIR}/src/server_main.cpp" \
  --exclude "${ROOT_DIR}/src/nats_client.cpp" \
  --html-details "${ROOT_DIR}/build/coverage-report/index.html" \
  --xml "${ROOT_DIR}/build/coverage-report/coverage.xml" \
  --print-summary \
  "${ROOT_DIR}/build/coverage"
