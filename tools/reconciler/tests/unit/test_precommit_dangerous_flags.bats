#!/usr/bin/env bats
# tests/unit/test_precommit_dangerous_flags.bats
#
# Issue #147: test coverage for the pre-commit hook's dangerous-flags integration.
#
# The pre-commit hook (hooks/pre-commit) calls scripts/check-dangerous-flags.sh
# on staged agent and fleet YAML files.  These tests set up a temporary git
# repository, stage agent YAMLs containing (or not) --dangerously-skip-permissions,
# and verify the hook exits with the correct status.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

HOOK_SCRIPT="${SCRIPT_DIR}/hooks/pre-commit"
DANGEROUS_FLAGS_SCRIPT="${SCRIPT_DIR}/scripts/check-dangerous-flags.sh"

# Temporary git repo used by the tests — created under SCRIPT_DIR so it stays
# within the worktree and is removed cleanly by teardown.
TMP_REPO=""

setup() {
    # Create a temp directory inside the worktree to avoid safety-net restrictions
    TMP_REPO="${SCRIPT_DIR}/_precommit_test_$$_${RANDOM}"
    mkdir -p "$TMP_REPO"

    # Initialise git repo (without gpg signing to avoid prompts)
    git -C "$TMP_REPO" init -q
    git -C "$TMP_REPO" config user.email "test@example.com"
    git -C "$TMP_REPO" config user.name "Test"
    git -C "$TMP_REPO" config commit.gpgsign false

    # Copy the hook and scripts into the temp repo
    mkdir -p "${TMP_REPO}/scripts"
    mkdir -p "${TMP_REPO}/hooks"
    mkdir -p "${TMP_REPO}/.git/hooks"
    cp "$HOOK_SCRIPT" "${TMP_REPO}/hooks/pre-commit"
    cp "$DANGEROUS_FLAGS_SCRIPT" "${TMP_REPO}/scripts/check-dangerous-flags.sh"
    chmod +x "${TMP_REPO}/hooks/pre-commit"
    chmod +x "${TMP_REPO}/scripts/check-dangerous-flags.sh"

    # Install the hook
    cp "${TMP_REPO}/hooks/pre-commit" "${TMP_REPO}/.git/hooks/pre-commit"
    chmod +x "${TMP_REPO}/.git/hooks/pre-commit"

    # Create the agents directory
    mkdir -p "${TMP_REPO}/agents/hermes"

    # Create an initial commit so the repo is in a valid state
    touch "${TMP_REPO}/.gitkeep"
    git -C "$TMP_REPO" add .gitkeep
    git -C "$TMP_REPO" commit -q --no-verify -m "init"
}

teardown() {
    if [[ -n "$TMP_REPO" && -d "$TMP_REPO" ]]; then
        rm -rf "$TMP_REPO"
    fi
}

# ---------------------------------------------------------------------------
# Helper: stage a file and run the hook directly.
# The hook is run with the mikefarah yq (from pixi) in PATH if available,
# since the hook uses `yq eval` syntax (yq v4).
# ---------------------------------------------------------------------------

_stage_and_run_hook() {
    local file_path="$1"   # absolute path under TMP_REPO
    local rel_path="${file_path#"${TMP_REPO}/"}"

    git -C "$TMP_REPO" add "$rel_path"

    # Prefer pixi yq (v4/mikefarah) over system yq (may be Python-based v3)
    local pixi_yq_dir="${SCRIPT_DIR}/.pixi/envs/lint/bin"
    local run_path="$PATH"
    if [[ -x "${pixi_yq_dir}/yq" ]]; then
        run_path="${pixi_yq_dir}:${PATH}"
    fi

    # Run the pre-commit hook from the repo root
    # SKIP_TESTS=1: TMP_REPO has no Justfile/pixi.toml, so skip the test-suite step
    (cd "$TMP_REPO" && SKIP_TESTS=1 PATH="$run_path" bash ".git/hooks/pre-commit") 2>&1
}

# ---------------------------------------------------------------------------
# Test 1: Bare --dangerously-skip-permissions without suppression → hook rejects
# ---------------------------------------------------------------------------

@test "pre-commit hook: rejects agent YAML with bare --dangerously-skip-permissions" {
    local agent_file="${TMP_REPO}/agents/hermes/danger.yaml"
    cat > "$agent_file" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: danger-agent
  host: hermes
spec:
  label: DangerAgent
  program: claude-code
  workingDirectory: /tmp/danger
  programArgs: "--dangerously-skip-permissions"
  desiredState: active
YAML

    run _stage_and_run_hook "$agent_file"
    [ "$status" -ne 0 ]
    [[ "$output" == *"dangerously-skip-permissions"* || "$output" == *"dangerous"* || "$output" == *"violation"* || "$output" == *"lint"* || "$output" == *"failed"* ]]
}

