#!/usr/bin/env bats
# tests/integration/test_reconcile_workflow.bats
#
# Integration tests for end-to-end reconciliation workflows.
# Exercises the full parse → compute_drift → build_create_json pipeline
# and verifies API function behaviour against a mock HTTP server.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/tests/fixtures"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"
MOCK_PORT=18081
MOCK_PID_FILE="/tmp/bats-integ-mock-$$.pid"

# ---------------------------------------------------------------------------
# Mock server helpers
# ---------------------------------------------------------------------------

_start_mock_server() {
    local http_status="${1:-200}"
    # Avoid ":-{}" bash brace-default parsing issue; use explicit fallback.
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
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"
}

teardown() {
    _stop_mock_server
}

# ---------------------------------------------------------------------------
# Workflow: CREATE — agent exists in YAML but not in Agamemnon
# ---------------------------------------------------------------------------

@test "workflow: CREATE — parse YAML then build create JSON for new agent" {
    # Parse the valid agent fixture
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-valid.yaml")

    # Build the create JSON
    create_json="$(build_create_json \
        "${fields[name]}" \
        "${fields[label]}" \
        "${fields[program]}" \
        "${fields[workingDirectory]}" \
        "${fields[programArgs]}" \
        "${fields[taskDescription]}" \
        "${fields[tags]}" \
        "${fields[owner]}" \
        "${fields[role]}")"

    # Verify the JSON is valid and has correct fields
    echo "$create_json" | jq . > /dev/null
    [[ "$(echo "$create_json" | jq -r '.name')" == "test-agent" ]]
    [[ "$(echo "$create_json" | jq -r '.program')" == "claude-code" ]]
    [[ "$(echo "$create_json" | jq -r '.workingDirectory')" == "/home/mvillmow/TestProject" ]]
}

@test "workflow: CREATE — create_agent call succeeds against mock server" {
    _start_mock_server 201 '{"id":"new-uuid","name":"test-agent","status":"offline"}'

    body='{"name":"test-agent","program":"claude-code","workingDirectory":"/tmp"}'
    result="$(agamemnon_create_agent "$body")"
    [[ "$(echo "$result" | jq -r '.id')" == "new-uuid" ]]
    [[ "$(echo "$result" | jq -r '.name')" == "test-agent" ]]
}

# ---------------------------------------------------------------------------
# Workflow: UNCHANGED — agent matches desired state
# ---------------------------------------------------------------------------

@test "workflow: UNCHANGED — agent matches desired state exactly" {
    # Parse agent YAML
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-valid.yaml")

    # Simulate API response matching the YAML
    actual_json="$(jq -n \
        --arg lbl "${fields[label]}" \
        --arg program "${fields[program]}" \
        --arg workingDirectory "${fields[workingDirectory]}" \
        --arg programArgs "${fields[programArgs]}" \
        --arg taskDescription "${fields[taskDescription]}" \
        --arg owner "${fields[owner]}" \
        --arg role "${fields[role]}" \
        --arg model "${fields[model]}" \
        '{
            status: "active",
            label: $lbl,
            program: $program,
            workingDirectory: $workingDirectory,
            programArgs: $programArgs,
            taskDescription: $taskDescription,
            tags: ["testing","ci"],
            owner: $owner,
            role: $role,
            model: $model,
            deployment: {type: "local"}
        }')"

    result="$(compute_drift \
        "${fields[name]}" \
        "${fields[desiredState]}" \
        "$actual_json" \
        "${fields[label]}" \
        "${fields[program]}" \
        "${fields[workingDirectory]}" \
        "${fields[programArgs]}" \
        "${fields[taskDescription]}" \
        "${fields[tags]}" \
        "${fields[model]:-}" \
        "${fields[owner]:-}" \
        "${fields[role]:-}" \
        "${fields[deploymentType]:-local}")"

    [[ "$result" == "UNCHANGED" ]]
}

# ---------------------------------------------------------------------------
# Workflow: WAKE — agent is offline but should be active
# ---------------------------------------------------------------------------

@test "workflow: WAKE — offline agent with active desired state" {
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-valid.yaml")

    # Simulate agent is offline in Agamemnon
    actual_json="$(jq -n \
        --arg lbl "${fields[label]}" \
        --arg program "${fields[program]}" \
        --arg workingDirectory "${fields[workingDirectory]}" \
        --arg programArgs "${fields[programArgs]}" \
        --arg taskDescription "${fields[taskDescription]}" \
        --arg owner "${fields[owner]}" \
        --arg role "${fields[role]}" \
        --arg model "${fields[model]}" \
        '{
            status: "offline",
            label: $lbl,
            program: $program,
            workingDirectory: $workingDirectory,
            programArgs: $programArgs,
            taskDescription: $taskDescription,
            tags: ["testing","ci"],
            owner: $owner,
            role: $role,
            model: $model,
            deployment: {type: "local"}
        }')"

    result="$(compute_drift \
        "${fields[name]}" \
        "active" \
        "$actual_json" \
        "${fields[label]}" \
        "${fields[program]}" \
        "${fields[workingDirectory]}" \
        "${fields[programArgs]}" \
        "${fields[taskDescription]}" \
        "${fields[tags]}" \
        "${fields[model]:-}" \
        "${fields[owner]:-}" \
        "${fields[role]:-}" \
        "${fields[deploymentType]:-local}")"

    [[ "$result" == "WAKE" ]]
}

@test "workflow: WAKE — wake_agent API call succeeds" {
    _start_mock_server 200 '{"id":"abc123","status":"active"}'
    result="$(agamemnon_wake_agent "abc123")"
    [[ "$(echo "$result" | jq -r '.status')" == "active" ]]
}

