#!/usr/bin/env bats
# tests/unit/test_api_auth.bats — tests for auth header injection in scripts/lib/api.sh
#
# Verifies that AGAMEMNON_API_KEY correctly injects Authorization and X-API-Key
# headers, and that no headers are sent when the key is unset.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"

MOCK_PORT=18081
MOCK_PID_FILE="/tmp/bats-mock-api-auth-$$.pid"

# Start the mock server with a fixed status + body response.
_start_mock_server() {
    local http_status="${1:-200}"
    local body="${2}"
    [[ -z "$body" ]] && body='{}'

    MOCK_STATUS="$http_status" MOCK_BODY="$body" \
        python3 "${HELPERS_DIR}/mock_server.py" "$MOCK_PORT" \
        > /dev/null 2>&1 &
    echo $! > "$MOCK_PID_FILE"
    sleep 0.2
}

_stop_mock_server() {
    if [[ -f "$MOCK_PID_FILE" ]]; then
        kill "$(cat "$MOCK_PID_FILE")" 2>/dev/null || true
        rm -f "$MOCK_PID_FILE"
    fi
}

setup() {
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
}

teardown() {
    _stop_mock_server
}

# Helper: check if a string pattern appears in curl command
# Uses set -x to trace curl invocations
_check_curl_headers() {
    local pattern="$1"
    local script="$2"
    local output

    output=$( (set -x; eval "$script") 2>&1 )

    if echo "$output" | grep -q "$pattern"; then
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# Auth header injection tests
# ---------------------------------------------------------------------------

@test "auth: no headers when AGAMEMNON_API_KEY is unset" {
    unset AGAMEMNON_API_KEY
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # Check _agamemnon_auth_headers directly
    _agamemnon_auth_headers
    [[ "${#_AUTH_HEADERS[@]}" -eq 0 ]]
}

@test "auth: Authorization Bearer header injected when key is set" {
    export AGAMEMNON_API_KEY="test-secret-key-12345"
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # Call _agamemnon_auth_headers to build the array
    _agamemnon_auth_headers

    # Check that Authorization header is in the array
    printf '%s\n' "${_AUTH_HEADERS[@]}" | grep -q "Authorization: Bearer test-secret-key-12345"
}

@test "auth: X-API-Key header injected when key is set" {
    export AGAMEMNON_API_KEY="test-secret-key-12345"
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # Call _agamemnon_auth_headers to build the array
    _agamemnon_auth_headers

    # Check that X-API-Key header is in the array
    printf '%s\n' "${_AUTH_HEADERS[@]}" | grep -q "X-API-Key: test-secret-key-12345"
}

@test "auth: both Authorization and X-API-Key headers present when key is set" {
    export AGAMEMNON_API_KEY="my-api-token"
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # Call _agamemnon_auth_headers to build the array
    _agamemnon_auth_headers

    # Both headers should be present
    local headers_str
    headers_str="$(printf '%s\n' "${_AUTH_HEADERS[@]}")"
    echo "$headers_str" | grep -q "Authorization: Bearer my-api-token"
    echo "$headers_str" | grep -q "X-API-Key: my-api-token"
}

@test "auth: token value does not leak to stderr on API call failure" {
    export AGAMEMNON_API_KEY="super-secret-api-key"
    _start_mock_server 500 '{"error":"internal server error"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # Capture stderr from a failing API call
    local stderr_output
    stderr_output=$( { agamemnon_check_connection 2>&1 1>/dev/null; } || true)

    # Token should NOT appear in stderr output
    ! echo "$stderr_output" | grep -q "super-secret-api-key"
}

@test "auth: empty key treated as unset (no auth headers)" {
    export AGAMEMNON_API_KEY=""
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # Call _agamemnon_auth_headers
    _agamemnon_auth_headers

    # Empty key should result in no auth headers
    [[ "${#_AUTH_HEADERS[@]}" -eq 0 ]]
}

@test "auth: Authorization header format is correct" {
    export AGAMEMNON_API_KEY="my-test-token"
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    _agamemnon_auth_headers

    # Check the exact format of the Authorization header
    printf '%s\n' "${_AUTH_HEADERS[@]}" | grep -q "^Authorization: Bearer my-test-token\$"
}

@test "auth: X-API-Key header format is correct" {
    export AGAMEMNON_API_KEY="my-test-token"
    _start_mock_server 200 '{"status":"ok"}'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    _agamemnon_auth_headers

    # Check the exact format of the X-API-Key header
    printf '%s\n' "${_AUTH_HEADERS[@]}" | grep -q "^X-API-Key: my-test-token\$"
}

@test "auth: API call succeeds when correct headers are sent" {
    export AGAMEMNON_API_KEY="valid-token"
    _start_mock_server 200 '[{"id":"agent1","name":"test-agent"}]'

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"

    # This should succeed because mock server returns 200
    result="$(agamemnon_list_agents)"
    [[ -n "$result" ]]
    echo "$result" | jq -e '.[0].id == "agent1"'
}
