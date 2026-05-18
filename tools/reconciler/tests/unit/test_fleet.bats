#!/usr/bin/env bats
bats_require_minimum_version 1.5.0
# tests/unit/test_fleet.bats — unit tests for fleet resolution (scripts/lib/reconcile.sh)
#
# Tests cover:
# - find_fleet_file() for locating fleet YAML files
# - resolve_fleet_files() for resolving refs and creating inline agent temp files
# - cleanup_fleet_tmpdir() for temporary file cleanup
#
# Issue #208: Add bats tests for resolve_fleet() and fleet resolution logic

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
REPO_ROOT="${SCRIPT_DIR}"

# Source reconcile.sh before each test
setup() {
    export AGAMEMNON_URL="http://localhost:19999"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"
}

# Teardown: clean up any FLEET_TMPDIR created during tests
teardown() {
    cleanup_fleet_tmpdir
}

# ---------------------------------------------------------------------------
# find_fleet_file
# ---------------------------------------------------------------------------

@test "find_fleet_file: locates production fleet by name" {
    result="$(find_fleet_file "production")"
    [[ -f "$result" ]]
    [[ "$result" == *"production.yaml" ]]
}

@test "find_fleet_file: locates dev-mesh fleet by name" {
    result="$(find_fleet_file "dev-mesh")"
    [[ -f "$result" ]]
    [[ "$result" == *"dev-mesh.yaml" ]]
}

@test "find_fleet_file: exits 1 for nonexistent fleet" {
    ! find_fleet_file "nonexistent-fleet-xyz" 2>/dev/null
}

@test "find_fleet_file: outputs error message for missing fleet" {
    result="$(find_fleet_file "nonexistent-fleet-xyz" 2>&1 || true)"
    [[ "$result" == *"not found"* ]]
}

# ---------------------------------------------------------------------------
# resolve_fleet_files: ref resolution
# ---------------------------------------------------------------------------

@test "resolve_fleet_files: resolves production fleet refs to correct file paths" {
    fleet_file="${REPO_ROOT}/fleets/production.yaml"
    result="$(resolve_fleet_files "$fleet_file")"

    # Should output 5 lines (5 refs in production.yaml)
    line_count="$(echo "$result" | wc -l | tr -d ' ')"
    [[ "$line_count" == "5" ]]

    # Each output path should exist
    while IFS= read -r f; do
        [[ -f "$f" ]]
    done <<< "$result"
}

@test "resolve_fleet_files: resolves ref paths to agents/<host>/<name>.yaml format" {
    fleet_file="${REPO_ROOT}/fleets/production.yaml"
    result="$(resolve_fleet_files "$fleet_file")"

    # Check that resolved paths contain the expected agent names and agents directory
    echo "$result" | grep -q "agents/hermes/aindrea.yaml"
    echo "$result" | grep -q "agents/hermes/raiden.yaml"
    echo "$result" | grep -q "agents/hermes/baird.yaml"
    echo "$result" | grep -q "agents/hermes/vegai.yaml"
    echo "$result" | grep -q "agents/hermes/julia.yaml"
}

@test "resolve_fleet_files: bad ref path exits 1 with error" {
    # Create a temp fleet with a bad ref
    local tmp_fleet
    tmp_fleet="$(mktemp)"
    cat > "$tmp_fleet" <<'EOF'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: bad-refs
  host: hermes
spec:
  agents:
    - ref: hermes/nonexistent-agent-xyz
EOF

    result="$(resolve_fleet_files "$tmp_fleet" 2>&1 || true)"
    run ! resolve_fleet_files "$tmp_fleet" 2>/dev/null
    [[ "$result" == *"not found"* ]]
    rm -f "$tmp_fleet"
}

@test "resolve_fleet_files: missing fleet file produces error" {
    result=$(resolve_fleet_files "/nonexistent/fleet.yaml" 2>&1 || true)
    # Should produce an error message about the file not being found
    [[ "$result" == *"no such file"* ]] || [[ "$result" == *"Error"* ]]
}

# ---------------------------------------------------------------------------
# resolve_fleet_files: inline agent handling
# ---------------------------------------------------------------------------

@test "resolve_fleet_files: creates FLEET_TMPDIR and temp files for inline agents" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"

    result="$(resolve_fleet_files "$fleet_file")"

    # dev-mesh has 3 refs + 1 inline agent = 4 lines
    line_count="$(echo "$result" | wc -l | tr -d ' ')"
    [[ "$line_count" == "4" ]]

    # Find the inline temp file in the result
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"
    [[ -f "$ci_worker_file" ]]

    # Verify temp dir exists
    ci_worker_tmpdir="$(dirname "$ci_worker_file")"
    [[ -d "$ci_worker_tmpdir" ]]
}

@test "resolve_fleet_files: inline agent written to FLEET_TMPDIR with correct name" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"

    # Find the ci-worker temp file
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"
    [[ -f "$ci_worker_file" ]]
    [[ "$ci_worker_file" == *"ci-worker.yaml" ]]
}

