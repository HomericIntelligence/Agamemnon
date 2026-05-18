#!/usr/bin/env bash
# tests/test-api-xtrace-init.sh — Verify AGAMEMNON_API_KEY init in api.sh does not leak under xtrace
#
# Issue #429: the source-time assignment `AGAMEMNON_API_KEY="${AGAMEMNON_API_KEY:-}"` at the top
# of scripts/lib/api.sh was a secondary xtrace leak path. A set +x guard was added around it.
# These tests verify that guard works correctly.
#
# Tests:
#   1. Token value does not appear in xtrace when api.sh is sourced under bash -x
#   2. AGAMEMNON_API_KEY is correctly set after the guard (functional check)
#   3. Xtrace is restored after sourcing (a statement after source still traces)
#   4. Guard does not enable xtrace when it was off before sourcing
#
# Usage:
#   ./tests/test-api-xtrace-init.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PASS=0
FAIL=0

_pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
_fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

assert_not_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if [[ "$haystack" != *"$needle"* ]]; then
        _pass "$desc"
    else
        _fail "$desc — found '${needle}' in output (should be hidden)"
    fi
}

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if [[ "$haystack" == *"$needle"* ]]; then
        _pass "$desc"
    else
        _fail "$desc — expected '${needle}' not found in output"
    fi
}

echo "Running api source-time xtrace init tests..."
echo ""

# ── Test 1: Token value does not appear in xtrace when api.sh is sourced ─────
# The key is passed via the environment so the caller's own assignment does not
# appear in the xtrace — we only want to verify that api.sh's internal re-init
# `AGAMEMNON_API_KEY="${AGAMEMNON_API_KEY:-}"` does not re-echo the token.
_XTRACE_OUT="$(AGAMEMNON_API_KEY=init-secret-token-1 bash -x -c "
    AGAMEMNON_URL=http://localhost:9999
    source '${REPO_ROOT}/scripts/lib/api.sh'
" 2>&1 >/dev/null)"
assert_not_contains "source-time init: token not in xtrace" \
    "init-secret-token-1" "$_XTRACE_OUT"

# ── Test 2: AGAMEMNON_API_KEY is correctly set after source ───────────────────
_VALUE_CHECK="$(bash -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=init-secret-token-2
    source '${REPO_ROOT}/scripts/lib/api.sh'
    echo \"\${AGAMEMNON_API_KEY}\"
" 2>/dev/null)"
assert_contains "source-time init: AGAMEMNON_API_KEY correctly set" \
    "init-secret-token-2" "$_VALUE_CHECK"

# ── Test 3: Xtrace is restored after sourcing api.sh ─────────────────────────
_RESTORE_CHECK="$(bash -x -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=init-canary-restore
    source '${REPO_ROOT}/scripts/lib/api.sh'
    echo XTRACE_STILL_ON
" 2>&1)"
assert_contains "xtrace restored after source: echo command traced" \
    "+ echo XTRACE_STILL_ON" "$_RESTORE_CHECK"

# ── Test 4: Guard does not enable xtrace when it was off ─────────────────────
_NO_XTRACE_CHECK="$(bash +x -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=init-secret-token-4
    source '${REPO_ROOT}/scripts/lib/api.sh'
    echo AFTER_SOURCE
" 2>&1)"
assert_contains "xtrace-off path: echo still works after source" \
    "AFTER_SOURCE" "$_NO_XTRACE_CHECK"
assert_not_contains "xtrace-off path: no xtrace lines introduced by source" \
    "+ source" "$_NO_XTRACE_CHECK"

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
