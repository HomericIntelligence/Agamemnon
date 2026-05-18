#!/usr/bin/env bash
# tests/test-api-xtrace.sh — Verify AGAMEMNON_API_KEY does not leak in xtrace output
#
# The issue: curl invocations expand "-H Authorization: Bearer ${AGAMEMNON_API_KEY}"
# under bash -x / set -x, printing the token to the trace log. Guards in api.sh
# suppress xtrace during header construction and curl calls. These tests verify:
#   1. The Authorization/X-API-Key header values do not appear in xtrace output.
#   2. The _AUTH_HEADERS array is still correctly populated (guard doesn't break function).
#   3. Xtrace is restored to its prior state after the guarded section.
#   4. No headers are built when AGAMEMNON_API_KEY is unset.
#
# What is NOT tested (and is intentionally out of scope):
#   - "export AGAMEMNON_API_KEY=..." in caller scripts — that is caller responsibility.
#
# Source-time variable init (issue #429) is covered by tests/test-api-xtrace-init.sh.
#
# Usage:
#   ./tests/test-api-xtrace.sh
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

echo "Running api xtrace leak tests..."
echo ""

# ── Test 1: Authorization header value does not appear in xtrace ──────────────
# Run a subshell with bash -x and capture stderr (where xtrace goes).
# The "Authorization: Bearer <token>" string must not appear in the trace.
# Note: "export AGAMEMNON_API_KEY=<token>" from the caller IS expected in trace;
# we check specifically that the header string is not traced.
_XTRACE_OUT="$(bash -x -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=supersecret-token-1
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_auth_headers
" 2>&1 >/dev/null)"
assert_not_contains "_agamemnon_auth_headers: Authorization Bearer not in xtrace" \
    "Authorization: Bearer supersecret-token-1" "$_XTRACE_OUT"
assert_not_contains "_agamemnon_auth_headers: X-API-Key not in xtrace" \
    "X-API-Key: supersecret-token-1" "$_XTRACE_OUT"

# ── Test 2: _AUTH_HEADERS array is still populated correctly ──────────────────
# Functional check: the guard must not break header population.
_HEADERS_CHECK="$(bash -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=supersecret-token-2
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_auth_headers
    echo \"\${_AUTH_HEADERS[*]}\"
" 2>/dev/null)"
assert_contains "_agamemnon_auth_headers: Authorization header populated" \
    "Authorization: Bearer supersecret-token-2" "$_HEADERS_CHECK"
assert_contains "_agamemnon_auth_headers: X-API-Key header populated" \
    "X-API-Key: supersecret-token-2" "$_HEADERS_CHECK"

# ── Test 3: Authorization header value does not leak from _agamemnon_curl_retry ─
# Override curl in the subshell to avoid network calls, then check xtrace output.
_RETRY_XTRACE="$(bash -x -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=supersecret-token-3
    # Stub curl: write an empty body and return HTTP 200
    curl() {
        local args=(\"\$@\")
        local i
        for (( i=0; i<\${#args[@]}; i++ )); do
            if [[ \"\${args[\$i]}\" == \"-o\" ]]; then
                echo '{}' > \"\${args[\$((i+1))]}\"; break
            fi
        done
        echo -n '200'
        return 0
    }
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_curl_retry 'http://localhost:9999/v1/agents' >/dev/null
" 2>&1 >/dev/null)"
assert_not_contains "_agamemnon_curl_retry: Authorization Bearer not in xtrace" \
    "Authorization: Bearer supersecret-token-3" "$_RETRY_XTRACE"
assert_not_contains "_agamemnon_curl_retry: X-API-Key not in xtrace" \
    "X-API-Key: supersecret-token-3" "$_RETRY_XTRACE"

# ── Test 4: xtrace is restored after _agamemnon_auth_headers ──────────────────
# If xtrace was on before the call, it must be on after. We verify by checking
# that a statement AFTER the call still appears in the xtrace output.
_RESTORE_CHECK="$(bash -x -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=canary-restore-token
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_auth_headers
    echo XTRACE_STILL_ON
" 2>&1)"
assert_contains "xtrace restored after _agamemnon_auth_headers" \
    "XTRACE_STILL_ON" "$_RESTORE_CHECK"
assert_contains "xtrace re-enabled: echo command traced" \
    "+ echo XTRACE_STILL_ON" "$_RESTORE_CHECK"

# ── Test 5: no-auth path unchanged — empty key produces no headers ────────────
_NO_KEY_CHECK="$(bash -c "
    AGAMEMNON_URL=http://localhost:9999
    unset AGAMEMNON_API_KEY
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_auth_headers
    echo \"\${#_AUTH_HEADERS[@]}\"
" 2>/dev/null)"
if [[ "$_NO_KEY_CHECK" == "0" ]]; then
    _pass "no-auth path: empty key yields empty _AUTH_HEADERS"
else
    _fail "no-auth path: expected 0 headers, got ${_NO_KEY_CHECK}"
fi

# ── Test 6: xtrace guard does not enable xtrace when it was off ───────────────
# If xtrace is OFF before the call, it must remain OFF after.
_NO_XTRACE_CHECK="$(bash +x -c "
    AGAMEMNON_URL=http://localhost:9999
    AGAMEMNON_API_KEY=supersecret-token-6
    source '${REPO_ROOT}/scripts/lib/api.sh'
    _agamemnon_auth_headers
    echo AFTER_CALL
" 2>&1)"
# In non-xtrace mode, the output should contain AFTER_CALL (from echo) but no +/-x traces
assert_contains "xtrace-off path: echo still works" "AFTER_CALL" "$_NO_XTRACE_CHECK"
assert_not_contains "xtrace-off path: no xtrace lines introduced" \
    "+ _agamemnon_auth_headers" "$_NO_XTRACE_CHECK"

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
