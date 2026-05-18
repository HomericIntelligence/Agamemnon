#!/usr/bin/env bats
# tests/unit/test_api.bats — unit tests for scripts/lib/api.sh
#
# Uses a Python-based mock HTTP server (tests/helpers/mock_server.py)
# to simulate ProjectAgamemnon API responses without a live server.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"

MOCK_PORT=18080
MOCK_PID_FILE="/tmp/bats-mock-api-$$.pid"

# ---------------------------------------------------------------------------
# Mock HTTP server helpers
# ---------------------------------------------------------------------------

# Start the mock server with a fixed status + body response.
# Uses a fixed 200ms sleep instead of polling to avoid set -e issues.
_start_mock_server() {
    local http_status="${1:-200}"
    # Avoid ":-{}" default — bash parses it as "${2:-{" + literal "}" due to
    # unbalanced braces in parameter expansion. Use explicit fallback instead.
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
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"
}

teardown() {
    _stop_mock_server
}

# ---------------------------------------------------------------------------
# agamemnon_check_connection
# ---------------------------------------------------------------------------

@test "agamemnon_check_connection: succeeds when server returns 200" {
    _start_mock_server 200 '{"status":"ok"}'
    run agamemnon_check_connection
    [[ "$status" -eq 0 ]]
}

@test "agamemnon_check_connection: fails when server is not running" {
    export AGAMEMNON_URL="http://127.0.0.1:19999"
    run agamemnon_check_connection
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# agamemnon_list_agents
# ---------------------------------------------------------------------------

@test "agamemnon_list_agents: returns JSON array from server" {
    local agents_json='[{"id":"abc","name":"agent1","status":"active"},{"id":"def","name":"agent2","status":"offline"}]'
    _start_mock_server 200 "$agents_json"
    result="$(agamemnon_list_agents)"
    count="$(echo "$result" | jq 'length')"
    [[ "$count" == "2" ]]
}

@test "agamemnon_list_agents: fails on HTTP 500" {
    _start_mock_server 500 '{"error":"internal server error"}'
    run agamemnon_list_agents
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# agamemnon_get_agent
# ---------------------------------------------------------------------------

@test "agamemnon_get_agent: returns agent JSON by ID" {
    local agent_json='{"id":"abc123","name":"my-agent","status":"active"}'
    _start_mock_server 200 "$agent_json"
    result="$(agamemnon_get_agent "abc123")"
    name="$(echo "$result" | jq -r '.name')"
    [[ "$name" == "my-agent" ]]
}

@test "agamemnon_get_agent: fails on 404" {
    _start_mock_server 404 '{"error":"not found"}'
    run agamemnon_get_agent "nonexistent"
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# agamemnon_create_agent
# ---------------------------------------------------------------------------

@test "agamemnon_create_agent: sends POST and returns created agent JSON" {
    local created_json='{"id":"new123","name":"new-agent","status":"offline"}'
    _start_mock_server 201 "$created_json"
    body='{"name":"new-agent","program":"claude-code","workingDirectory":"/tmp"}'
    result="$(agamemnon_create_agent "$body")"
    id="$(echo "$result" | jq -r '.id')"
    [[ "$id" == "new123" ]]
}

@test "agamemnon_create_agent: fails on HTTP 400" {
    _start_mock_server 400 '{"error":"bad request"}'
    run agamemnon_create_agent '{"invalid":"body"}'
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# agamemnon_update_agent
# ---------------------------------------------------------------------------

@test "agamemnon_update_agent: sends PATCH and returns updated agent JSON" {
    local updated_json='{"id":"abc123","name":"my-agent","label":"Updated Label","status":"active"}'
    _start_mock_server 200 "$updated_json"
    result="$(agamemnon_update_agent "abc123" '{"label":"Updated Label"}')"
    lbl="$(echo "$result" | jq -r '.label')"
    [[ "$lbl" == "Updated Label" ]]
}

# ---------------------------------------------------------------------------
# agamemnon_delete_agent
# ---------------------------------------------------------------------------

@test "agamemnon_delete_agent: succeeds on 200" {
    _start_mock_server 200 '{"deleted":true}'
    run agamemnon_delete_agent "abc123"
    [[ "$status" -eq 0 ]]
}

@test "agamemnon_delete_agent: fails on 404" {
    _start_mock_server 404 '{"error":"not found"}'
    run agamemnon_delete_agent "nonexistent"
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# agamemnon_wake_agent
# ---------------------------------------------------------------------------

@test "agamemnon_wake_agent: succeeds on 200" {
    _start_mock_server 200 '{"id":"abc123","status":"active"}'
    result="$(agamemnon_wake_agent "abc123")"
    status_val="$(echo "$result" | jq -r '.status')"
    [[ "$status_val" == "active" ]]
}

@test "agamemnon_wake_agent: fails on HTTP error" {
    _start_mock_server 500 '{"error":"failed to start"}'
    run agamemnon_wake_agent "abc123"
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# agamemnon_hibernate_agent
# ---------------------------------------------------------------------------

@test "agamemnon_hibernate_agent: succeeds on 200" {
    _start_mock_server 200 '{"id":"abc123","status":"offline"}'
    result="$(agamemnon_hibernate_agent "abc123")"
    status_val="$(echo "$result" | jq -r '.status')"
    [[ "$status_val" == "offline" ]]
}

# ---------------------------------------------------------------------------
# agamemnon_id_by_name
# ---------------------------------------------------------------------------

@test "agamemnon_id_by_name: returns correct ID when agent exists" {
    local agents_json='[{"id":"id-001","name":"agent-alpha","status":"active"},{"id":"id-002","name":"agent-beta","status":"offline"}]'
    _start_mock_server 200 "$agents_json"
    result="$(agamemnon_id_by_name "agent-alpha")"
    [[ "$result" == "id-001" ]]
}

@test "agamemnon_id_by_name: returns empty string when agent not found" {
    local agents_json='[{"id":"id-001","name":"agent-alpha","status":"active"}]'
    _start_mock_server 200 "$agents_json"
    result="$(agamemnon_id_by_name "nonexistent")"
    [[ -z "$result" ]]
}

# ---------------------------------------------------------------------------
# agamemnon_status_by_name
# ---------------------------------------------------------------------------

@test "agamemnon_status_by_name: returns correct status when agent exists" {
    local agents_json='[{"id":"id-001","name":"my-agent","status":"online"}]'
    _start_mock_server 200 "$agents_json"
    result="$(agamemnon_status_by_name "my-agent")"
    [[ "$result" == "online" ]]
}

@test "agamemnon_status_by_name: returns unknown when agent not found" {
    local agents_json='[]'
    _start_mock_server 200 "$agents_json"
    result="$(agamemnon_status_by_name "ghost-agent")"
    [[ "$result" == "unknown" ]]
}
