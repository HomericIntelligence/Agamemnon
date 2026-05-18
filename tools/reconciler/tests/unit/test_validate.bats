#!/usr/bin/env bats
# tests/unit/test_validate.bats — unit tests for tests/validate-schemas.sh exit conditions

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

setup() {
    # Create a temporary directory for each test
    TEST_TMPDIR="$(mktemp -d)"
    export TEST_TMPDIR

    # Create agents and fleets subdirectories
    mkdir -p "$TEST_TMPDIR/agents"
    mkdir -p "$TEST_TMPDIR/fleets"
}

teardown() {
    # Clean up temporary directory
    [[ -d "$TEST_TMPDIR" ]] && rm -rf "$TEST_TMPDIR"
}

# Helper function to run validate script in temp directory.
# NOTE: must be exported so BATS's `run` subshell can resolve it.
run_validate() {
    cd "$TEST_TMPDIR" || return 1
    # The script uses REPO_ROOT derived from its own path, so we need to run it
    # in a way that makes it think the temp dir is the root.
    # Copy both validate-schemas.sh and validate-fleet-refs.sh so the
    # in-script call to "${SCRIPT_DIR}/validate-fleet-refs.sh" resolves.
    mkdir -p "$TEST_TMPDIR/tests"
    cp "$SCRIPT_DIR/tests/validate-schemas.sh" "$TEST_TMPDIR/tests/"
    cp "$SCRIPT_DIR/tests/validate-fleet-refs.sh" "$TEST_TMPDIR/tests/"
    bash "$TEST_TMPDIR/tests/validate-schemas.sh"
}
export -f run_validate

# ---------------------------------------------------------------------------
# Test 1: Valid agent YAML exits 0
# ---------------------------------------------------------------------------
@test "valid agent YAML exits 0" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/validagent.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: valid-agent
  host: hermes
spec:
  label: ValidAgent
  program: claude-code
  workingDirectory: /tmp/valid
  deployment:
    type: local
  desiredState: active
EOF
    run run_validate
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 2: Missing required field (metadata.name) exits non-zero
# ---------------------------------------------------------------------------
@test "missing metadata.name exits non-zero" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/missingname.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  host: hermes
spec:
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"metadata.name is required"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: Wrong apiVersion exits non-zero
# ---------------------------------------------------------------------------
@test "wrong apiVersion exits non-zero" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/wrongversion.yaml" <<'EOF'
apiVersion: wrong/v1
kind: Agent
metadata:
  name: test-agent
  host: hermes
spec:
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"expected apiVersion=myrmidons/v1"* ]]
}

# ---------------------------------------------------------------------------
# Test 4: Invalid desiredState exits non-zero
# ---------------------------------------------------------------------------
@test "invalid desiredState exits non-zero" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/invalidstate.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: test-agent
  host: hermes
spec:
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
  desiredState: running
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"spec.desiredState must be 'active' or 'hibernated'"* ]]
}

# ---------------------------------------------------------------------------
# Test 5: No YAML files found exits 0
# ---------------------------------------------------------------------------
@test "no YAML files found exits 0" {
    # Empty agents and fleets directories
    run run_validate
    [ "$status" -eq 0 ]
    [[ "$output" == *"Checked: 0 files"* ]]
}

# ---------------------------------------------------------------------------
# Test 6: Relative workingDirectory exits non-zero
# ---------------------------------------------------------------------------
@test "relative workingDirectory exits non-zero" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/relativedir.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: relative-agent
  host: hermes
spec:
  program: claude-code
  workingDirectory: ./relative/path
  deployment:
    type: local
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"is not an absolute path"* ]]
}

# ---------------------------------------------------------------------------
# Test 7: Valid fleet YAML exits 0
# ---------------------------------------------------------------------------
@test "valid fleet YAML exits 0" {
    mkdir -p "$TEST_TMPDIR/fleets"
    cat > "$TEST_TMPDIR/fleets/validfleet.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: validfleet
spec:
  replicas: 2
  selector:
    matchLabels:
      environment: test
EOF
    run run_validate
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 8: Fleet with mismatched filename exits non-zero
# ---------------------------------------------------------------------------
@test "fleet with mismatched filename exits non-zero" {
    mkdir -p "$TEST_TMPDIR/fleets"
    cat > "$TEST_TMPDIR/fleets/wrongfilename.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: differentname
spec:
  replicas: 2
  selector:
    matchLabels:
      environment: test
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"filename"* && "$output" == *"does not match metadata.name"* ]]
}

# ---------------------------------------------------------------------------
# Test 9: metadata.host matches directory (issue #150)
# ---------------------------------------------------------------------------
@test "metadata.host mismatching directory exits non-zero" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/wronghost.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: wrong-host-agent
  host: apollo
spec:
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
  desiredState: active
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"does not match directory"* ]]
}

@test "metadata.host matching directory exits 0" {
    mkdir -p "$TEST_TMPDIR/agents/hermes"
    cat > "$TEST_TMPDIR/agents/hermes/correcthost.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: correct-host-agent
  host: hermes
spec:
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
  desiredState: active
EOF
    run run_validate
    [ "$status" -eq 0 ]
    [[ "$output" != *"does not match directory"* ]]
}

# ---------------------------------------------------------------------------
# Test 11: .myrmidons.yaml validation (issue #235)
# ---------------------------------------------------------------------------
@test ".myrmidons.yaml with valid fields passes validation" {
    cat > "$TEST_TMPDIR/.myrmidons.yaml" <<'EOF'
defaultHost: hermes
aimHost: http://localhost:8080
logLevel: info
prunePolicy: manual
snapshotRetention: 7
EOF
    run run_validate
    [ "$status" -eq 0 ]
    [[ "$output" == *".myrmidons.yaml: ok"* ]]
}

@test ".myrmidons.yaml with invalid logLevel fails validation" {
    cat > "$TEST_TMPDIR/.myrmidons.yaml" <<'EOF'
logLevel: verbose
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"logLevel must be one of:"* ]]
}

@test ".myrmidons.yaml with invalid prunePolicy fails validation" {
    cat > "$TEST_TMPDIR/.myrmidons.yaml" <<'EOF'
prunePolicy: always
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"prunePolicy must be one of:"* ]]
}

@test ".myrmidons.yaml with invalid aimHost (no scheme) fails validation" {
    cat > "$TEST_TMPDIR/.myrmidons.yaml" <<'EOF'
aimHost: localhost:8080
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"aimHost must start with http://"* ]]
}

@test ".myrmidons.yaml with invalid snapshotRetention (non-integer) fails validation" {
    cat > "$TEST_TMPDIR/.myrmidons.yaml" <<'EOF'
snapshotRetention: two-weeks
EOF
    run run_validate
    [ "$status" -ne 0 ]
    [[ "$output" == *"snapshotRetention must be a non-negative integer"* ]]
}

@test "absent .myrmidons.yaml does not cause validation failure" {
    # No .myrmidons.yaml in TEST_TMPDIR
    run run_validate
    [ "$status" -eq 0 ]
    [[ "$output" != *".myrmidons.yaml"* ]]
}
