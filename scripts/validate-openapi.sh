#!/usr/bin/env bash
# Validates docs/api/openapi.yaml with @stoplight/spectral-cli.
# Requires: npx (Node.js >= 18) or a globally installed spectral binary.
# Exit codes: 0 = valid, non-zero = validation errors.
#
# NOTE: @stoplight/spectral-core 1.23.0 (and its bundled spectral-rulesets
# 1.22.3) regressed the built-in spectral:oas ruleset, crashing with
# "Cannot read properties of null (reading 'enum')" on otherwise-valid specs.
# We therefore pin spectral-core/spectral-rulesets to the last known-good
# versions via npm `overrides`. This is a tooling pin only -- the lint rules
# applied are unchanged (spectral:oas). Remove the overrides once the upstream
# regression is fixed.
set -euo pipefail

SPEC="${1:-docs/api/openapi.yaml}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPEC_PATH="${REPO_ROOT}/${SPEC}"

if [[ ! -f "${SPEC_PATH}" ]]; then
  echo "ERROR: spec not found at ${SPEC_PATH}" >&2
  exit 1
fi

RULESET="${REPO_ROOT}/.spectral.yaml"

# Pinned, known-good Spectral toolchain (see NOTE above).
SPECTRAL_CLI_VERSION="6.16.0"
SPECTRAL_CORE_VERSION="1.22.0"
SPECTRAL_RULESETS_VERSION="1.22.2"

if command -v spectral &>/dev/null; then
  spectral lint --ruleset "${RULESET}" "${SPEC_PATH}"
else
  # Install Spectral into an isolated, pinned workspace so the broken
  # spectral-core@1.23.0 cannot be pulled in via floating transitive ranges.
  RUNNER_DIR="$(mktemp -d)"
  trap 'rm -rf "${RUNNER_DIR}"' EXIT

  cat >"${RUNNER_DIR}/package.json" <<EOF
{
  "name": "spectral-runner",
  "private": true,
  "dependencies": { "@stoplight/spectral-cli": "${SPECTRAL_CLI_VERSION}" },
  "overrides": {
    "@stoplight/spectral-core": "${SPECTRAL_CORE_VERSION}",
    "@stoplight/spectral-rulesets": "${SPECTRAL_RULESETS_VERSION}"
  }
}
EOF

  (cd "${RUNNER_DIR}" && npm install --no-audit --no-fund --silent)
  "${RUNNER_DIR}/node_modules/.bin/spectral" lint --ruleset "${RULESET}" "${SPEC_PATH}"
fi
