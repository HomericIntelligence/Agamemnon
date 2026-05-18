#!/usr/bin/env bats
# tests/unit/test_drift_owner_role_tags.bats — Smoke tests for compute_drift with
# owner, role, and tags fields (#87)
#
# These tests exercise compute_drift using a full actual_json that includes
# owner, role, model, and deployment fields — matching what the Agamemnon API
# returns in production.  The _make_actual helper in test_drift.bats omits those
# fields; this file ensures drift is detected when they change.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

setup() {
    export AGAMEMNON_URL="http://localhost:19999"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
}

# Build a full agent JSON that matches what the Agamemnon API returns,
# including owner, role, model, and deployment sub-object.
_make_full_actual() {
    local status="$1" lbl="$2" program="$3" workdir="$4"
    local args="$5" desc="$6" tags_json="$7"
    local model="$8" owner="$9" role="${10}" deploy_type="${11}"
    # Note: $label is a reserved keyword in jq 1.6; use $lbl.
    jq -n \
        --arg status "$status" \
        --arg lbl "$lbl" \
        --arg program "$program" \
        --arg workingDirectory "$workdir" \
        --arg programArgs "$args" \
        --arg taskDescription "$desc" \
        --argjson tags "$tags_json" \
        --arg model "$model" \
        --arg owner "$owner" \
        --arg role "$role" \
        --arg deployType "$deploy_type" \
        '{
            status: $status,
            label: $lbl,
            program: $program,
            workingDirectory: $workingDirectory,
            programArgs: $programArgs,
            taskDescription: $taskDescription,
            tags: $tags,
            model: $model,
            owner: $owner,
            role: $role,
            deployment: { type: $deployType }
        }'
}

# ---------------------------------------------------------------------------
# Smoke: UNCHANGED with owner/role/tags all set
# ---------------------------------------------------------------------------

@test "compute_drift smoke: UNCHANGED when owner matches" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "alice" "member" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift smoke: UNCHANGED when role matches" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]' "" "alice" "admin" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "alice" "admin" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift smoke: UNCHANGED when tags match (single tag)" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["ops"]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "ops" "" "alice" "member" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift smoke: UNCHANGED when tags match (multiple tags, same order)" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["alpha","beta","gamma"]' "" "mvillmow" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "alpha,beta,gamma" "" "mvillmow" "member" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

@test "compute_drift smoke: UNCHANGED when tags match out-of-order (sorted comparison)" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["gamma","alpha","beta"]' "" "mvillmow" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "beta,alpha,gamma" "" "mvillmow" "member" "local")"
    [[ "$result" == "UNCHANGED" ]]
}

# ---------------------------------------------------------------------------
# Smoke: drift detected when owner changes
# ---------------------------------------------------------------------------

@test "compute_drift smoke: UPDATE:owner when owner changes" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "bob" "member" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"owner"* ]]
}

@test "compute_drift smoke: UPDATE:owner only when only owner changes" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "bob" "member" "local")"
    # Should be exactly UPDATE:owner (no other fields drifted)
    [[ "$result" == "UPDATE:owner" ]]
}

# ---------------------------------------------------------------------------
# Smoke: drift detected when role changes
# ---------------------------------------------------------------------------

@test "compute_drift smoke: UPDATE:role when role changes" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "alice" "admin" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"role"* ]]
}

@test "compute_drift smoke: UPDATE:role only when only role changes" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '[]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "alice" "admin" "local")"
    [[ "$result" == "UPDATE:role" ]]
}

# ---------------------------------------------------------------------------
# Smoke: drift detected when tags change
# ---------------------------------------------------------------------------

@test "compute_drift smoke: UPDATE:tags when tag is added" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["ai"]' "" "mvillmow" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "ai,ops" "" "mvillmow" "member" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"tags"* ]]
}

@test "compute_drift smoke: UPDATE:tags when tag is removed" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["ai","ops"]' "" "mvillmow" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "ai" "" "mvillmow" "member" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"tags"* ]]
}

@test "compute_drift smoke: UPDATE:tags when tags cleared (non-empty to empty)" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["tag1"]' "" "mvillmow" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "" "" "mvillmow" "member" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"tags"* ]]
}

# ---------------------------------------------------------------------------
# Smoke: all three fields drifted simultaneously
# ---------------------------------------------------------------------------

@test "compute_drift smoke: UPDATE contains owner, role, and tags when all three change" {
    actual="$(_make_full_actual "active" "Agent" "claude-code" "/tmp" "" "" '["old-tag"]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "new-tag" "" "bob" "admin" "local")"
    [[ "$result" == UPDATE:* ]]
    [[ "$result" == *"owner"* ]]
    [[ "$result" == *"role"* ]]
    [[ "$result" == *"tags"* ]]
}

# ---------------------------------------------------------------------------
# Smoke: WAKE/HIBERNATE still take priority over field drift
# ---------------------------------------------------------------------------

@test "compute_drift smoke: WAKE takes priority over owner/role/tags drift" {
    # Actual is offline; desired is active — should WAKE even though owner/role/tags also differ
    actual="$(_make_full_actual "offline" "Agent" "claude-code" "/tmp" "" "" '["old"]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "active" "$actual" "Agent" "claude-code" "/tmp" "" "" "new" "" "bob" "admin" "local")"
    [[ "$result" == "WAKE" ]]
}

@test "compute_drift smoke: HIBERNATE takes priority over owner/role/tags drift" {
    actual="$(_make_full_actual "online" "Agent" "claude-code" "/tmp" "" "" '["old"]' "" "alice" "member" "local")"
    result="$(compute_drift "agent" "hibernated" "$actual" "Agent" "claude-code" "/tmp" "" "" "new" "" "bob" "admin" "local")"
    [[ "$result" == "HIBERNATE" ]]
}