# ---------------------------------------------------------------------------
# Workflow: HIBERNATE — agent is active but should be hibernated
# ---------------------------------------------------------------------------

@test "workflow: HIBERNATE — active agent with hibernated desired state" {
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-hibernated.yaml")

    actual_json="$(jq -n \
        --arg lbl "${fields[label]}" \
        --arg program "${fields[program]}" \
        --arg workingDirectory "${fields[workingDirectory]}" \
        --arg programArgs "${fields[programArgs]}" \
        --arg taskDescription "${fields[taskDescription]}" \
        --arg owner "${fields[owner]}" \
        --arg role "${fields[role]}" \
        --arg model "${fields[model]}" \
        '{
            status: "active",
            label: $lbl,
            program: $program,
            workingDirectory: $workingDirectory,
            programArgs: $programArgs,
            taskDescription: $taskDescription,
            tags: ["hibernated"],
            owner: $owner,
            role: $role,
            model: $model,
            deployment: {type: "local"}
        }')"

    result="$(compute_drift \
        "${fields[name]}" \
        "hibernated" \
        "$actual_json" \
        "${fields[label]}" \
        "${fields[program]}" \
        "${fields[workingDirectory]}" \
        "${fields[programArgs]}" \
        "${fields[taskDescription]}" \
        "${fields[tags]}" \
        "${fields[model]:-}" \
        "${fields[owner]:-}" \
        "${fields[role]:-}" \
        "${fields[deploymentType]:-local}")"

    [[ "$result" == "HIBERNATE" ]]
}

@test "workflow: HIBERNATE — hibernate_agent API call succeeds" {
    _start_mock_server 200 '{"id":"abc123","status":"offline"}'
    result="$(agamemnon_hibernate_agent "abc123")"
    [[ "$(echo "$result" | jq -r '.status')" == "offline" ]]
}

# ---------------------------------------------------------------------------
# Workflow: UPDATE — agent exists but fields have drifted
# ---------------------------------------------------------------------------

@test "workflow: UPDATE — detect label drift and trigger update" {
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-valid.yaml")

    # Simulate Agamemnon has a stale label
    actual_json="$(jq -n \
        --arg program "${fields[program]}" \
        --arg workingDirectory "${fields[workingDirectory]}" \
        --arg programArgs "${fields[programArgs]}" \
        --arg taskDescription "${fields[taskDescription]}" \
        --arg owner "${fields[owner]}" \
        --arg role "${fields[role]}" \
        --arg model "${fields[model]}" \
        '{
            status: "active",
            label: "Stale Label",
            program: $program,
            workingDirectory: $workingDirectory,
            programArgs: $programArgs,
            taskDescription: $taskDescription,
            tags: ["testing","ci"],
            owner: $owner,
            role: $role,
            model: $model,
            deployment: {type: "local"}
        }')"

    result="$(compute_drift \
        "${fields[name]}" \
        "${fields[desiredState]}" \
        "$actual_json" \
        "${fields[label]}" \
        "${fields[program]}" \
        "${fields[workingDirectory]}" \
        "${fields[programArgs]}" \
        "${fields[taskDescription]}" \
        "${fields[tags]}" \
        "${fields[model]:-}" \
        "${fields[owner]:-}" \
        "${fields[role]:-}" \
        "${fields[deploymentType]:-local}")"

    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"label"* ]]
}

@test "workflow: UPDATE — update_agent API call succeeds" {
    _start_mock_server 200 '{"id":"abc123","label":"New Label","status":"active"}'
    result="$(agamemnon_update_agent "abc123" '{"label":"New Label"}')"
    [[ "$(echo "$result" | jq -r '.label')" == "New Label" ]]
}

# ---------------------------------------------------------------------------
# Workflow: API failure handling
# ---------------------------------------------------------------------------

@test "workflow: API failures are propagated — list fails on 500" {
    _start_mock_server 500 '{"error":"internal server error"}'
    run agamemnon_list_agents
    [[ "$status" -ne 0 ]]
}

@test "workflow: lookup by name works via list then filter" {
    local agents_json='[{"id":"id-xyz","name":"test-agent","status":"active"},{"id":"id-abc","name":"other-agent","status":"offline"}]'
    _start_mock_server 200 "$agents_json"
    agent_id="$(agamemnon_id_by_name "test-agent")"
    [[ "$agent_id" == "id-xyz" ]]
}

# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------

@test "edge case: minimal YAML parses and produces valid create JSON" {
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-minimal.yaml")

    create_json="$(build_create_json \
        "${fields[name]}" \
        "${fields[label]:-}" \
        "${fields[program]}" \
        "${fields[workingDirectory]}" \
        "${fields[programArgs]:-}" \
        "${fields[taskDescription]:-}" \
        "${fields[tags]:-}" \
        "${fields[owner]:-}" \
        "${fields[role]:-member}")"

    echo "$create_json" | jq . > /dev/null
    [[ "$(echo "$create_json" | jq -r '.name')" == "minimal-agent" ]]
}

@test "edge case: docker agent YAML parses docker fields correctly" {
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="$value"
    done < <(parse_agent_yaml "${FIXTURES_DIR}/agent-docker.yaml")

    [[ "${fields[deploymentType]}" == "docker" ]]
    [[ "${fields[dockerImage]}" == "my-claude:latest" ]]
    [[ "${fields[dockerCpus]}" == "2" ]]
    [[ "${fields[dockerMemory]}" == "4g" ]]
}
