#!/usr/bin/env bats
# tests/unit/test_reconcile.bats — unit tests for scripts/lib/reconcile.sh

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/tests/fixtures"

# Source reconcile.sh before each test (without api.sh dependency)
setup() {
    # Stub out api.sh sourcing to avoid connection errors
    export AGAMEMNON_URL="http://localhost:19999"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
}

# ---------------------------------------------------------------------------
# normalize_path
# ---------------------------------------------------------------------------

@test "normalize_path: expands tilde to HOME" {
    # shellcheck disable=SC2088
    result="$(normalize_path '~/Projects/foo')"
    [[ "$result" == "${HOME}/Projects/foo" ]]
}

@test "normalize_path: leaves absolute path unchanged" {
    result="$(normalize_path "/absolute/path")"
    [[ "$result" == "/absolute/path" ]]
}

@test "normalize_path: leaves empty string as empty" {
    result="$(normalize_path "")"
    [[ "$result" == "" ]]
}

@test "normalize_path: does not expand tilde in the middle of path" {
    result="$(normalize_path "/home/user/~/foo")"
    [[ "$result" == "/home/user/~/foo" ]]
}

# ---------------------------------------------------------------------------
# parse_agent_yaml
# ---------------------------------------------------------------------------

@test "parse_agent_yaml: parses all fields from valid agent YAML" {
    output="$(parse_agent_yaml "${FIXTURES_DIR}/agent-valid.yaml")"
    [[ "$output" == *"name=test-agent"* ]]
    [[ "$output" == *"host=hermes"* ]]
    [[ "$output" == *"label=Test Agent"* ]]
    [[ "$output" == *"program=claude-code"* ]]
    [[ "$output" == *"workingDirectory=/home/mvillmow/TestProject"* ]]
    [[ "$output" == *"programArgs=--verbose"* ]]
    [[ "$output" == *"desiredState=active"* ]]
    [[ "$output" == *"deploymentType=local"* ]]
}

@test "parse_agent_yaml: parses tags as comma-separated string" {
    output="$(parse_agent_yaml "${FIXTURES_DIR}/agent-valid.yaml")"
    # tags should be joined with commas
    [[ "$output" == *"tags="* ]]
    tags_line="$(echo "$output" | grep '^tags=')"
    [[ "$tags_line" == *"testing"* ]]
    [[ "$tags_line" == *"ci"* ]]
}

@test "parse_agent_yaml: handles missing optional fields with defaults" {
    output="$(parse_agent_yaml "${FIXTURES_DIR}/agent-minimal.yaml")"
    [[ "$output" == *"name=minimal-agent"* ]]
    [[ "$output" == *"desiredState=active"* ]]
    [[ "$output" == *"deploymentType=local"* ]]
    [[ "$output" == *"role=member"* ]]
}

@test "parse_agent_yaml: parses hibernated desiredState" {
    output="$(parse_agent_yaml "${FIXTURES_DIR}/agent-hibernated.yaml")"
    [[ "$output" == *"desiredState=hibernated"* ]]
}

@test "parse_agent_yaml: parses docker deployment fields" {
    output="$(parse_agent_yaml "${FIXTURES_DIR}/agent-docker.yaml")"
    [[ "$output" == *"deploymentType=docker"* ]]
    [[ "$output" == *"dockerImage=my-claude:latest"* ]]
    [[ "$output" == *"dockerCpus=2"* ]]
    [[ "$output" == *"dockerMemory=4g"* ]]
}

@test "parse_agent_yaml: empty tags field when no tags defined" {
    output="$(parse_agent_yaml "${FIXTURES_DIR}/agent-no-tags.yaml")"
    tags_line="$(echo "$output" | grep '^tags=')"
    # Should be tags= with empty value
    [[ "$tags_line" == "tags=" ]]
}

# ---------------------------------------------------------------------------
# build_create_json
# ---------------------------------------------------------------------------

@test "build_create_json: produces valid JSON" {
    result="$(build_create_json "my-agent" "My Agent" "claude-code" "/home/user/proj" "" "Does stuff" "ai,ops" "mvillmow" "member")"
    echo "$result" | jq . > /dev/null  # fails if invalid JSON
}

@test "build_create_json: sets name field correctly" {
    result="$(build_create_json "my-agent" "My Agent" "claude-code" "/home/user/proj" "" "Does stuff" "" "mvillmow" "member")"
    name="$(echo "$result" | jq -r '.name')"
    [[ "$name" == "my-agent" ]]
}

@test "build_create_json: sets program field correctly" {
    result="$(build_create_json "agent" "Agent" "aider" "/tmp" "" "" "" "user" "member")"
    prog="$(echo "$result" | jq -r '.program')"
    [[ "$prog" == "aider" ]]
}