@test "resolve_fleet_files: inline agent has kind Agent" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    kind="$(yq eval '.kind' "$ci_worker_file" 2>/dev/null)"
    [[ "$kind" == "Agent" ]]
}

@test "resolve_fleet_files: inline agent has correct metadata.name" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    name="$(yq eval '.metadata.name' "$ci_worker_file" 2>/dev/null)"
    [[ "$name" == "ci-worker" ]]
}

@test "resolve_fleet_files: inline agent inherits fleet host in metadata" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    host="$(yq eval '.metadata.host' "$ci_worker_file" 2>/dev/null)"
    [[ "$host" == "hermes" ]]
}

@test "resolve_fleet_files: inline agent has correct spec.program" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    program="$(yq eval '.spec.program' "$ci_worker_file" 2>/dev/null)"
    [[ "$program" == "none" ]]
}

@test "resolve_fleet_files: inline agent has correct spec.workingDirectory" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    workdir="$(yq eval '.spec.workingDirectory' "$ci_worker_file" 2>/dev/null)"
    [[ "$workdir" == "/tmp/ci" ]]
}

@test "resolve_fleet_files: inline agent has correct spec.taskDescription" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    taskdesc="$(yq eval '.spec.taskDescription' "$ci_worker_file" 2>/dev/null)"
    [[ "$taskdesc" == "CI shell worker for local dev tasks" ]]
}

@test "resolve_fleet_files: inline agent has correct spec.deployment.type" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    deploy_type="$(yq eval '.spec.deployment.type' "$ci_worker_file" 2>/dev/null)"
    [[ "$deploy_type" == "docker" ]]
}

@test "resolve_fleet_files: inline agent preserves docker image config" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    image="$(yq eval '.spec.deployment.docker.image' "$ci_worker_file" 2>/dev/null)"
    [[ "$image" == "achaean-worker:latest" ]]
}

@test "resolve_fleet_files: inline agent has correct spec.desiredState" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    result="$(resolve_fleet_files "$fleet_file")"
    ci_worker_file="$(echo "$result" | grep "ci-worker.yaml")"

    state="$(yq eval '.spec.desiredState' "$ci_worker_file" 2>/dev/null)"
    [[ "$state" == "active" ]]
}

# ---------------------------------------------------------------------------
# cleanup_fleet_tmpdir
# ---------------------------------------------------------------------------

@test "cleanup_fleet_tmpdir: removes FLEET_TMPDIR when set" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"
    unset FLEET_TMPDIR

    # Resolve to create temp dir
    resolve_fleet_files "$fleet_file" > /dev/null

    # FLEET_TMPDIR should be set
    [[ -n "${FLEET_TMPDIR:-}" ]]
    tmp_dir="${FLEET_TMPDIR}"
    [[ -d "$tmp_dir" ]]

    # Clean up
    cleanup_fleet_tmpdir

    # Directory should be gone
    [[ ! -d "$tmp_dir" ]]
    [[ -z "${FLEET_TMPDIR:-}" ]]
}

@test "cleanup_fleet_tmpdir: does nothing when FLEET_TMPDIR is unset" {
    unset FLEET_TMPDIR
    # Should not error
    cleanup_fleet_tmpdir
}

# ---------------------------------------------------------------------------
# Integration: inline agent without name errors
# ---------------------------------------------------------------------------

@test "resolve_fleet_files: inline agent without name exits 1 with error" {
    local tmp_fleet
    tmp_fleet="$(mktemp)"
    cat > "$tmp_fleet" <<'EOF'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: bad-inline
  host: hermes
spec:
  agents:
    - program: claude-code
      workingDirectory: /tmp/test
EOF

    unset FLEET_TMPDIR
    result="$(resolve_fleet_files "$tmp_fleet" 2>&1 || true)"
    run ! resolve_fleet_files "$tmp_fleet" 2>/dev/null
    [[ "$result" == *"no name"* ]]
    rm -f "$tmp_fleet"
}

# ---------------------------------------------------------------------------
# Integration: mixed refs and inline agents
# ---------------------------------------------------------------------------

@test "resolve_fleet_files: dev-mesh resolves both refs and inline agents correctly" {
    fleet_file="${REPO_ROOT}/fleets/dev-mesh.yaml"

    result="$(resolve_fleet_files "$fleet_file")"

    # Should have 4 entries (3 refs + 1 inline)
    line_count="$(echo "$result" | wc -l | tr -d ' ')"
    [[ "$line_count" == "4" ]]

    # Should contain both ref-resolved files and inline temp files
    echo "$result" | grep -q "agents/hermes/aindrea.yaml"
    echo "$result" | grep -q "agents/hermes/baird.yaml"
    echo "$result" | grep -q "agents/hermes/vegai.yaml"
    echo "$result" | grep -q "ci-worker.yaml"

    # All files should exist
    while IFS= read -r f; do
        [[ -f "$f" ]]
    done <<< "$result"
}
