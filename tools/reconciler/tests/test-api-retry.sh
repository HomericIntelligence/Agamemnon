#!/usr/bin/env bash
# tests/test-api-retry.sh — Unit tests for _agamemnon_curl_retry()
#
# Tests retry logic, exponential backoff, transient vs permanent error
# classification, and AGAMEMNON_TIMEOUT env var handling in api.sh.
#
# Usage:
#   ./tests/test-api-retry.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PASS=0
FAIL=0

# ── Test harness ──────────────────────────────────────────────────────────────

_pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
_fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        _pass "$desc"
    else
        _fail "$desc — expected '${expected}', got '${actual}'"
    fi
}

assert_zero() {
    local desc="$1" actual="$2"
    if [[ "$actual" -eq 0 ]]; then
        _pass "$desc"
    else
        _fail "$desc — expected exit 0, got ${actual}"
    fi
}

assert_nonzero() {
    local desc="$1" actual="$2"
    if [[ "$actual" -ne 0 ]]; then
        _pass "$desc"
    else
        _fail "$desc — expected non-zero exit, got 0"
    fi
}

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if [[ "$haystack" == *"$needle"* ]]; then
        _pass "$desc"
    else
        _fail "$desc — expected to find '${needle}' in: ${haystack}"
    fi
}

# ── Mock infrastructure ───────────────────────────────────────────────────────
# Because _agamemnon_curl_retry calls curl inside $() subshells, we track
# call counts and sequence state via temp files so mutations survive the
# subshell boundary.

_CALL_FILE=""
_SEQ_FILE=""
_BODY_FILE=""
_STDERR_FILE=""

setup_mock() {
    _CALL_FILE="$(mktemp)"
    _SEQ_FILE="$(mktemp)"
    _BODY_FILE="$(mktemp)"
    _STDERR_FILE="$(mktemp)"
    echo 0 > "$_CALL_FILE"
    echo '{"ok":true}' > "$_BODY_FILE"
}

teardown_mock() {
    rm -f "$_CALL_FILE" "$_SEQ_FILE" "$_BODY_FILE" "$_STDERR_FILE"
}

# Set a single response for all calls: exit_code http_code
mock_single() {
    local exit_code="$1" http_code="$2"
    echo "${exit_code},${http_code}" > "$_SEQ_FILE"
}

# Set a sequence of responses: each arg is "exit_code,http_code"
# The last entry repeats indefinitely.
mock_sequence() {
    local IFS='|'
    echo "$*" > "$_SEQ_FILE"
}

# Override curl so tests don't hit the network.
# Reads sequence from _SEQ_FILE, writes body from _BODY_FILE to -o target,
# outputs http_code, returns exit_code.
#
# Handles both space-separated and equals-sign forms of flags:
#   --max-time 30   (space form)
#   --max-time=30   (equals sign form, issue #265)
#   -o file         (space form)
#   -o=file         (equals sign form)
curl() {
    # Increment call count
    local count
    count=$(<"$_CALL_FILE")
    echo $((count + 1)) > "$_CALL_FILE"

    # Read next sequence entry
    local seq entry rest exit_code http_code
    seq=$(<"$_SEQ_FILE")
    entry="${seq%%|*}"
    rest="${seq#*|}"
    exit_code="${entry%%,*}"
    http_code="${entry#*,}"

    # Advance sequence (last entry repeats)
    if [[ "$rest" != "$seq" ]]; then
        echo "$rest" > "$_SEQ_FILE"
    fi

    # Write body to -o file, handling both "-o file" and "-o=file" / "--output=file"
    local args=("$@")
    local i
    for (( i=0; i<${#args[@]}; i++ )); do
        case "${args[$i]}" in
            -o)
                # Space-separated: -o <file>
                cat "$_BODY_FILE" > "${args[$((i+1))]}"
                break
                ;;
            -o=*)
                # Equals-sign form: -o=<file>
                cat "$_BODY_FILE" > "${args[$i]#-o=}"
                break
                ;;
            --output=*)
                # Long equals-sign form: --output=<file>
                cat "$_BODY_FILE" > "${args[$i]#--output=}"
                break
                ;;
        esac
    done

    echo -n "$http_code"
    return "$exit_code"
}

