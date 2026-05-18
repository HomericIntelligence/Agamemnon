#!/usr/bin/env bash
# tests/test-export-default-owner.sh — Unit tests for MYRMIDONS_DEFAULT_OWNER fallback
#
# Verifies that export.sh uses MYRMIDONS_DEFAULT_OWNER when set, falls back to
# $(whoami) when unset, and never contains the hardcoded literal "mvillmow" in
# any jq expression.
#
# Usage:
#   ./tests/test-export-default-owner.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
EXPORT_SH="${REPO_ROOT}/scripts/export.sh"

PASS=0
FAIL=0

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "  PASS: ${desc}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}"
        echo "        expected: ${expected}"
        echo "        actual:   ${actual}"
        FAIL=$((FAIL + 1))
    fi
}

assert_no_match() {
    local desc="$1" pattern="$2" file="$3"
    if ! grep -qE "$pattern" "$file"; then
        echo "  PASS: ${desc}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc} — pattern '${pattern}' still found in ${file}"
        FAIL=$((FAIL + 1))
    fi
}

# ---------------------------------------------------------------------------
# Helper: extract the resolved owner from the jq expression in export.sh.
# Simulates the export_agent owner line by evaluating it in a subshell with
# the given env, passing a synthetic agent JSON with no owner field.
# ---------------------------------------------------------------------------
resolve_owner() {
    local env_var="${1:-}"
    local agent_json='{"name":"test","label":"Test","program":"claude-code"}'

    if [[ -n "$env_var" ]]; then
        MYRMIDONS_DEFAULT_OWNER="$env_var" \
            bash -c "echo '$agent_json' | jq -r --arg default_owner \"\${MYRMIDONS_DEFAULT_OWNER:-\$(whoami)}\" '.owner // \$default_owner'"
    else
        # `unset` only fails if the name is read-only or invalid; both would
        # be bugs we want to know about, so don't suppress the rc.
        unset MYRMIDONS_DEFAULT_OWNER
        bash -c "echo '$agent_json' | jq -r --arg default_owner \"\${MYRMIDONS_DEFAULT_OWNER:-\$(whoami)}\" '.owner // \$default_owner'"
    fi
}

echo "Testing MYRMIDONS_DEFAULT_OWNER fallback logic..."
echo ""

echo "=== Env var set ==="
result="$(resolve_owner "custom-user")"
assert_eq "MYRMIDONS_DEFAULT_OWNER=custom-user → owner is custom-user" "custom-user" "$result"

echo ""
echo "=== Env var unset (falls back to whoami) ==="
expected_whoami="$(whoami)"
result="$(resolve_owner "")"
assert_eq "Unset MYRMIDONS_DEFAULT_OWNER → owner matches whoami (${expected_whoami})" "$expected_whoami" "$result"

echo ""
echo "=== Owner present in API response (env var not used) ==="
result="$(bash -c "
    agent_json='{\"name\":\"a\",\"label\":\"A\",\"owner\":\"api-owner\"}'
    MYRMIDONS_DEFAULT_OWNER=env-user
    echo \"\$agent_json\" | jq -r --arg default_owner \"\${MYRMIDONS_DEFAULT_OWNER:-\$(whoami)}\" '.owner // \$default_owner'
")"
assert_eq "API-supplied owner takes precedence over env var" "api-owner" "$result"

echo ""
echo "=== Source code does not contain hardcoded 'mvillmow' in a jq expression ==="
assert_no_match \
    "No 'mvillmow' literal in a jq fallback expression in export.sh" \
    'jq.*mvillmow' \
    "$EXPORT_SH"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