@test "build_create_json: converts CSV tags to JSON array" {
    result="$(build_create_json "agent" "Agent" "prog" "/tmp" "" "" "tag1,tag2,tag3" "user" "member")"
    tags="$(echo "$result" | jq -r '.tags | length')"
    [[ "$tags" == "3" ]]
    [[ "$(echo "$result" | jq -r '.tags[0]')" == "tag1" ]]
    [[ "$(echo "$result" | jq -r '.tags[1]')" == "tag2" ]]
    [[ "$(echo "$result" | jq -r '.tags[2]')" == "tag3" ]]
}

@test "build_create_json: produces empty array for empty tags" {
    result="$(build_create_json "agent" "Agent" "prog" "/tmp" "" "" "" "user" "member")"
    tags="$(echo "$result" | jq -r '.tags | length')"
    [[ "$tags" == "0" ]]
}

@test "build_create_json: handles programArgs with special characters" {
    result="$(build_create_json "agent" "Agent" "prog" "/tmp" "--flag value" "" "" "user" "member")"
    args="$(echo "$result" | jq -r '.programArgs')"
    [[ "$args" == "--flag value" ]]
}

@test "build_create_json: sets workingDirectory" {
    result="$(build_create_json "agent" "Agent" "prog" "/home/user/myproject" "" "" "" "user" "member")"
    workdir="$(echo "$result" | jq -r '.workingDirectory')"
    [[ "$workdir" == "/home/user/myproject" ]]
}

@test "build_create_json: sets owner and role" {
    result="$(build_create_json "agent" "Agent" "prog" "/tmp" "" "" "" "alice" "admin")"
    owner="$(echo "$result" | jq -r '.owner')"
    role="$(echo "$result" | jq -r '.role')"
    [[ "$owner" == "alice" ]]
    [[ "$role" == "admin" ]]
}

# ---------------------------------------------------------------------------
# compute_drift
# ---------------------------------------------------------------------------

# Helper to build a minimal actual_json
_make_actual() {
    # Note: $label is a reserved keyword in jq 1.6; use $lbl to avoid parser conflict.
    # Use explicit positional args to avoid bash ${:-} collapsing empty strings to defaults.
    jq -n \
        --arg status "$1" \
        --arg lbl "$2" \
        --arg program "$3" \
        --arg workingDirectory "$4" \
        --arg programArgs "$5" \
        --arg taskDescription "$6" \
        --argjson tags "$7" \
        '{status: $status, label: $lbl, program: $program,
          workingDirectory: $workingDirectory, programArgs: $programArgs,
          taskDescription: $taskDescription, tags: $tags}'
}

@test "compute_drift: UNCHANGED when all fields match and agent is active" {
    actual="$(_make_actual "active" "Test Agent" "claude-code" "/home/mvillmow/TestProject" "" "A test agent" '["ci","testing"]')"
    result="$(compute_drift "test-agent" "active" "$actual" "Test Agent" "claude-code" "/home/mvillmow/TestProject" "" "A test agent" "ci,testing" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift: UNCHANGED when hibernated agent matches hibernated desired state" {
    actual="$(_make_actual "offline" "Sleeping" "aider" "/tmp/proj" "" "" '[]')"
    result="$(compute_drift "sleep-agent" "hibernated" "$actual" "Sleeping" "aider" "/tmp/proj" "" "" "" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift: WAKE when desired=active and actual status=offline" {
    actual="$(_make_actual "offline" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "WAKE" ]]
}

@test "compute_drift: HIBERNATE when desired=hibernated and actual status=active" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "hibernated" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "HIBERNATE" ]]
}

@test "compute_drift: HIBERNATE when desired=hibernated and actual status=online" {
    actual="$(_make_actual "online" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "hibernated" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "HIBERNATE" ]]
}

@test "compute_drift: UPDATE when label differs" {
    actual="$(_make_actual "active" "Old Label" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "New Label" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"label"* ]]
}

@test "compute_drift: UPDATE when program differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "aider" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"program"* ]]
}

@test "compute_drift: UPDATE when workingDirectory differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/old/path" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/new/path" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"workingDirectory"* ]]
}

@test "compute_drift: UPDATE when programArgs differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "--old" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "--new" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"programArgs"* ]]
}

@test "compute_drift: UPDATE when taskDescription differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "Old description" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "New description" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"taskDescription"* ]]
}

@test "compute_drift: UPDATE when tags differ" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '["tag1"]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "tag1,tag2" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"tags"* ]]
}

@test "compute_drift: UPDATE lists all drifted fields" {
    actual="$(_make_actual "active" "Old Label" "old-prog" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "New Label" "new-prog" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"label"* ]]
    [[ "$result" == *"program"* ]]
}

@test "compute_drift: UNCHANGED when tags are in different order (sorted comparison)" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '["beta","alpha"]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "alpha,beta" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift: UNCHANGED when workingDirectory uses tilde and actual uses full path" {
    actual="$(_make_actual "active" "Agent" "claude-code" "${HOME}/Projects" "" "" '[]')"
    # shellcheck disable=SC2088
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" '~/Projects' "" "" "" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift: UNCHANGED when both tags are empty" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}
