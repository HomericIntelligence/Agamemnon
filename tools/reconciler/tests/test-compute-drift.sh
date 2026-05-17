#!/usr/bin/env bash
# tests/test-compute-drift.sh — Unit tests for compute_drift() in scripts/lib/reconcile.sh
#
# Tests that model, owner, role, and deployment.type drift is detected correctly,
# and that existing fields still work (regression).
#
# Usage:
#   ./tests/test-compute-drift.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=scripts/lib/reconcile.sh
source "${REPO_ROOT}/scripts/lib/reconcile.sh"

PASS=0
FAIL=0

# assert_eq DESCRIPTION EXPECTED ACTUAL
assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "        expected: $expected"
        echo "        actual:   $actual"
        FAIL=$((FAIL + 1))
    fi
}

# Base JSON that matches desired state — used as a starting point; override fields per test
BASE_JSON='{"status":"online","label":"MyAgent","program":"claude-code","workingDirectory":"/home/user/project","programArgs":"","taskDescription":"Does stuff","tags":[],"model":"claude-sonnet-4-6","owner":"mvillmow","role":"member","deployment":{"type":"local"}}'

echo "=== compute_drift() unit tests ==="
echo ""

echo "--- UNCHANGED / baseline ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "all fields match → UNCHANGED" "UNCHANGED" "$result"

echo ""
echo "--- model drift ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-opus-4-6" "mvillmow" "member" "local")"
assert_eq "model changed → UPDATE:model" "UPDATE:model" "$result"

# API returns null model — desired is empty string (null YAML)
NULL_MODEL_JSON='{"status":"online","label":"MyAgent","program":"claude-code","workingDirectory":"/home/user/project","programArgs":"","taskDescription":"Does stuff","tags":[],"model":null,"owner":"mvillmow","role":"member","deployment":{"type":"local"}}'
result="$(compute_drift "test" "active" "$NULL_MODEL_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "" "mvillmow" "member" "local")"
assert_eq "null model vs empty desired → UNCHANGED (null normalization)" "UNCHANGED" "$result"

echo ""
echo "--- owner drift ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "other-user" "member" "local")"
assert_eq "owner changed → UPDATE:owner" "UPDATE:owner" "$result"

echo ""
echo "--- role drift ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "admin" "local")"
assert_eq "role changed → UPDATE:role" "UPDATE:role" "$result"

echo ""
echo "--- deployment.type drift ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "docker")"
assert_eq "deploymentType changed → UPDATE:deploymentType" "UPDATE:deploymentType" "$result"

echo ""
echo "--- multiple new fields drift ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-opus-4-6" "other-user" "admin" "docker")"
assert_eq "model+owner+role+deploymentType changed → UPDATE contains all 4" \
    "UPDATE:model,owner,role,deploymentType" "$result"

echo ""
echo "--- regression: existing fields still detected ---"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "NewLabel" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "label changed → UPDATE:label" "UPDATE:label" "$result"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "aider" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "program changed → UPDATE:program" "UPDATE:program" "$result"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/other" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "workingDirectory changed → UPDATE:workingDirectory" "UPDATE:workingDirectory" "$result"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "--verbose" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "programArgs changed → UPDATE:programArgs" "UPDATE:programArgs" "$result"

result="$(compute_drift "test" "active" "$BASE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Different desc" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "taskDescription changed → UPDATE:taskDescription" "UPDATE:taskDescription" "$result"

echo ""
echo "--- regression: WAKE / HIBERNATE still work ---"

OFFLINE_JSON='{"status":"offline","label":"MyAgent","program":"claude-code","workingDirectory":"/home/user/project","programArgs":"","taskDescription":"Does stuff","tags":[],"model":"claude-sonnet-4-6","owner":"mvillmow","role":"member","deployment":{"type":"local"}}'
result="$(compute_drift "test" "active" "$OFFLINE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "desired=active, actual=offline → WAKE" "WAKE" "$result"

ONLINE_JSON='{"status":"online","label":"MyAgent","program":"claude-code","workingDirectory":"/home/user/project","programArgs":"","taskDescription":"Does stuff","tags":[],"model":"claude-sonnet-4-6","owner":"mvillmow","role":"member","deployment":{"type":"local"}}'
result="$(compute_drift "test" "hibernated" "$ONLINE_JSON" \
    "MyAgent" "claude-code" "/home/user/project" "" "Does stuff" "" \
    "claude-sonnet-4-6" "mvillmow" "member" "local")"
assert_eq "desired=hibernated, actual=online → HIBERNATE" "HIBERNATE" "$result"

echo ""
echo "==================================="
echo "Results: ${PASS} passed, ${FAIL} failed"
echo ""

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
