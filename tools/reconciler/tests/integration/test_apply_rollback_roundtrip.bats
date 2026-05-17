#!/usr/bin/env bats
# tests/integration/test_apply_rollback_roundtrip.bats
#
# Integration test: full apply -> rollback round-trip (#227)
#
# Mocks the Agamemnon API, runs apply.sh to snapshot + reconcile, then
# runs rollback.sh and verifies the pre-rollback snapshot is created and
# the restore is attempted.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"
MOCK_PORT=18082
MOCK_PID_FILE="/tmp/bats-roundtrip-mock-$$.pid"

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
# Setup / teardown
# ---------------------------------------------------------------------------

setup() {
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"

    # Isolated temp workspace for this test run
    TEST_WORKSPACE="$(mktemp -d)"
    export TEST_WORKSPACE

    # Create minimal agents directory with one agent YAML
    mkdir -p "${TEST_WORKSPACE}/agents/hermes"
    printf 'apiVersion: myrmidons/v1\nkind: Agent\nmetadata:\n  name: roundtrip-agent\n  host: hermes\nspec:\n  label: RoundTrip Agent\n  program: claude-code\n  model: null\n  workingDirectory: /tmp/roundtrip\n  programArgs: ""\n  taskDescription: "Integration test agent"\n  tags: [integration, test]\n  owner: testuser\n  role: member\n  deployment:\n    type: local\n  desiredState: active\n' \
        > "${TEST_WORKSPACE}/agents/hermes/roundtrip-agent.yaml"

    SNAPSHOT_DIR="${TEST_WORKSPACE}/.myrmidons/snapshots"
    export SNAPSHOT_DIR
}

teardown() {
    _stop_mock_server
    rm -rf "${TEST_WORKSPACE:-}"
}

# ---------------------------------------------------------------------------
# Helper: count files matching a pattern in SNAPSHOT_DIR
# ---------------------------------------------------------------------------

_count_snapshots() {
    local pattern="${1:-*.json}"
    find "$SNAPSHOT_DIR" -maxdepth 1 -name "$pattern" 2>/dev/null | wc -l
}

# ---------------------------------------------------------------------------
# Test: apply creates a pre-apply snapshot with context fields (#228)
# ---------------------------------------------------------------------------

@test "apply: creates snapshot with context fields (user, git_branch, host, timestamp)" {
    # Mock: health check OK, list returns empty (no existing agents)
    _start_mock_server 200 '[]'

    # Run apply.sh, pointing snapshot-dir to our isolated workspace
    run bash "${SCRIPT_DIR}/scripts/apply.sh" \
        --snapshot-dir "$SNAPSHOT_DIR" \
        2>&1 || true

    # A snapshot file must have been written
    local snap_count
    snap_count="$(_count_snapshots '*.json')"
    [[ "$snap_count" -ge 1 ]]

    # Find the snapshot and verify context fields
    local snap_file
    snap_file="$(find "$SNAPSHOT_DIR" -maxdepth 1 -name '*.json' | head -1)"
    [[ -f "$snap_file" ]]

    local snap_json
    snap_json="$(cat "$snap_file")"

    # Must have a context object
    echo "$snap_json" | jq -e '.context' > /dev/null

    # context.user must be non-empty
    local ctx_user
    ctx_user="$(echo "$snap_json" | jq -r '.context.user')"
    [[ -n "$ctx_user" && "$ctx_user" != "null" ]]

    # context.git_branch must be non-empty
    local ctx_branch
    ctx_branch="$(echo "$snap_json" | jq -r '.context.git_branch')"
    [[ -n "$ctx_branch" && "$ctx_branch" != "null" ]]

    # context.timestamp must match ISO-8601 pattern
    local ctx_ts
    ctx_ts="$(echo "$snap_json" | jq -r '.context.timestamp')"
    [[ "$ctx_ts" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]]

    # context.host field must be present
    echo "$snap_json" | jq -e '.context.host' > /dev/null

    # agents array must be present
    echo "$snap_json" | jq -e '.agents | type == "array"' > /dev/null
}

# ---------------------------------------------------------------------------
# Test: apply -> rollback round-trip (#227)
# Apply runs, writes a snapshot. Rollback reads it, writes pre-rollback
# snapshot, and attempts to restore.
# ---------------------------------------------------------------------------

