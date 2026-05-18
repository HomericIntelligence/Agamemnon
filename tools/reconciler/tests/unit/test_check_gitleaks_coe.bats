#!/usr/bin/env bats
# tests/unit/test_check_gitleaks_coe.bats
#
# Issue #567: test coverage for scripts/check-gitleaks-coe.sh.
#
# The script enforces policy: gitleaks security scanning steps must not have
# 'continue-on-error: true', which would silently suppress results. The correct
# approach is using .gitleaks.toml allowlist entries with justification comments.
#
# The script must handle both Unix (LF) and Windows (CRLF) line endings.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
CHECKER="${SCRIPT_DIR}/scripts/check-gitleaks-coe.sh"

TMP_DIR=""
TMP_WORKFLOWS=""

setup() {
    TMP_DIR="${SCRIPT_DIR}/_gitleaks_coe_test_$$_${RANDOM}"
    TMP_WORKFLOWS="${TMP_DIR}/.github/workflows"
    mkdir -p "$TMP_WORKFLOWS"
}

teardown() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

# Helper: run the checker with REPO_ROOT overridden to TMP_DIR
_run_checker() {
    run bash -c "
        REPO_ROOT='${TMP_DIR}' bash '${CHECKER}'
    "
}

# ---------------------------------------------------------------------------
# Test 1: Workflow with gitleaks but no continue-on-error → exit 0
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: workflow with gitleaks, no continue-on-error → exits 0" {
    cat > "${TMP_WORKFLOWS}/test.yml" <<'EOF'
name: Security
on: push
jobs:
  secrets:
    runs-on: ubuntu-latest
    steps:
      - name: Scan for secrets (gitleaks)
        run: gitleaks detect --source . --config .gitleaks.toml
EOF

    _run_checker
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 2: Workflow with gitleaks AND continue-on-error → exit 1 with error
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: workflow with gitleaks and continue-on-error → exits 1" {
    cat > "${TMP_WORKFLOWS}/test.yml" <<'EOF'
name: Security
on: push
jobs:
  secrets:
    runs-on: ubuntu-latest
    steps:
      - name: Scan for secrets (gitleaks)
        run: gitleaks detect --source . --config .gitleaks.toml
      - continue-on-error: true
EOF

    _run_checker
    [ "$status" -eq 1 ]
    [[ "$output" == *"continue-on-error"* ]]
    [[ "$output" == *"gitleaks"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: Multiple workflows; only one has the violation → exit 1, report the bad one
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: multiple workflows, one violates → exits 1, reports violating file" {
    cat > "${TMP_WORKFLOWS}/good.yml" <<'EOF'
name: Good
jobs:
  lint:
    steps:
      - name: Check secrets
        run: gitleaks detect
EOF

    cat > "${TMP_WORKFLOWS}/bad.yml" <<'EOF'
name: Bad
jobs:
  secrets:
    steps:
      - name: Scan for secrets (gitleaks)
        run: gitleaks detect
      - continue-on-error: true
EOF

    _run_checker
    [ "$status" -eq 1 ]
    [[ "$output" == *"bad.yml"* ]]
}

# ---------------------------------------------------------------------------
# Test 4: Workflow without gitleaks, but has continue-on-error → exit 0
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: workflow without gitleaks, has continue-on-error → exits 0" {
    cat > "${TMP_WORKFLOWS}/other.yml" <<'EOF'
name: Other
jobs:
  build:
    steps:
      - name: Some other step
        run: echo hello
      - continue-on-error: true
EOF

    _run_checker
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 5: Handles Windows line endings (CRLF) → still detects violation
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: detects violation with CRLF line endings" {
    # Create a file with CRLF line endings
    printf '%s\r\n' \
        'name: Security' \
        'jobs:' \
        '  secrets:' \
        '    steps:' \
        '      - name: Scan for secrets (gitleaks)' \
        '        run: gitleaks detect' \
        '      - continue-on-error: true' \
        > "${TMP_WORKFLOWS}/crlf.yml"

    _run_checker
    [ "$status" -eq 1 ]
    [[ "$output" == *"continue-on-error"* ]]
}

# ---------------------------------------------------------------------------
# Test 6: Empty workflows directory → exit 0
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: empty workflows directory → exits 0" {
    # TMP_WORKFLOWS is already created but empty
    _run_checker
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 7: Script runs without error on real repo (regression check)
# ---------------------------------------------------------------------------

@test "check-gitleaks-coe: real repo workflows pass" {
    run bash "$CHECKER"
    [ "$status" -eq 0 ]
}
