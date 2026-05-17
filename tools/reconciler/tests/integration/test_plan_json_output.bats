#!/usr/bin/env bats
# tests/integration/test_plan_json_output.bats
#
# Integration tests for plan.sh --output json.
# Runs plan.sh against a mock Agamemnon API and validates that:
#   - The output is valid JSON
#   - The top-level schema fields (timestamp, host, agamemnon_url, summary, planned_changes) are present
#   - summary counts match the expected planned actions
#   - planned_changes entries have name/action/details fields
#
# Covers issue #183.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/tests/fixtures"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"
PLAN_SH="${SCRIPT_DIR}/scripts/plan.sh"
MOCK_PORT=18083
MOCK_PID_FILE="/tmp/bats-plan-json-mock-$$.pid"

# ---------------------------------------------------------------------------
# Mock server helpers
# ---------------------------------------------------------------------------

_start_mock_server() {
    local http_status="${1:-200}"
    local body="${2}"
    [[ -z "$body" ]] && body='[]'

    MOCK_STATUS="$http_status" MOCK_BODY="$body" \
        python3 "${HELPERS_DIR}/mock_server.py" "$MOCK_PORT" \
        > /dev/null 2>&1 &
    echo $! > "$MOCK_PID_FILE"
    sleep 0.3
}

_stop_mock_server() {
    if [[ -f "$MOCK_PID_FILE" ]]; then
        kill "$(cat "$MOCK_PID_FILE")" 2>/dev/null || true
        rm -f "$MOCK_PID_FILE"
    fi
}

# ---------------------------------------------------------------------------
# Setup: create a minimal temp repo root so plan.sh can find agents/
# ---------------------------------------------------------------------------

setup() {
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
    export NO_COLOR=1

    # Build a temp repo root that mirrors the real one:
    #   <root>/scripts/ → symlinked to real scripts/
    #   <root>/agents/testhost/<agent>.yaml
    REPO_TMP="$(mktemp -d)"
    mkdir -p "${REPO_TMP}/agents/testhost"

    # Symlink scripts/ into temp root so BASH_SOURCE resolves plan.sh → lib/
    ln -s "${SCRIPT_DIR}/scripts" "${REPO_TMP}/scripts"
}

teardown() {
    _stop_mock_server
    [[ -n "${REPO_TMP:-}" ]] && rm -rf "$REPO_TMP"
    unset NO_COLOR
}

# ---------------------------------------------------------------------------
# Helper: place a YAML file in the temp repo and run plan.sh --output json
# plan.sh finds agents/ relative to its own BASH_SOURCE directory.
# Since scripts/ is a symlink resolving to the real scripts/, the repo root
# computed by BASH_SOURCE is the real repo root — not REPO_TMP.
# We therefore copy the agent YAML into the real agents/ under a temp host dir
# and pass that host name to plan.sh to scope the search.
# ---------------------------------------------------------------------------

# Unique host name per test to avoid collisions
_PLAN_TEST_HOST=""

_setup_test_agent() {
    local agent_yaml="$1"
    # Use a unique host dir under the real agents/ directory
    _PLAN_TEST_HOST="plantest-$$-${RANDOM}"
    mkdir -p "${SCRIPT_DIR}/agents/${_PLAN_TEST_HOST}"
    cp "$agent_yaml" "${SCRIPT_DIR}/agents/${_PLAN_TEST_HOST}/"
}

_cleanup_test_agent() {
    if [[ -n "${_PLAN_TEST_HOST:-}" ]]; then
        rm -rf "${SCRIPT_DIR}/agents/${_PLAN_TEST_HOST}"
        _PLAN_TEST_HOST=""
    fi
}

# Run plan.sh --output json with a given agent fixture, scoped to the temp host.
# Sets $output and $status from bats `run`.
_run_plan_json() {
    local agent_yaml="$1"
    shift  # remaining args forwarded to plan.sh

    _setup_test_agent "$agent_yaml"
    run bash "$PLAN_SH" "$_PLAN_TEST_HOST" --output json "$@"
    _cleanup_test_agent
}

# ---------------------------------------------------------------------------
# Tests: schema and JSON validity
# ---------------------------------------------------------------------------

@test "plan.sh --output json: produces valid JSON (CREATE scenario)" {
    # Mock: Agamemnon returns empty list → agent would be created
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    # Output must parse as JSON regardless of exit code
    echo "$output" | jq . > /dev/null
}