@test "round-trip: apply creates snapshot, rollback creates pre-rollback snapshot" {
    # Mock: health OK, list returns one agent already existing
    local existing_agent
    existing_agent='[{"id":"existing-id","name":"roundtrip-agent","status":"active","label":"RoundTrip Agent","program":"claude-code","workingDirectory":"/tmp/roundtrip","programArgs":"","taskDescription":"Integration test agent","tags":["integration","test"],"owner":"testuser","role":"member"}]'
    _start_mock_server 200 "$existing_agent"

    # Step 1: run apply.sh -- should write a pre-apply snapshot
    run bash "${SCRIPT_DIR}/scripts/apply.sh" \
        --snapshot-dir "$SNAPSHOT_DIR" \
        2>&1 || true

    # Verify at least one snapshot was created
    local snap_count_before
    snap_count_before="$(_count_snapshots '*.json')"
    [[ "$snap_count_before" -ge 1 ]]

    _stop_mock_server

    # Step 2: restart mock so rollback can call agamemnon_check_connection and list
    _start_mock_server 200 "$existing_agent"

    # Step 3: run rollback.sh pointing at our snapshot dir.
    # It should: (a) find the snapshot, (b) list current agents, (c) write pre-rollback snapshot
    run bash "${SCRIPT_DIR}/scripts/rollback.sh" \
        --snapshot-dir "$SNAPSHOT_DIR" \
        2>&1 || true

    # A pre-rollback snapshot must now exist (filename contains "pre-rollback")
    local pre_rollback_count
    pre_rollback_count="$(_count_snapshots '*.pre-rollback.json')"
    [[ "$pre_rollback_count" -ge 1 ]]

    # The rollback output must mention the pre-rollback snapshot
    [[ "$output" == *"pre-rollback"* ]]
}

# ---------------------------------------------------------------------------
# Test: rollback --dry-run does NOT write a pre-rollback snapshot (#225)
# ---------------------------------------------------------------------------

@test "rollback --dry-run: does not write pre-rollback snapshot" {
    # Create a minimal snapshot manually so rollback has something to read
    mkdir -p "$SNAPSHOT_DIR"
    local ts
    ts="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    jq -n \
        --arg ts "$ts" \
        '{context:{timestamp:$ts,user:"ci",git_branch:"main",host:"all"},agents:[{"id":"a1","name":"roundtrip-agent","status":"active"}]}' \
        > "${SNAPSHOT_DIR}/${ts}.json"

    run bash "${SCRIPT_DIR}/scripts/rollback.sh" \
        --snapshot-dir "$SNAPSHOT_DIR" \
        --dry-run \
        2>&1

    # No pre-rollback snapshot should have been written during dry-run
    local pre_rollback_count
    pre_rollback_count="$(_count_snapshots '*.pre-rollback.json')"
    [[ "$pre_rollback_count" -eq 0 ]]

    # Output should contain DRY-RUN or Dry run marker
    [[ "$output" == *"DRY-RUN"* || "$output" == *"Dry run"* ]]
}

# ---------------------------------------------------------------------------
# Test: rollback can restore from a snapshot written by apply (#227 full cycle)
# ---------------------------------------------------------------------------

@test "round-trip: rollback reads apply snapshot and attempts restore of each agent" {
    # Write a snapshot that looks like it came from apply
    mkdir -p "$SNAPSHOT_DIR"
    local ts
    ts="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    jq -n \
        --arg ts "$ts" \
        '{
            context: {timestamp:$ts, user:"testuser", git_branch:"main", host:"hermes"},
            agents: [{
                id: "agent-id-1",
                name: "roundtrip-agent",
                status: "active",
                label: "RoundTrip Agent",
                program: "claude-code",
                workingDirectory: "/tmp/roundtrip",
                programArgs: "",
                taskDescription: "Integration test agent",
                tags: ["integration","test"],
                owner: "testuser",
                role: "member"
            }]
        }' > "${SNAPSHOT_DIR}/${ts}.json"

    # Mock: current live state (agent exists, will be patched during restore)
    local live_agents
    live_agents='[{"id":"agent-id-1","name":"roundtrip-agent","status":"active","label":"RoundTrip Agent","program":"claude-code","workingDirectory":"/tmp/roundtrip","programArgs":"","taskDescription":"Integration test agent","tags":["integration","test"],"owner":"testuser","role":"member"}]'
    _start_mock_server 200 "$live_agents"

    run bash "${SCRIPT_DIR}/scripts/rollback.sh" \
        --snapshot-dir "$SNAPSHOT_DIR" \
        2>&1 || true

    # Rollback must mention the agent being processed
    [[ "$output" == *"roundtrip-agent"* ]]

    # A pre-rollback snapshot must have been created
    local pre_rollback_count
    pre_rollback_count="$(_count_snapshots '*.pre-rollback.json')"
    [[ "$pre_rollback_count" -ge 1 ]]

    # Verify the pre-rollback snapshot is valid JSON with context
    local pre_snap
    pre_snap="$(find "$SNAPSHOT_DIR" -maxdepth 1 -name '*.pre-rollback.json' | head -1)"
    jq -e '.context' "$pre_snap" > /dev/null
    jq -e '.agents | type == "array"' "$pre_snap" > /dev/null
}