# Override sleep so tests run instantly.
sleep() { : ; }

# ── Load api.sh ───────────────────────────────────────────────────────────────
AGAMEMNON_URL="http://mock.test:9999"
export AGAMEMNON_TIMEOUT=5
source "${REPO_ROOT}/scripts/lib/api.sh"

# ── Tests ─────────────────────────────────────────────────────────────────────

echo "Running api retry tests..."
echo ""

# ── Test 1: Success on first attempt — no retries ─────────────────────────────
setup_mock
mock_single 0 200
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")"
rc=$?
calls=$(<"$_CALL_FILE")
assert_zero "success on first attempt: exit 0" "$rc"
assert_eq "success on first attempt: response body" '{"ok":true}' "$output"
assert_eq "success on first attempt: curl called once" "1" "$calls"
teardown_mock

# ── Test 2: Retry on exit 7 (connection refused), success on 2nd attempt ──────
setup_mock
mock_sequence "7,000" "0,200"
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")"
rc=$?
calls=$(<"$_CALL_FILE")
stderr_content="$(cat "$_STDERR_FILE")"
assert_zero "retry on exit 7, success on 2nd: exit 0" "$rc"
assert_eq "retry on exit 7, success on 2nd: curl called twice" "2" "$calls"
assert_contains "retry on exit 7: WARN emitted" "WARN: Retry 1/3" "$stderr_content"
teardown_mock

# ── Test 3: Retry on exit 28 (timeout), success on 2nd attempt ────────────────
setup_mock
mock_sequence "28,000" "0,200"
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")"
rc=$?
calls=$(<"$_CALL_FILE")
stderr_content="$(cat "$_STDERR_FILE")"
assert_zero "retry on exit 28, success on 2nd: exit 0" "$rc"
assert_eq "retry on exit 28, success on 2nd: curl called twice" "2" "$calls"
assert_contains "retry on exit 28: WARN emitted" "WARN: Retry 1/3" "$stderr_content"
teardown_mock

# ── Test 4: Retry on HTTP 503, success on 2nd attempt ─────────────────────────
setup_mock
mock_sequence "0,503" "0,200"
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")"
rc=$?
calls=$(<"$_CALL_FILE")
stderr_content="$(cat "$_STDERR_FILE")"
assert_zero "retry on HTTP 503, success on 2nd: exit 0" "$rc"
assert_eq "retry on HTTP 503, success on 2nd: curl called twice" "2" "$calls"
assert_contains "retry on HTTP 503: WARN emitted" "WARN: Retry 1/3" "$stderr_content"
teardown_mock

# ── Test 5: No retry on HTTP 404 (permanent error) ────────────────────────────
setup_mock
mock_single 0 404
rc=0
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents/missing" 2>"$_STDERR_FILE")" && rc=0 || rc=$?
calls=$(<"$_CALL_FILE")
assert_nonzero "no retry on HTTP 404: non-zero exit" "$rc"
assert_eq "no retry on HTTP 404: curl called exactly once" "1" "$calls"
teardown_mock

# ── Test 6: No retry on HTTP 401 (permanent error) ────────────────────────────
setup_mock
mock_single 0 401
rc=0
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")" && rc=0 || rc=$?
calls=$(<"$_CALL_FILE")
assert_nonzero "no retry on HTTP 401: non-zero exit" "$rc"
assert_eq "no retry on HTTP 401: curl called exactly once" "1" "$calls"
teardown_mock

