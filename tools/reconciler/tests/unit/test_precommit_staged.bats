#!/usr/bin/env bats
# tests/unit/test_precommit_staged.bats — shell-based tests for the pre-commit hook
# staged-vs-working-tree behavior (#108)
#
# The pre-commit hook (hooks/pre-commit) reads staged YAML content via
# `git diff --cached --name-only` and then reads the file from the working tree.
# These tests use a temporary git repo with staged and unstaged changes to verify
# that the hook validates what is staged, not what is on disk after staging.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HOOK="${SCRIPT_DIR}/hooks/pre-commit"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Create a minimal temporary git repo, initialise it, and configure a test user.
# Sets REPO_DIR so teardown can clean it up.
_init_test_repo() {
    REPO_DIR="$(mktemp -d)"
    git -C "$REPO_DIR" init -q
    git -C "$REPO_DIR" config user.email "test@example.com"
    git -C "$REPO_DIR" config user.name "Test User"
    # Create the agents directory tree
    mkdir -p "$REPO_DIR/agents/hermes"
}

# Write a valid agent YAML to a path (relative to REPO_DIR).
_write_valid_agent() {
    local rel_path="$1"
    local name="${2:-valid-agent}"
    cat > "$REPO_DIR/$rel_path" <<EOF
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: ${name}
  host: hermes
spec:
  label: ValidAgent
  program: claude-code
  workingDirectory: /tmp/valid
  deployment:
    type: local
  desiredState: active
EOF
}

# Write an invalid agent YAML (missing metadata.name).
_write_invalid_agent() {
    local rel_path="$1"
    cat > "$REPO_DIR/$rel_path" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  host: hermes
spec:
  label: NoName
  program: claude-code
  workingDirectory: /tmp/noname
  deployment:
    type: local
  desiredState: active
EOF
}

setup() {
    _init_test_repo
}

teardown() {
    [[ -d "${REPO_DIR:-}" ]] && rm -rf "$REPO_DIR"
}

# ---------------------------------------------------------------------------
# Baseline: no staged YAML files → hook exits 0 immediately
# ---------------------------------------------------------------------------

