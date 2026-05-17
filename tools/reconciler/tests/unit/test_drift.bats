#!/usr/bin/env bats
# tests/unit/test_drift.bats — unit tests for compute_drift function in scripts/lib/reconcile.sh

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# Source reconcile.sh before each test (without api.sh dependency)
setup() {
    # Stub out api.sh sourcing to avoid connection errors
    export AGAMEMNON_URL="http://localhost:19999"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
}

# ---------------------------------------------------------------------------
# compute_drift: Test helpers
# ---------------------------------------------------------------------------

# Helper to build a minimal actual_json
# Note: $label is a reserved keyword in jq 1.6; use $lbl to avoid parser conflict.
_make_actual() {
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

# ---------------------------------------------------------------------------
# compute_drift: UNCHANGED paths
# ---------------------------------------------------------------------------

@test "compute_drift UNCHANGED: all fields match, agent active" {
    actual="$(_make_actual "active" "Test Agent" "claude-code" "/home/mvillmow/TestProject" "" "A test agent" '["ci","testing"]')"
    result="$(compute_drift "test-agent" "active" "$actual" "Test Agent" "claude-code" "/home/mvillmow/TestProject" "" "A test agent" "ci,testing" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift UNCHANGED: hibernated agent matches hibernated desired state" {
    actual="$(_make_actual "offline" "Sleeping Agent" "aider" "/tmp/proj" "" "" '[]')"
    result="$(compute_drift "sleep-agent" "hibernated" "$actual" "Sleeping Agent" "aider" "/tmp/proj" "" "" "" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift UNCHANGED: tags in different order sorted to same result" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '["beta","alpha"]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "alpha,beta" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift UNCHANGED: tilde path normalized with full path" {
    actual="$(_make_actual "active" "Agent" "claude-code" "${HOME}/Projects" "" "" '[]')"
    # shellcheck disable=SC2088
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" '~/Projects' "" "" "" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift UNCHANGED: both tags empty" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

# ---------------------------------------------------------------------------
# compute_drift: WAKE path (desired=active, actual=offline)
# ---------------------------------------------------------------------------

@test "compute_drift WAKE: desired=active, actual=offline" {
    actual="$(_make_actual "offline" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "WAKE" ]]
}

@test "compute_drift WAKE: returns WAKE even if other fields differ" {
    actual="$(_make_actual "offline" "Old Label" "old-prog" "/old/path" "--old" "Old desc" '["old-tag"]')"
    result="$(compute_drift "agent" "active" "$actual" "New Label" "new-prog" "/new/path" "--new" "New desc" "new-tag" "" "" "" "local")"
    [[ "$result" == "WAKE" ]]
}

# ---------------------------------------------------------------------------
# compute_drift: HIBERNATE path (desired=hibernated, actual=active/online)
# ---------------------------------------------------------------------------

@test "compute_drift HIBERNATE: desired=hibernated, actual=active" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "hibernated" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "HIBERNATE" ]]
}

@test "compute_drift HIBERNATE: desired=hibernated, actual=online" {
    actual="$(_make_actual "online" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "hibernated" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == "HIBERNATE" ]]
}

@test "compute_drift HIBERNATE: returns HIBERNATE even if other fields differ" {
    actual="$(_make_actual "active" "Old Label" "old-prog" "/old/path" "--old" "Old desc" '["old-tag"]')"
    result="$(compute_drift "agent" "hibernated" "$actual" "New Label" "new-prog" "/new/path" "--new" "New desc" "new-tag" "" "" "" "local")"
    [[ "$result" == "HIBERNATE" ]]
}

# ---------------------------------------------------------------------------
# compute_drift: UPDATE path (field-level differences)
# ---------------------------------------------------------------------------

@test "compute_drift UPDATE: label differs" {
    actual="$(_make_actual "active" "Old Label" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "New Label" "claude-code" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"label"* ]]
}

@test "compute_drift UPDATE: program differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "aider" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"program"* ]]
}

@test "compute_drift UPDATE: workingDirectory differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/old/path" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/new/path" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"workingDirectory"* ]]
}

@test "compute_drift UPDATE: programArgs differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "--old" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "--new" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"programArgs"* ]]
}

@test "compute_drift UPDATE: taskDescription differs" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "Old description" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "New description" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"taskDescription"* ]]
}

@test "compute_drift UPDATE: tags differ" {
    actual="$(_make_actual "active" "Agent" "claude-code" "/tmp" "" "" '["tag1"]')"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "tag1,tag2" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"tags"* ]]
}

@test "compute_drift UPDATE: multiple fields differ, lists all in comma-separated format" {
    actual="$(_make_actual "active" "Old Label" "old-prog" "/tmp" "" "" '[]')"
    result="$(compute_drift "agent" "active" "$actual" "New Label" "new-prog" "/tmp" "" "" "" "" "" "" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"label"* ]]
    [[ "$result" == *"program"* ]]
}
