#!/usr/bin/env bats
# tests/unit/test_compute_drift_arity.bats — arity guard tests for compute_drift
#
# Ensures compute_drift rejects calls with wrong argument counts, catching
# callers that were not updated in lockstep when a new parameter was added.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

setup() {
    export AGAMEMNON_URL="http://localhost:19999"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
}

# Minimal valid actual_json for 13-arg success tests
_minimal_actual() {
    jq -n '{status: "active", label: "A", program: "claude-code",
            workingDirectory: "/tmp", programArgs: "", taskDescription: "",
            tags: [], model: "", owner: "", role: "member",
            deployment: {type: "local"}}'
}

# ---------------------------------------------------------------------------
# Wrong-arity calls must fail with a descriptive error message
# ---------------------------------------------------------------------------

@test "compute_drift arity: 0 args → non-zero exit and error message" {
    run compute_drift
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"requires exactly 13 arguments"* ]]
}

@test "compute_drift arity: 12 args → non-zero exit and error message" {
    run compute_drift a b c d e f g h i j k l
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"requires exactly 13 arguments"* ]]
}

@test "compute_drift arity: 12 args → error message mentions got 12" {
    run compute_drift a b c d e f g h i j k l
    [[ "$output" == *"got 12"* ]]
}

@test "compute_drift arity: 14 args → non-zero exit and error message" {
    run compute_drift a b c d e f g h i j k l m n
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"requires exactly 13 arguments"* ]]
}

@test "compute_drift arity: 14 args → error message mentions got 14" {
    run compute_drift a b c d e f g h i j k l m n
    [[ "$output" == *"got 14"* ]]
}

@test "compute_drift arity: 1 arg → non-zero exit and error message" {
    run compute_drift only-one-arg
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"requires exactly 13 arguments"* ]]
}

# ---------------------------------------------------------------------------
# Correct arity (13 args) succeeds
# ---------------------------------------------------------------------------

@test "compute_drift arity: exactly 13 args → exits zero" {
    actual="$(_minimal_actual)"
    run compute_drift "agent" "active" "$actual" \
        "A" "claude-code" "/tmp" "" "" "" "" "" "member" "local"
    [[ "$status" -eq 0 ]]
}

@test "compute_drift arity: exactly 13 args → produces valid output" {
    actual="$(_minimal_actual)"
    run compute_drift "agent" "active" "$actual" \
        "A" "claude-code" "/tmp" "" "" "" "" "" "member" "local"
    [[ "$output" == "UNCHANGED" || "$output" == WAKE || "$output" == HIBERNATE || "$output" == UPDATE:* ]]
}
