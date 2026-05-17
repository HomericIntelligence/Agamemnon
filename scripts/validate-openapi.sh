#!/usr/bin/env bash
# Validates docs/api/openapi.yaml with @stoplight/spectral-cli.
# Requires: npx (Node.js >= 18) or a globally installed spectral binary.
# Exit codes: 0 = valid, non-zero = validation errors.
set -euo pipefail

SPEC="${1:-docs/api/openapi.yaml}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPEC_PATH="${REPO_ROOT}/${SPEC}"

if [[ ! -f "${SPEC_PATH}" ]]; then
  echo "ERROR: spec not found at ${SPEC_PATH}" >&2
  exit 1
fi

RULESET="${REPO_ROOT}/.spectral.yaml"

if command -v spectral &>/dev/null; then
  spectral lint --ruleset "${RULESET}" "${SPEC_PATH}"
else
  npx --yes @stoplight/spectral-cli@latest lint --ruleset "${RULESET}" "${SPEC_PATH}"
fi
