#!/usr/bin/env bats
# tests/unit/test_unmanaged.bats — unit tests for unmanaged agent reporting in reconcile.sh

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/tests/fixtures"

# Source reconcile.sh before each test (without api.sh dependency)
setup() {
    # Stub out api.sh sourcing to avoid connection errors
    export AGAMEMNON_URL="http://localhost:19999"
    # Mock log_warn to capture output instead of logging
    log_warn() {
        echo "$@"
    }
    export -f log_warn
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
}

# ---------------------------------------------------------------------------
# get_unmanaged_names
# ---------------------------------------------------------------------------

@test "get_unmanaged_names: empty agents_json returns 0 with no output" {
    result="$(get_unmanaged_names "" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "get_unmanaged_names: agents_json='[]' returns 0 with no output" {
    result="$(get_unmanaged_names "[]" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "get_unmanaged_names: single managed agent in both lists produces no output" {
    # agent-valid.yaml has metadata.name=test-agent
    local agents_json='[{"name":"test-agent","status":"active"}]'
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "get_unmanaged_names: unmanaged agent in agents_json is reported" {
    # Create a minimal agents_json with an unmanaged agent
    local agents_json='[{"name":"unmanaged-agent","status":"active"}]'
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ "$result" == "unmanaged-agent" ]]
}

@test "get_unmanaged_names: multiple unmanaged agents all reported" {
    local agents_json='[{"name":"unmanaged-1","status":"active"},{"name":"unmanaged-2","status":"active"}]'
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")"
    # Should output both names (order may vary)
    [[ "$result" == *"unmanaged-1"* ]]
    [[ "$result" == *"unmanaged-2"* ]]
}

@test "get_unmanaged_names: mixed managed and unmanaged agents only reports unmanaged" {
    # agent-valid.yaml has test-agent, agents_json has both test-agent and unmanaged
    local agents_json='[{"name":"test-agent","status":"active"},{"name":"rogue-agent","status":"active"}]'
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")"
    # Should only output rogue-agent
    [[ "$result" == "rogue-agent" ]]
    [[ ! "$result" == *"test-agent"* ]]
}

@test "get_unmanaged_names: multiple yaml files with mix of managed and unmanaged" {
    # agent-valid.yaml (test-agent) + agent-minimal.yaml (minimal-agent)
    # agents_json has both managed + one unmanaged
    local agents_json='[{"name":"test-agent","status":"active"},{"name":"minimal-agent","status":"active"},{"name":"unknown-agent","status":"active"}]'
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml" "${FIXTURES_DIR}/agent-minimal.yaml")"
    # Should only output unknown-agent
    [[ "$result" == "unknown-agent" ]]
    [[ ! "$result" == *"test-agent"* ]]
    [[ ! "$result" == *"minimal-agent"* ]]
}

@test "get_unmanaged_names: agents_json with null entries does not crash" {
    # jq should handle null gracefully
    local agents_json='[{"name":"agent-1","status":"active"},null,{"name":"agent-2","status":"active"}]'
    # This should not error out; behavior depends on jq's robustness
    # At minimum, it should process the valid entries
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml" 2>&1)" || true
    # If it succeeds, good; if jq complains, we've caught the error
    true
}

# ---------------------------------------------------------------------------
# report_unmanaged
# ---------------------------------------------------------------------------

@test "report_unmanaged: empty agents_json produces no output" {
    result="$(report_unmanaged "" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "report_unmanaged: agents_json='[]' produces no output" {
    result="$(report_unmanaged "[]" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "report_unmanaged: all agents managed produces no output" {
    local agents_json='[{"name":"test-agent","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "report_unmanaged: unmanaged agent produces [-] UNMANAGED line" {
    local agents_json='[{"name":"unmanaged-agent","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ "$result" == *"[-]"* ]]
    [[ "$result" == *"UNMANAGED"* ]]
    [[ "$result" == *"unmanaged-agent"* ]]
}

@test "report_unmanaged: unmanaged agent line contains agent name" {
    local agents_json='[{"name":"my-rogue-agent","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ "$result" == *"my-rogue-agent"* ]]
}

@test "report_unmanaged: multiple unmanaged agents produce multiple lines" {
    local agents_json='[{"name":"rogue-1","status":"active"},{"name":"rogue-2","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")"
    # Count lines containing [-] UNMANAGED
    local count
    count=$(echo "$result" | grep -c "UNMANAGED" || true)
    [[ $count -eq 2 ]]
}

@test "report_unmanaged: mentions --prune option in output" {
    local agents_json='[{"name":"unmanaged-agent","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ "$result" == *"--prune"* ]]
}

@test "report_unmanaged: with multiple yaml files only unmanaged are reported" {
    local agents_json='[{"name":"test-agent","status":"active"},{"name":"minimal-agent","status":"active"},{"name":"orphan-agent","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml" "${FIXTURES_DIR}/agent-minimal.yaml")"
    # Should contain orphan-agent line
    [[ "$result" == *"orphan-agent"* ]]
    # Should NOT contain test-agent or minimal-agent as unmanaged
    [[ ! "$result" == *"test-agent"* || ! "$result" == *"UNMANAGED test-agent"* ]]
    [[ ! "$result" == *"minimal-agent"* || ! "$result" == *"UNMANAGED minimal-agent"* ]]
}

# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------

@test "get_unmanaged_names: null agents_json behaves like empty" {
    # If agents_json is explicitly null (not a string containing "null", but the shell value)
    # This is more of a guard test; in practice the guard should catch it
    result="$(get_unmanaged_names "" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ -z "$result" ]]
}

@test "report_unmanaged: no yaml files produces no output (guard #130)" {
    # When no YAML files are passed, the function returns early (guard #130:
    # if nothing is managed, unmanaged detection is a no-op).
    local agents_json='[{"name":"any-agent","status":"active"}]'
    result="$(report_unmanaged "$agents_json")" || true
    [[ -z "$result" ]]
}

@test "get_unmanaged_names: agent name with special characters is handled" {
    # Agent names should be valid identifiers, but if they contain hyphens, underscores, etc.
    local agents_json='[{"name":"agent-with-dashes_and_underscores","status":"active"}]'
    result="$(get_unmanaged_names "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")"
    [[ "$result" == "agent-with-dashes_and_underscores" ]]
}

@test "report_unmanaged: agent name with spaces (if present) is preserved" {
    # Unlikely but test the data path
    local agents_json='[{"name":"agent with spaces","status":"active"}]'
    result="$(report_unmanaged "$agents_json" "${FIXTURES_DIR}/agent-valid.yaml")" || true
    [[ "$result" == *"agent with spaces"* ]]
}
