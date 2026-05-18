#!/usr/bin/env bats
# tests/unit/test_curl_cleanup.bats — tests for _agamemnon_curl temp file
# cleanup guarantees (#117)
#
# _agamemnon_curl_retry creates a mktemp file per attempt and removes it with
# `rm -f "$tmpfile"` after reading the response.  These tests verify that no
# temp files are leaked to the filesystem on success, on permanent error
# (non-retried HTTP error), and on transient error (all retries exhausted).
#
# Strategy: intercept mktemp to record every temp-file path created during a
# _agamemnon_curl_retry call, then assert all recorded paths have been removed
# after the call returns.
#
# NOTE: curl and mktemp must be overridden BEFORE sourcing api.sh so that
# the overrides are in scope when set -euo pipefail from api.sh takes effect.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------

setup() {
    # Working directory for all per-test tracking files
    TRACKED_TMPDIR="$(command mktemp -d)"
    export TRACKED_TMPDIR

    # File that records every path returned by our mktemp override
    TRACKED_LIST="${TRACKED_TMPDIR}/tracked_tmps"
    : > "$TRACKED_LIST"
    export TRACKED_LIST

    # File recording curl invocation count
    CALL_FILE="${TRACKED_TMPDIR}/calls"
    echo 0 > "$CALL_FILE"
    export CALL_FILE

    # File driving mock responses; entries are "exit_code,http_code" separated
    # by | (the last entry repeats for subsequent calls).
    SEQ_FILE="${TRACKED_TMPDIR}/seq"
    export SEQ_FILE

    # Body written to curl's -o output file
    BODY_FILE="${TRACKED_TMPDIR}/body"
    echo '{"ok":true}' > "$BODY_FILE"
    export BODY_FILE

    # Override sleep so tests run instantly (must be exported for subshells)
    sleep() { :; }
    export -f sleep

    # Override mktemp: create a real temp file via the external binary,
    # record its path, and echo it.  Defined BEFORE sourcing api.sh so that
    # the override is active when set -euo pipefail takes effect.
    mktemp() {
        local real_tmp
        real_tmp="$(command mktemp)"
        echo "$real_tmp" >> "$TRACKED_LIST"
        echo "$real_tmp"
    }
    export -f mktemp

    # Override curl to avoid real network calls.  Defined BEFORE sourcing api.sh
    # for the same reason as mktemp above.
    curl() {
        local count
        count=$(<"$CALL_FILE")
        echo $((count + 1)) > "$CALL_FILE"

        # Read next sequence entry; entries separated by |; last entry repeats
        local seq entry rest exit_code http_code
        seq=$(<"$SEQ_FILE")
        entry="${seq%%|*}"
        rest="${seq#*|}"
        exit_code="${entry%%,*}"
        http_code="${entry#*,}"
        [[ "$rest" != "$seq" ]] && echo "$rest" > "$SEQ_FILE"

        # Write mock body to curl's -o destination
        local args=("$@") i
        for (( i=0; i<${#args[@]}; i++ )); do
            if [[ "${args[$i]}" == "-o" ]]; then
                cat "$BODY_FILE" > "${args[$((i+1))]}"; break
            fi
        done

        echo -n "$http_code"
        return "$exit_code"
    }
    export -f curl

    # Source api.sh after overrides are in place
    export AGAMEMNON_URL="http://mock.test:19999"
    export AGAMEMNON_TIMEOUT=1
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"
}

teardown() {
    [[ -d "${TRACKED_TMPDIR:-}" ]] && rm -rf "$TRACKED_TMPDIR"
}

# ---------------------------------------------------------------------------
# Helper: assert all tracked temp files have been removed
# ---------------------------------------------------------------------------
_assert_no_leaked_tmpfiles() {
    local leaked=()
    while IFS= read -r path; do
        [[ -z "$path" ]] && continue
        [[ -f "$path" ]] && leaked+=("$path")
    done < "$TRACKED_LIST"

    if [[ ${#leaked[@]} -gt 0 ]]; then
        echo "LEAKED temp files:" >&2
        printf '  %s\n' "${leaked[@]}" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Test 1: Success on first attempt — temp file is cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp file removed on success (HTTP 200)" {
    echo "0,200" > "$SEQ_FILE"

    _agamemnon_curl_retry "http://mock.test:19999/v1/agents" >/dev/null 2>/dev/null
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 2: Permanent error (HTTP 404) — temp file is cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp file removed on permanent HTTP error (404)" {
    echo "0,404" > "$SEQ_FILE"

    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 3: Permanent error (HTTP 401) — temp file is cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp file removed on permanent HTTP error (401)" {
    echo "0,401" > "$SEQ_FILE"

    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 4: Transient error (exit 7) then success — both temp files cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp files removed after retry (exit 7 then 200)" {
    # Pipe-separated sequence: first call returns exit 7, second returns 200
    echo "7,000|0,200" > "$SEQ_FILE"

    # Use `run` so that set -e from api.sh does not abort the test on curl's
    # transient exit 7; the function should retry internally and succeed.
    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null
    [[ "$status" -eq 0 ]]
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 5: All retries exhausted (exit 7 x3) — all temp files cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp files removed when all 3 attempts fail (exit 7)" {
    echo "7,000" > "$SEQ_FILE"

    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 6: All retries exhausted (HTTP 503 x3) — all temp files cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp files removed when all 3 attempts return HTTP 503" {
    echo "0,503" > "$SEQ_FILE"

    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 7: Exactly one temp file per attempt is created and removed on success
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: exactly 1 temp file created and removed on success" {
    echo "0,200" > "$SEQ_FILE"
    # Reset tracked list so we only count what this test creates
    : > "$TRACKED_LIST"

    _agamemnon_curl_retry "http://mock.test:19999/v1/agents" >/dev/null 2>/dev/null

    local count
    count=$(grep -c . "$TRACKED_LIST" || true)
    # One attempt → exactly 1 temp file created
    [[ $count -eq 1 ]]
    # And it must be cleaned up
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 8: Three temp files created and removed when all retries fail
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: 3 temp files created and all removed when all retries fail" {
    echo "7,000" > "$SEQ_FILE"
    # Reset tracked list so we only count what this test creates
    : > "$TRACKED_LIST"

    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null

    local count
    count=$(grep -c . "$TRACKED_LIST" || true)
    # Three attempts → 3 temp files tracked
    [[ $count -eq 3 ]]
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 9: Transient timeout (exit 28) then success — temp files cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp files removed after exit-28 retry then success" {
    echo "28,000|0,200" > "$SEQ_FILE"

    # Use `run` so that set -e from api.sh does not abort the test on curl's
    # transient exit 28; the function should retry internally and succeed.
    run _agamemnon_curl_retry "http://mock.test:19999/v1/agents" 2>/dev/null
    [[ "$status" -eq 0 ]]
    _assert_no_leaked_tmpfiles
}

# ---------------------------------------------------------------------------
# Test 10: HTTP 500 (transient) then success — temp files cleaned up
# ---------------------------------------------------------------------------

@test "_agamemnon_curl_retry: temp files removed after HTTP 500 retry then success" {
    echo "0,500|0,200" > "$SEQ_FILE"

    _agamemnon_curl_retry "http://mock.test:19999/v1/agents" >/dev/null 2>/dev/null
    _assert_no_leaked_tmpfiles
}
