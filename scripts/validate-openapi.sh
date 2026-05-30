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

# Pin the Spectral CLI and the spectral:oas ruleset to known-good versions.
# Floating to @latest pulled @stoplight/spectral-rulesets@1.22.3, whose oas
# ruleset crashes nimma with "Cannot read properties of null (reading 'enum')"
# on otherwise-valid specs. Pinning the ruleset back to 1.22.2 (the last good
# release) restores deterministic validation. See CI regression 2026-05-30.
SPECTRAL_CLI_VERSION="6.16.0"
SPECTRAL_RULESETS_VERSION="1.22.2"

if command -v spectral &>/dev/null; then
  spectral lint --ruleset "${RULESET}" "${SPEC_PATH}"
else
  # npx alone cannot pin a transitive dependency, so install the CLI into a
  # throwaway directory with an npm "overrides" entry that forces the good
  # ruleset version, then run the locally-installed binary.
  RUNNER_DIR="$(mktemp -d)"
  trap 'rm -rf "${RUNNER_DIR}"' EXIT
  cat > "${RUNNER_DIR}/package.json" <<EOF
{
  "name": "spectral-runner",
  "private": true,
  "overrides": { "@stoplight/spectral-rulesets": "${SPECTRAL_RULESETS_VERSION}" }
}
EOF
  (cd "${RUNNER_DIR}" && npm install --no-audit --no-fund --silent \
    "@stoplight/spectral-cli@${SPECTRAL_CLI_VERSION}")
  node "${RUNNER_DIR}/node_modules/@stoplight/spectral-cli/dist/index.js" \
    lint --ruleset "${RULESET}" "${SPEC_PATH}"
fi
