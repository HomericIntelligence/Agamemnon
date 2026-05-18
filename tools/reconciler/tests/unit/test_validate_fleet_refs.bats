#!/usr/bin/env bats
# tests/unit/test_validate_fleet_refs.bats — unit tests for tests/validate-fleet-refs.sh
#
# The script derives REPO_ROOT from its own path (BASH_SOURCE[0]).
# Each test copies validate-fleet-refs.sh into a temp directory structure
# under tests/ so that REPO_ROOT resolves to the temp root, giving full
# control over the agents/ and fleets/ layout without touching real files.
#
# Issue #444: Add fleet ref validation to pre-commit / CI lint step

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
VALIDATE_SCRIPT="${SCRIPT_DIR}/tests/validate-fleet-refs.sh"

# Temporary root created per test
TMP_ROOT=""

setup() {
    TMP_ROOT="${SCRIPT_DIR}/_fleet_refs_test_$$_${RANDOM}"
    mkdir -p "${TMP_ROOT}/tests"
    mkdir -p "${TMP_ROOT}/agents/hermes"
    mkdir -p "${TMP_ROOT}/fleets"

    # Copy the script so REPO_ROOT resolves to TMP_ROOT
    cp "$VALIDATE_SCRIPT" "${TMP_ROOT}/tests/validate-fleet-refs.sh"
    chmod +x "${TMP_ROOT}/tests/validate-fleet-refs.sh"

    # Create a real agent file that fleet refs can point at
    cat > "${TMP_ROOT}/agents/hermes/real-agent.yaml" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: real-agent
  host: hermes
spec:
  label: RealAgent
  program: claude-code
  workingDirectory: /tmp/real
  deployment:
    type: local
  desiredState: active
YAML
}

teardown() {
    if [[ -n "$TMP_ROOT" && -d "$TMP_ROOT" ]]; then
        rm -rf "$TMP_ROOT"
    fi
}

# ---------------------------------------------------------------------------
# Helper: run the copied script
# ---------------------------------------------------------------------------
_run_validate() {
    bash "${TMP_ROOT}/tests/validate-fleet-refs.sh"
}

# ---------------------------------------------------------------------------
# Test 1: fleet with all valid refs exits 0
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 0 for fleet with all valid refs" {
    cat > "${TMP_ROOT}/fleets/valid.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: valid-fleet
spec:
  agents:
    - ref: hermes/real-agent
YAML

    run _run_validate
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 2: fleet with a dangling ref exits 1
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 1 for fleet with dangling ref" {
    cat > "${TMP_ROOT}/fleets/dangling.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: dangling-fleet
spec:
  agents:
    - ref: hermes/nonexistent-agent-xyz
YAML

    run _run_validate
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# Test 3: dangling ref output includes FAIL and the ref path
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: FAIL message includes the ref path for dangling ref" {
    cat > "${TMP_ROOT}/fleets/dangling.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: dangling-fleet
spec:
  agents:
    - ref: hermes/ghost-agent
YAML

    run _run_validate
    [ "$status" -eq 1 ]
    [[ "$output" == *"hermes/ghost-agent"* ]]
    [[ "$output" == *"FAIL"* ]]
}

# ---------------------------------------------------------------------------
# Test 4: inline agent with all required fields exits 0
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 0 for fleet with valid inline agent" {
    cat > "${TMP_ROOT}/fleets/inline-valid.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: inline-valid-fleet
spec:
  agents:
    - name: my-inline-agent
      program: claude-code
      workingDirectory: /tmp/inline
YAML

    run _run_validate
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 5: inline agent missing name exits 1
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 1 for inline agent missing name" {
    cat > "${TMP_ROOT}/fleets/inline-no-name.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: inline-no-name-fleet
spec:
  agents:
    - program: claude-code
      workingDirectory: /tmp/inline
YAML

    run _run_validate
    [ "$status" -eq 1 ]
    [[ "$output" == *"name is required"* ]]
}

# ---------------------------------------------------------------------------
# Test 6: inline agent missing program exits 1
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 1 for inline agent missing program" {
    cat > "${TMP_ROOT}/fleets/inline-no-program.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: inline-no-program-fleet
spec:
  agents:
    - name: my-inline-agent
      workingDirectory: /tmp/inline
YAML

    run _run_validate
    [ "$status" -eq 1 ]
    [[ "$output" == *"program is required"* ]]
}

# ---------------------------------------------------------------------------
# Test 7: inline agent missing workingDirectory exits 1
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 1 for inline agent missing workingDirectory" {
    cat > "${TMP_ROOT}/fleets/inline-no-workdir.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: inline-no-workdir-fleet
spec:
  agents:
    - name: my-inline-agent
      program: claude-code
YAML

    run _run_validate
    [ "$status" -eq 1 ]
    [[ "$output" == *"workingDirectory is required"* ]]
}

# ---------------------------------------------------------------------------
# Test 8: fleet with no agents skips cleanly and exits 0
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 0 for fleet with no agents" {
    cat > "${TMP_ROOT}/fleets/empty.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: empty-fleet
spec:
  agents: []
YAML

    run _run_validate
    [ "$status" -eq 0 ]
    [[ "$output" == *"no agents"* ]]
}

# ---------------------------------------------------------------------------
# Test 9: non-Fleet YAML files are skipped
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: ignores YAML files with kind != Fleet" {
    cat > "${TMP_ROOT}/fleets/not-a-fleet.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: some-agent
spec:
  program: claude-code
YAML

    run _run_validate
    [ "$status" -eq 0 ]
    # The agent-kind file should not appear in the counted summary
    [[ "$output" == *"Checked: 0 refs, 0 inline agents"* ]]
}

# ---------------------------------------------------------------------------
# Test 10: mixed fleet — one valid ref + one dangling ref exits 1
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 1 when one of multiple refs is dangling" {
    cat > "${TMP_ROOT}/fleets/mixed.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: mixed-fleet
spec:
  agents:
    - ref: hermes/real-agent
    - ref: hermes/missing-agent
YAML

    run _run_validate
    [ "$status" -eq 1 ]
    [[ "$output" == *"hermes/real-agent"* ]]
    [[ "$output" == *"hermes/missing-agent"* ]]
    [[ "$output" == *"FAIL"* ]]
}

# ---------------------------------------------------------------------------
# Test 11: summary line includes correct counts
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: summary line shows correct ref and inline counts" {
    cat > "${TMP_ROOT}/fleets/counts.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: counts-fleet
spec:
  agents:
    - ref: hermes/real-agent
    - name: my-inline
      program: none
      workingDirectory: /tmp/inline
YAML

    run _run_validate
    [ "$status" -eq 0 ]
    [[ "$output" == *"Checked: 1 refs, 1 inline agents"* ]]
}

# ---------------------------------------------------------------------------
# Test 12: empty fleets directory exits 0 with zero errors
# ---------------------------------------------------------------------------

@test "validate-fleet-refs: exits 0 when fleets directory is empty" {
    # TMP_ROOT/fleets/ exists but has no YAML files
    run _run_validate
    [ "$status" -eq 0 ]
    [[ "$output" == *"Checked: 0 refs, 0 inline agents, Errors: 0"* ]]
}