# ---------------------------------------------------------------------------
# Test 2: --dangerously-skip-permissions with suppression annotation → hook accepts
# ---------------------------------------------------------------------------

@test "pre-commit hook: accepts agent YAML with suppressed --dangerously-skip-permissions" {
    local agent_file="${TMP_REPO}/agents/hermes/suppressed.yaml"
    cat > "$agent_file" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: suppressed-agent
  host: hermes
spec:
  label: SuppressedAgent
  program: claude-code
  workingDirectory: /tmp/suppressed
  programArgs: "--dangerously-skip-permissions" # skip-permissions-lint: ephemeral container with read-only mount and 30m timeout
  desiredState: active
YAML

    run _stage_and_run_hook "$agent_file"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 3: Agent YAML with no dangerous flags → hook accepts
# ---------------------------------------------------------------------------

@test "pre-commit hook: accepts agent YAML without --dangerously-skip-permissions" {
    local agent_file="${TMP_REPO}/agents/hermes/safe.yaml"
    cat > "$agent_file" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: safe-agent
  host: hermes
spec:
  label: SafeAgent
  program: claude-code
  workingDirectory: /tmp/safe
  programArgs: "--model claude-sonnet-4-6"
  desiredState: active
YAML

    run _stage_and_run_hook "$agent_file"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 4: Fleet YAML with bare --dangerously-skip-permissions → hook rejects
# ---------------------------------------------------------------------------

@test "pre-commit hook: rejects fleet YAML with bare --dangerously-skip-permissions" {
    mkdir -p "${TMP_REPO}/fleets"
    local fleet_file="${TMP_REPO}/fleets/danger-fleet.yaml"
    cat > "$fleet_file" <<'YAML'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: danger-fleet
spec:
  agents:
    - name: danger-inline
      program: claude-code
      workingDirectory: /tmp/danger
      programArgs: "--dangerously-skip-permissions"
      desiredState: active
YAML

    run _stage_and_run_hook "$fleet_file"
    [ "$status" -ne 0 ]
    [[ "$output" == *"dangerously-skip-permissions"* || "$output" == *"dangerous"* || "$output" == *"violation"* || "$output" == *"lint"* || "$output" == *"failed"* ]]
}

# ---------------------------------------------------------------------------
# Test 5: Fleet YAML with suppressed --dangerously-skip-permissions → hook accepts
# ---------------------------------------------------------------------------

@test "pre-commit hook: accepts fleet YAML with suppressed --dangerously-skip-permissions" {
    mkdir -p "${TMP_REPO}/fleets"
    local fleet_file="${TMP_REPO}/fleets/suppressed-fleet.yaml"
    cat > "$fleet_file" <<'YAML'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: suppressed-fleet
spec:
  agents:
    - name: suppressed-inline
      program: claude-code
      workingDirectory: /tmp/suppressed
      programArgs: "--dangerously-skip-permissions" # skip-permissions-lint: isolated docker container with no network
      desiredState: active
YAML

    run _stage_and_run_hook "$fleet_file"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 6: Combined flags with bare dangerous flag → hook rejects
# ---------------------------------------------------------------------------

@test "pre-commit hook: rejects when --dangerously-skip-permissions combined with other flags" {
    local agent_file="${TMP_REPO}/agents/hermes/combined.yaml"
    cat > "$agent_file" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: combined-agent
  host: hermes
spec:
  label: CombinedAgent
  program: claude-code
  workingDirectory: /tmp/combined
  programArgs: "--dangerously-skip-permissions --model claude-sonnet-4-6"
  desiredState: active
YAML

    run _stage_and_run_hook "$agent_file"
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# Test 7: check-dangerous-flags.sh directly — bare flag exits 1
# ---------------------------------------------------------------------------

@test "check-dangerous-flags.sh: bare flag in file exits 1" {
    local tmpfile="${TMP_REPO}/direct_test.yaml"
    cat > "$tmpfile" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: direct-test
  host: hermes
spec:
  programArgs: "--dangerously-skip-permissions"
YAML
    run bash "$DANGEROUS_FLAGS_SCRIPT" "$tmpfile"
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# Test 8: check-dangerous-flags.sh directly — suppressed flag exits 0
# ---------------------------------------------------------------------------

@test "check-dangerous-flags.sh: suppressed flag in file exits 0" {
    local tmpfile="${TMP_REPO}/direct_suppressed.yaml"
    cat > "$tmpfile" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: direct-suppressed
  host: hermes
spec:
  programArgs: "--dangerously-skip-permissions" # skip-permissions-lint: test justification
YAML
    run bash "$DANGEROUS_FLAGS_SCRIPT" "$tmpfile"
    [ "$status" -eq 0 ]
}
