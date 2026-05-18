#!/usr/bin/env bash
# tests/test-report.sh — Unit tests for scripts/lib/report.sh
#
# Tests the JSON report builder functions in isolation (no live Agamemnon needed).
#
# Usage:
#   ./tests/test-report.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Source libs under test (api.sh defines AGAMEMNON_URL; reconcile.sh defines normalize_path)
AGAMEMNON_URL="${AGAMEMNON_URL:-http://localhost:8080}"
# shellcheck source=scripts/lib/reconcile.sh
source "${REPO_ROOT}/scripts/lib/reconcile.sh"
# shellcheck source=scripts/lib/report.sh
source "${REPO_ROOT}/scripts/lib/report.sh"

PASS=0
FAIL=0

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        pass "$desc"
    else
        fail "$desc (expected '${expected}', got '${actual}')"
    fi
}

assert_json_field() {
    local desc="$1" json="$2" jq_expr="$3" expected="$4"
    local actual
    actual="$(echo "$json" | jq -r "$jq_expr")"
    assert_eq "$desc" "$expected" "$actual"
}

assert_json_valid() {
    local desc="$1" json="$2"
    if echo "$json" | jq -e . > /dev/null 2>&1; then
        pass "$desc"
    else
        fail "$desc (invalid JSON: ${json:0:80})"
    fi
}

# ---------------------------------------------------------------------------
# Tests: report_init / report_emit (empty run)
# ---------------------------------------------------------------------------

echo ""
echo "=== report_init / report_emit ==="

report_init "hermes"
trap report_cleanup EXIT

result="$(report_emit 0 0 0 0 0 0 0)"
assert_json_valid "report_emit produces valid JSON" "$result"
assert_json_field "timestamp is non-empty" "$result" '.timestamp' "$(echo "$result" | jq -r '.timestamp')"
assert_json_field "host is hermes" "$result" '.host' "hermes"
assert_json_field "summary.created=0" "$result" '.summary.created' "0"
assert_json_field "summary.errors=0" "$result" '.summary.errors' "0"
assert_json_field "agents is empty array" "$result" '.agents | length' "0"
assert_json_field "unmanaged is empty array" "$result" '.unmanaged | length' "0"

# ---------------------------------------------------------------------------
# Tests: report_add_agent accumulates entries
# ---------------------------------------------------------------------------

echo ""
echo "=== report_add_agent ==="

report_init "hermes"

report_add_agent "alpha" "hermes" "UNCHANGED" "active" "online" "[]" ""
report_add_agent "beta"  "hermes" "CREATE"    "active" "MISSING" "[]" ""
report_add_agent "gamma" "hermes" "ERROR"     "active" "unknown" "[]" "create failed"

result="$(report_emit 0 1 0 0 1 0 1)"
assert_json_valid "report_emit with agents is valid JSON" "$result"
assert_json_field "agents length = 3" "$result" '.agents | length' "3"
assert_json_field "first agent name" "$result" '.agents[0].name' "alpha"
assert_json_field "first agent action" "$result" '.agents[0].action' "UNCHANGED"
assert_json_field "second agent action" "$result" '.agents[1].action' "CREATE"
assert_json_field "third agent error is non-null" "$result" '.agents[2].error' "create failed"
assert_json_field "summary.created=0" "$result" '.summary.created' "0"
assert_json_field "summary.updated=1" "$result" '.summary.updated' "1"
assert_json_field "summary.unchanged=1" "$result" '.summary.unchanged' "1"
assert_json_field "summary.errors=1" "$result" '.summary.errors' "1"

# ---------------------------------------------------------------------------
# Tests: report_add_unmanaged
# ---------------------------------------------------------------------------

echo ""
echo "=== report_add_unmanaged ==="

report_init "all"
report_add_unmanaged "orphan-1"
report_add_unmanaged "orphan-2"
result="$(report_emit 0 0 0 0 0 0 0)"
assert_json_field "unmanaged length = 2" "$result" '.unmanaged | length' "2"
assert_json_field "unmanaged[0] = orphan-1" "$result" '.unmanaged[0]' "orphan-1"
assert_json_field "unmanaged[1] = orphan-2" "$result" '.unmanaged[1]' "orphan-2"

# ---------------------------------------------------------------------------
# Tests: build_drift_json
# ---------------------------------------------------------------------------

echo ""
echo "=== build_drift_json ==="

actual_json='{"label":"Old Label","program":"claude-code","workingDirectory":"/old","programArgs":"","taskDescription":"Old desc","tags":["a","b"]}'

# UPDATE on label and workingDirectory
drift="$(build_drift_json "UPDATE:label,workingDirectory" "$actual_json" \
    "New Label" "claude-code" "/new" "" "Old desc" "a,b")"

assert_json_valid "build_drift_json produces valid JSON" "$drift"
assert_json_field "drift length = 2" "$drift" 'length' "2"
assert_json_field "drift[0].field = label" "$drift" '.[0].field' "label"
assert_json_field "drift[0].old = Old Label" "$drift" '.[0].old' "Old Label"
assert_json_field "drift[0].new = New Label" "$drift" '.[0].new' "New Label"
assert_json_field "drift[1].field = workingDirectory" "$drift" '.[1].field' "workingDirectory"
assert_json_field "drift[1].old = /old" "$drift" '.[1].old' "/old"
assert_json_field "drift[1].new = /new" "$drift" '.[1].new' "/new"

# Non-UPDATE action → empty drift array
drift_none="$(build_drift_json "UNCHANGED" "$actual_json" "lbl" "prg" "/dir" "" "" "")"
assert_json_field "non-UPDATE action → empty array" "$drift_none" 'length' "0"

# ---------------------------------------------------------------------------
# Tests: report_save creates file
# ---------------------------------------------------------------------------

echo ""
echo "=== report_save ==="

report_init "hermes"
json="$(report_emit 1 0 0 0 0 0 0)"
tmpdir="$(mktemp -d)"
report_save "$json" "$tmpdir"

if [[ -f "${tmpdir}/last-reconciliation.json" ]]; then
    pass "report_save creates last-reconciliation.json"
    saved_json="$(cat "${tmpdir}/last-reconciliation.json")"
    assert_json_valid "saved JSON is valid" "$saved_json"
    assert_json_field "saved summary.created=1" "$saved_json" '.summary.created' "1"
else
    fail "report_save did not create file"
fi
rm -rf "$tmpdir"

# ---------------------------------------------------------------------------
# Tests: report_emit agamemnon_url field
# ---------------------------------------------------------------------------

echo ""
echo "=== report_emit agamemnon_url ==="

AGAMEMNON_URL="http://test-host:9090"
report_init "all"
result="$(report_emit 0 0 0 0 0 0 0)"
assert_json_field "agamemnon_url reflects AGAMEMNON_URL" "$result" '.agamemnon_url' "http://test-host:9090"

# ---------------------------------------------------------------------------
# Tests: drift JSON with tags
# ---------------------------------------------------------------------------

echo ""
echo "=== build_drift_json tags ==="

actual_with_tags='{"label":"L","program":"P","workingDirectory":"/w","programArgs":"","taskDescription":"D","tags":["x","y"]}'
drift_tags="$(build_drift_json "UPDATE:tags" "$actual_with_tags" \
    "L" "P" "/w" "" "D" "a,b,c")"
assert_json_valid "drift tags is valid JSON" "$drift_tags"
assert_json_field "drift tags field name" "$drift_tags" '.[0].field' "tags"
assert_json_field "drift tags old value" "$drift_tags" '.[0].old' "x,y"
assert_json_field "drift tags new value (csv)" "$drift_tags" '.[0].new' "a,b,c"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
