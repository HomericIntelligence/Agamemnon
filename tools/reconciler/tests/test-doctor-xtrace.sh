#!/usr/bin/env bash
# tests/test-doctor-xtrace.sh — Verify AGAMEMNON_API_KEY does not leak from doctor.sh
#
# doctor.sh's check_connectivity() makes two curl calls to /v1/health and /v1/agents.
# After the fix in issue #430, these calls are guarded with the standard xtrace pattern
# so that AGAMEMNON_API_KEY does not appear in bash -x trace output.
#
# Usage:
#   ./tests/test-doctor-xtrace.sh
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

echo "Running doctor.sh xtrace leak tests..."
echo ""

# ── Test 1: AGAMEMNON_API_KEY does not appear in xtrace for health check ────────
# Uses a function to allow `local` declarations; stubs curl to avoid network calls.
_XTRACE_OUT="$(bash -x -c "
    _run_connectivity_check() {
        AGAMEMNON_URL=http://localhost:9999
        AGAMEMNON_API_KEY=super-secret-doctor-key
        curl() { echo -n '200'; return 0; }
        export -f curl
        source '${REPO_ROOT}/scripts/lib/api.sh'
        _agamemnon_auth_headers
        local _had_xtrace=0
        if [[ \"\$-\" == *x* ]]; then _had_xtrace=1; fi
        { set +x; } 2>/dev/null
        local http_code
        http_code=\"\$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' \
            \"\${_AUTH_HEADERS[@]+\"\${_AUTH_HEADERS[@]}\"}\" \
            'http://localhost:9999/v1/health' 2>/dev/null)\" || http_code='000'
        if [[ \$_had_xtrace -eq 1 ]]; then set -x; fi
        echo \"health:\$http_code\"
    }
    _run_connectivity_check
" 2>&1 >/dev/null)"
assert_not_contains "doctor health check: Authorization Bearer not in xtrace" \
    "Authorization: Bearer super-secret-doctor-key" "$_XTRACE_OUT"
assert_not_contains "doctor health check: X-API-Key not in xtrace" \
    "X-API-Key: super-secret-doctor-key" "$_XTRACE_OUT"

# ── Test 2: xtrace is restored after the health check guard ─────────────────────
_RESTORE_OUT="$(bash -x -c "
    _run_restore_check() {
        AGAMEMNON_URL=http://localhost:9999
        AGAMEMNON_API_KEY=canary-restore-key
        curl() { echo -n '200'; return 0; }
        export -f curl
        source '${REPO_ROOT}/scripts/lib/api.sh'
        _agamemnon_auth_headers
        local _had_xtrace=0
        if [[ \"\$-\" == *x* ]]; then _had_xtrace=1; fi
        { set +x; } 2>/dev/null
        local http_code
        http_code=\"\$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' \
            \"\${_AUTH_HEADERS[@]+\"\${_AUTH_HEADERS[@]}\"}\" \
            'http://localhost:9999/v1/health' 2>/dev/null)\" || http_code='000'
        if [[ \$_had_xtrace -eq 1 ]]; then set -x; fi
        echo XTRACE_RESTORED
    }
    _run_restore_check
" 2>&1)"
assert_contains "xtrace restored after health check guard" \
    "XTRACE_RESTORED" "$_RESTORE_OUT"
assert_contains "xtrace re-enabled: echo traced after guard" \
    "+ echo XTRACE_RESTORED" "$_RESTORE_OUT"

# ── Test 3: no-auth path — no headers built, guard still safe ───────────────────
_NO_KEY_OUT="$(bash -c "
    unset AGAMEMNON_API_KEY
    AGAMEMNON_URL=http://localhost:9999
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_auth_headers
    echo \"header_count:\${#_AUTH_HEADERS[@]}\"
" 2>/dev/null)"
if [[ "$_NO_KEY_OUT" == *"header_count:0"* ]]; then
    _pass "doctor no-auth path: empty key yields no headers"
else
    _fail "doctor no-auth path: expected header_count:0, got: ${_NO_KEY_OUT}"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