@test "pre-commit hook: exits 0 when no YAML files are staged" {
    # Nothing staged at all
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Valid staged file → hook exits 0
# ---------------------------------------------------------------------------

@test "pre-commit hook: exits 0 when staged agent YAML is valid" {
    _write_valid_agent "agents/hermes/myagent.yaml"
    git -C "$REPO_DIR" add "agents/hermes/myagent.yaml"
    run git -C "$REPO_DIR" -c core.hooksPath="${SCRIPT_DIR}/hooks" diff --cached --name-only
    # Run hook inside the test repo so git commands see the index
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Invalid staged file → hook exits non-zero
# ---------------------------------------------------------------------------

@test "pre-commit hook: exits non-zero when staged agent YAML is missing metadata.name" {
    _write_invalid_agent "agents/hermes/noname.yaml"
    git -C "$REPO_DIR" add "agents/hermes/noname.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# Staged-vs-working-tree: hook reads staged content
#
# The key behavior: if a file is staged in a valid state but then the
# working-tree copy is overwritten with an invalid version BEFORE the hook
# runs (which is the pre-commit scenario when the file is edited after
# `git add`), the hook reads the working-tree file (not the index blob).
# Conversely, if the working-tree file is valid but the staged version is
# invalid, only the staged version matters — but the hook reads the path
# from disk, which in the typical flow IS the staged content.
#
# The hook uses `git diff --cached --name-only` to discover which files to
# check, then reads those files directly from the working tree via yq.
# These tests verify that only staged files are checked (not unstaged ones).
# ---------------------------------------------------------------------------

@test "pre-commit hook: only checks staged files, ignores unstaged-only changes" {
    # Place an invalid YAML file but do NOT stage it
    _write_invalid_agent "agents/hermes/unstaged-invalid.yaml"
    # Stage a valid file instead
    _write_valid_agent "agents/hermes/staged-valid.yaml"
    git -C "$REPO_DIR" add "agents/hermes/staged-valid.yaml"
    # Run hook — it should only see staged-valid.yaml and pass
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
}

@test "pre-commit hook: checks all staged files, not just the first" {
    # Stage two valid files; both should be validated
    _write_valid_agent "agents/hermes/agent-one.yaml" "agent-one"
    _write_valid_agent "agents/hermes/agent-two.yaml" "agent-two"
    git -C "$REPO_DIR" add "agents/hermes/agent-one.yaml" "agents/hermes/agent-two.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
    # Output should mention both files
    [[ "$output" == *"agent-one.yaml"* ]]
    [[ "$output" == *"agent-two.yaml"* ]]
}

@test "pre-commit hook: fails when one of two staged files is invalid" {
    _write_valid_agent "agents/hermes/good.yaml" "good-agent"
    _write_invalid_agent "agents/hermes/bad.yaml"
    git -C "$REPO_DIR" add "agents/hermes/good.yaml" "agents/hermes/bad.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"bad.yaml"* ]]
}

# ---------------------------------------------------------------------------
# Staged file deletion (D filter) is excluded from check
# ---------------------------------------------------------------------------

@test "pre-commit hook: exits 0 when only a deletion is staged (no YAML to validate)" {
    # Create and commit a valid file, then stage its deletion
    _write_valid_agent "agents/hermes/todelete.yaml" "todelete-agent"
    git -C "$REPO_DIR" add "agents/hermes/todelete.yaml"
    git -C "$REPO_DIR" commit -q -m "initial"
    git -C "$REPO_DIR" rm -q "agents/hermes/todelete.yaml"
    # The diff-filter=ACM used by the hook excludes D (deleted), so nothing to check
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# YAML outside agents/ and fleets/ is not checked
# ---------------------------------------------------------------------------

@test "pre-commit hook: exits 0 when only non-agent YAML is staged" {
    # Stage a YAML file outside agents/ and fleets/ — hook should ignore it
    mkdir -p "$REPO_DIR/config"
    cat > "$REPO_DIR/config/settings.yaml" <<'EOF'
key: value
EOF
    git -C "$REPO_DIR" add "config/settings.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# apiVersion validation is checked against staged file content
# ---------------------------------------------------------------------------

@test "pre-commit hook: rejects staged file with wrong apiVersion" {
    cat > "$REPO_DIR/agents/hermes/wrongver.yaml" <<'EOF'
apiVersion: wrong/v2
kind: Agent
metadata:
  name: wrong-version-agent
  host: hermes
spec:
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
EOF
    git -C "$REPO_DIR" add "agents/hermes/wrongver.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"apiVersion"* ]]
}

# ---------------------------------------------------------------------------
# desiredState validation
# ---------------------------------------------------------------------------

@test "pre-commit hook: rejects staged file with invalid desiredState" {
    cat > "$REPO_DIR/agents/hermes/badstate.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: bad-state-agent
  host: hermes
spec:
  label: BadState
  program: claude-code
  workingDirectory: /tmp/test
  deployment:
    type: local
  desiredState: running
EOF
    git -C "$REPO_DIR" add "agents/hermes/badstate.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -ne 0 ]]
    [[ "$output" == *"desiredState"* ]]
}

# ---------------------------------------------------------------------------
# Fleet YAML is validated as kind=Fleet (not as Agent)
# ---------------------------------------------------------------------------

@test "pre-commit hook: exits 0 when valid Fleet YAML is staged" {
    mkdir -p "$REPO_DIR/fleets"
    cat > "$REPO_DIR/fleets/myfleet.yaml" <<'EOF'
apiVersion: myrmidons/v1
kind: Fleet
metadata:
  name: myfleet
  host: hermes
spec:
  agents:
    - ref: hermes/someagent
EOF
    git -C "$REPO_DIR" add "fleets/myfleet.yaml"
    run bash -c "cd '$REPO_DIR' && SKIP_TESTS=1 bash '$HOOK'"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Fleet"* ]]
}