@test "plan.sh --output json: top-level schema fields are present" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    echo "$output" | jq -e 'has("timestamp")'        > /dev/null
    echo "$output" | jq -e 'has("host")'             > /dev/null
    echo "$output" | jq -e 'has("agamemnon_url")'    > /dev/null
    echo "$output" | jq -e 'has("summary")'          > /dev/null
    echo "$output" | jq -e 'has("planned_changes")'  > /dev/null
}

@test "plan.sh --output json: summary has all expected keys" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    echo "$output" | jq -e '.summary | has("agents_checked")'  > /dev/null
    echo "$output" | jq -e '.summary | has("changes_needed")'  > /dev/null
    echo "$output" | jq -e '.summary | has("to_create")'       > /dev/null
    echo "$output" | jq -e '.summary | has("to_update")'       > /dev/null
    echo "$output" | jq -e '.summary | has("to_wake")'         > /dev/null
    echo "$output" | jq -e '.summary | has("to_hibernate")'    > /dev/null
    echo "$output" | jq -e '.summary | has("unchanged")'       > /dev/null
}

@test "plan.sh --output json: planned_changes is a JSON array" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    arr_kind="$(echo "$output" | jq -r '.planned_changes | type')"
    [[ "$arr_kind" == "array" ]]
}

# ---------------------------------------------------------------------------
# Tests: CREATE scenario
# ---------------------------------------------------------------------------

@test "plan.sh --output json: to_create incremented when agent absent from Agamemnon" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    to_create="$(echo "$output" | jq -r '.summary.to_create')"
    [[ "$to_create" -ge 1 ]]
}

@test "plan.sh --output json: changes_needed is true when CREATE detected" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    changes_needed="$(echo "$output" | jq -r '.summary.changes_needed')"
    [[ "$changes_needed" == "true" ]]
}

@test "plan.sh --output json: planned_changes entries have name, action, details fields" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    len="$(echo "$output" | jq '.planned_changes | length')"
    [[ "$len" -ge 1 ]]

    echo "$output" | jq -e '.planned_changes[0] | has("name")'    > /dev/null
    echo "$output" | jq -e '.planned_changes[0] | has("action")'  > /dev/null
    echo "$output" | jq -e '.planned_changes[0] | has("details")' > /dev/null
}

@test "plan.sh --output json: CREATE action recorded in planned_changes" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    action="$(echo "$output" | jq -r '.planned_changes[0].action')"
    [[ "$action" == "CREATE" ]]
}

@test "plan.sh --output json: agent name is correct in planned_changes" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    name="$(echo "$output" | jq -r '.planned_changes[0].name')"
    [[ "$name" == "test-agent" ]]
}

# ---------------------------------------------------------------------------
# Tests: UNCHANGED scenario
# ---------------------------------------------------------------------------

@test "plan.sh --output json: UNCHANGED when agent matches Agamemnon exactly" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    echo "$output" | jq . > /dev/null

    changes_needed="$(echo "$output" | jq -r '.summary.changes_needed')"
    [[ "$changes_needed" == "false" ]]

    unchanged="$(echo "$output" | jq -r '.summary.unchanged')"
    [[ "$unchanged" -eq 1 ]]
}

@test "plan.sh --output json: UNCHANGED exits 0" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 0 ]]
}

@test "plan.sh --output json: CREATE exits 1" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 1 ]]
}

# ---------------------------------------------------------------------------
# Tests: metadata fields
# ---------------------------------------------------------------------------

@test "plan.sh --output json: agamemnon_url field matches AGAMEMNON_URL env var" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    url="$(echo "$output" | jq -r '.agamemnon_url')"
    [[ "$url" == "http://127.0.0.1:${MOCK_PORT}" ]]
}

@test "plan.sh --output json: host field reflects the host argument" {
    _start_mock_server 200 '[]'

    # The test host is set inside _run_plan_json; capture it before running
    _setup_test_agent "${FIXTURES_DIR}/agent-valid.yaml"
    local test_host="$_PLAN_TEST_HOST"
    run bash "$PLAN_SH" "$test_host" --output json
    _cleanup_test_agent

    host="$(echo "$output" | jq -r '.host')"
    [[ "$host" == "$test_host" ]]
}

@test "plan.sh --output json: stdout contains only valid JSON with no preamble" {
    _start_mock_server 200 '[]'

    _run_plan_json "${FIXTURES_DIR}/agent-valid.yaml"

    # jq -r 'type' fails on non-JSON; must return 'object'
    parsed="$(echo "$output" | jq -r 'type')"
    [[ "$parsed" == "object" ]]
}