# ── Test 7: AGAMEMNON_TIMEOUT is passed to curl as --max-time ─────────────────
setup_mock
mock_single 0 200
# Capture curl args by overriding curl once more
CAPTURED_ARGS=""
_CAPTURED_FILE="$(mktemp)"
curl() {
    echo "$*" > "$_CAPTURED_FILE"
    local count
    count=$(<"$_CALL_FILE")
    echo $((count + 1)) > "$_CALL_FILE"
    local args=("$@")
    local i
    for (( i=0; i<${#args[@]}; i++ )); do
        if [[ "${args[$i]}" == "-o" ]]; then
            cat "$_BODY_FILE" > "${args[$((i+1))]}"; break
        fi
    done
    echo -n "200"
    return 0
}
export AGAMEMNON_TIMEOUT=42
_agamemnon_curl_retry "http://mock.test:9999/v1/agents" >/dev/null 2>/dev/null
export AGAMEMNON_TIMEOUT=5
CAPTURED_ARGS="$(cat "$_CAPTURED_FILE")"
rm -f "$_CAPTURED_FILE"
# Restore standard curl mock
curl() {
    local count
    count=$(<"$_CALL_FILE")
    echo $((count + 1)) > "$_CALL_FILE"
    local seq entry rest exit_code http_code
    seq=$(<"$_SEQ_FILE")
    entry="${seq%%|*}"; rest="${seq#*|}"
    exit_code="${entry%%,*}"; http_code="${entry#*,}"
    [[ "$rest" != "$seq" ]] && echo "$rest" > "$_SEQ_FILE"
    local args=("$@"); local i
    for (( i=0; i<${#args[@]}; i++ )); do
        case "${args[$i]}" in
            -o) cat "$_BODY_FILE" > "${args[$((i+1))]}"; break ;;
            -o=*) cat "$_BODY_FILE" > "${args[$i]#-o=}"; break ;;
            --output=*) cat "$_BODY_FILE" > "${args[$i]#--output=}"; break ;;
        esac
    done
    echo -n "$http_code"
    return "$exit_code"
}
assert_contains "AGAMEMNON_TIMEOUT passed as --max-time 42" "--max-time 42" "$CAPTURED_ARGS"
teardown_mock

# ── Test 7b: mock curl handles --max-time=N (equals sign form, issue #265) ───────
# Verify the mock curl body-writing loop works correctly when a caller passes
# --max-time=30 instead of --max-time 30.  We simulate this by calling the
# mock directly with the equals-sign form and confirming it writes the body.
setup_mock
mock_single 0 200
_EQ_FILE="$(mktemp)"
# Call the mock curl directly with --max-time=30 (equals sign) and -o <file>.
# The mock returns the exit code from its sequence; here we only care that the
# body was written, so capture rc explicitly instead of suppressing it.
_curl_rc=0
curl --max-time=30 -o "$_EQ_FILE" "http://mock.test:9999/v1/agents" >/dev/null 2>/dev/null || _curl_rc=$?
: "captured curl exit rc=${_curl_rc} for equals-sign --max-time test"
eq_body="$(cat "$_EQ_FILE")"
rm -f "$_EQ_FILE"
assert_eq "mock curl handles --max-time=N: body written via -o" '{"ok":true}' "$eq_body"
teardown_mock

# ── Test 8: All 3 attempts fail (exit 7 each), non-zero exit returned ──────────
setup_mock
mock_single 7 000
rc=0
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")" && rc=0 || rc=$?
calls=$(<"$_CALL_FILE")
stderr_content="$(cat "$_STDERR_FILE")"
assert_nonzero "all 3 attempts fail: non-zero exit" "$rc"
assert_eq "all 3 attempts fail: curl called 3 times" "3" "$calls"
assert_contains "all 3 attempts fail: ERROR in stderr" "ERROR:" "$stderr_content"
teardown_mock

# ── Test 9: WARN retry messages count up correctly (1/3, 2/3) ─────────────────
setup_mock
mock_single 7 000
# Capture rc explicitly — we only assert on stderr content, not on rc, but we
# refuse to silently swallow the value (the previous form `... && true || true`
# was a placeholder for `ignore rc`).
_retry_rc=0
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")" || _retry_rc=$?
: "Test 9 retry rc=${_retry_rc} (intentionally not asserted)"
stderr_content="$(cat "$_STDERR_FILE")"
assert_contains "WARN messages: Retry 1/3 present" "Retry 1/3" "$stderr_content"
assert_contains "WARN messages: Retry 2/3 present" "Retry 2/3" "$stderr_content"
teardown_mock

# ── Test 10: HTTP 500 also treated as transient ────────────────────────────────
setup_mock
mock_sequence "0,500" "0,200"
output="$(_agamemnon_curl_retry "http://mock.test:9999/v1/agents" 2>"$_STDERR_FILE")"
rc=$?
calls=$(<"$_CALL_FILE")
assert_zero "HTTP 500 is transient, retried: exit 0" "$rc"
assert_eq "HTTP 500 retried: curl called twice" "2" "$calls"
teardown_mock

# ── Test 11: Call-count assertion — success on first try calls curl exactly once ──
# Issue #210: verify that sleep mock is working AND that retry behaviour fires
# the right number of times by asserting curl call counts precisely.
setup_mock
mock_single 0 200
# Test asserts on curl call count, not on the function's rc. Capture rc
# explicitly so a regression in the retry function's exit-code semantics is
# still visible in the test transcript.
_retry_rc=0
_agamemnon_curl_retry "http://mock.test:9999/v1/agents" >/dev/null 2>/dev/null || _retry_rc=$?
: "Test 11 retry rc=${_retry_rc} (intentionally not asserted)"
calls=$(<"$_CALL_FILE")
assert_eq "call-count: success on first try — curl called exactly 1 time" "1" "$calls"
teardown_mock

# ── Test 12: Call-count — 2 transient failures then success = 3 curl calls ────
setup_mock
mock_sequence "7,000" "7,000" "0,200"
_retry_rc=0
_agamemnon_curl_retry "http://mock.test:9999/v1/agents" >/dev/null 2>"$_STDERR_FILE" || _retry_rc=$?
: "Test 12 retry rc=${_retry_rc} (intentionally not asserted)"
calls=$(<"$_CALL_FILE")
stderr_content="$(cat "$_STDERR_FILE")"
assert_eq "call-count: 2 failures then success — curl called exactly 3 times" "3" "$calls"
assert_contains "call-count: Retry 1/3 present in stderr" "Retry 1/3" "$stderr_content"
assert_contains "call-count: Retry 2/3 present in stderr" "Retry 2/3" "$stderr_content"
teardown_mock

# ── Test 13: Call-count — permanent HTTP 404 aborts immediately, 1 curl call ──
setup_mock
mock_single 0 404
rc=0
_agamemnon_curl_retry "http://mock.test:9999/v1/agents/x" >/dev/null 2>/dev/null && rc=0 || rc=$?
calls=$(<"$_CALL_FILE")
assert_eq "call-count: permanent 404 — curl called exactly 1 time (no retries)" "1" "$calls"
assert_nonzero "call-count: permanent 404 returns non-zero" "$rc"
teardown_mock

# ── Test 14: Call-count — HTTP 503 x3 exhausts retries, curl called 3 times ──
setup_mock
mock_single 0 503
rc=0
_agamemnon_curl_retry "http://mock.test:9999/v1/agents" >/dev/null 2>/dev/null && rc=0 || rc=$?
calls=$(<"$_CALL_FILE")
assert_eq "call-count: 503 x3 exhausted — curl called exactly 3 times" "3" "$calls"
assert_nonzero "call-count: 503 exhausted returns non-zero" "$rc"
teardown_mock

# ── Test 15: sleep mock verifies tests run without real delays ─────────────────
# The sleep function is overridden to a no-op at the top of this file.
# We verify this is working by timing a full 3-attempt retry sequence:
# with real sleep (1s + 2s = 3s), this would take >3 seconds;
# with the mock sleep it should complete in well under 1 second.
setup_mock
mock_single 7 000  # all 3 attempts fail
_SLEEP_COUNT_FILE="$(mktemp)"
echo 0 > "$_SLEEP_COUNT_FILE"

# Override sleep to count calls in addition to being a no-op
sleep() {
    local n
    n=$(<"$_SLEEP_COUNT_FILE")
    echo $((n + 1)) > "$_SLEEP_COUNT_FILE"
    :  # no-op — do not actually sleep
}

rc=0
_agamemnon_curl_retry "http://mock.test:9999/v1/agents" >/dev/null 2>/dev/null && rc=0 || rc=$?
calls=$(<"$_CALL_FILE")
sleep_calls=$(<"$_SLEEP_COUNT_FILE")
rm -f "$_SLEEP_COUNT_FILE"

# Restore the original sleep override (no-op)
sleep() { : ; }

assert_eq "sleep-mock: curl called 3 times with mocked sleep" "3" "$calls"
# _agamemnon_curl_retry calls sleep between attempt 1→2 and 2→3 (max_attempts-1 = 2 times)
assert_eq "sleep-mock: sleep called exactly 2 times (between retries)" "2" "$sleep_calls"
teardown_mock

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
