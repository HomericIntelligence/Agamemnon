#!/usr/bin/env bats
# tests/unit/test_lint_agents_md.bats
#
# Unit tests for scripts/lint-agents-md.sh
#
# Verifies that the script passes when all six required sections are present,
# fails with a clear error message when any section is missing, and handles
# edge cases (missing file, partial matches, stdin, env var).

bats_require_minimum_version 1.5.0

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
LINT_SCRIPT="${SCRIPT_DIR}/scripts/lint-agents-md.sh"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Write a complete AGENTS.md with all required sections to a temp file.
_write_complete_agents_md() {
    local path="$1"
    cat > "$path" <<'EOF'
# AGENTS.md — Agent Safety Boundaries

## Scope

In scope content here.

## Permitted Actions

Permitted content here.

## Prohibited Actions

Prohibited content here.

## `--dangerously-skip-permissions` Policy

Policy content here.

## Fleet Coordination

Coordination content here.

## Escalation — Human Review Required

Escalation content here.
EOF
}

setup() {
    TMP_DIR="$(mktemp -d)"
}

teardown() {
    rm -rf "$TMP_DIR"
}

# ---------------------------------------------------------------------------
# Test 1: complete AGENTS.md exits 0
# ---------------------------------------------------------------------------

@test "lint-agents-md: complete AGENTS.md exits 0" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS.md"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 2: real AGENTS.md in repo passes
# ---------------------------------------------------------------------------

@test "lint-agents-md: real repo AGENTS.md passes" {
    run bash "$LINT_SCRIPT" "${SCRIPT_DIR}/AGENTS.md"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Tests 3–8: each required section missing → exit 1 with helpful message
# ---------------------------------------------------------------------------

@test "lint-agents-md: missing '## Scope' exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Scope" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-broken.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Scope"* ]]
}

@test "lint-agents-md: missing '## Permitted Actions' exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Permitted Actions" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-broken.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Permitted Actions"* ]]
}

@test "lint-agents-md: missing '## Prohibited Actions' exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Prohibited Actions" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-broken.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Prohibited Actions"* ]]
}

@test "lint-agents-md: missing '--dangerously-skip-permissions Policy' exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "dangerously-skip-permissions" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-broken.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"dangerously-skip-permissions"* ]]
}

@test "lint-agents-md: missing '## Fleet Coordination' exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Fleet Coordination" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-broken.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Fleet Coordination"* ]]
}

@test "lint-agents-md: missing '## Escalation' exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Escalation" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-broken.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Escalation"* ]]
}

# ---------------------------------------------------------------------------
# Test 9: multiple missing sections → reports all of them
# ---------------------------------------------------------------------------

@test "lint-agents-md: multiple missing sections reports count" {
    cat > "${TMP_DIR}/AGENTS-empty.md" <<'EOF'
# AGENTS.md — Agent Safety Boundaries

Some content but no required sections.
EOF
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-empty.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing"* ]]
}

# ---------------------------------------------------------------------------
# Test 10: file not found → exit 1 with clear error
# ---------------------------------------------------------------------------

@test "lint-agents-md: missing file exits 1 with error" {
    run bash "$LINT_SCRIPT" "${TMP_DIR}/does-not-exist.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"file not found"* ]]
}

# ---------------------------------------------------------------------------
# Test 11: partial heading match does not satisfy requirement
# ---------------------------------------------------------------------------

@test "lint-agents-md: partial heading 'Scope' without ## prefix does not satisfy requirement" {
    cat > "${TMP_DIR}/AGENTS-partial.md" <<'EOF'
# AGENTS.md — Agent Safety Boundaries

Scope is mentioned here in prose but not as a heading.

## Permitted Actions

## Prohibited Actions

## `--dangerously-skip-permissions` Policy

## Fleet Coordination

## Escalation — Human Review Required
EOF
    run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS-partial.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Scope"* ]]
}

# ---------------------------------------------------------------------------
# Test 12: no arguments → defaults to repo AGENTS.md and passes
# ---------------------------------------------------------------------------

@test "lint-agents-md: no arguments defaults to repo AGENTS.md" {
    # Run from repo root so the default path resolves correctly
    run bash -c "cd '${SCRIPT_DIR}' && bash scripts/lint-agents-md.sh"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Tests 13–18: stdin input modes
# ---------------------------------------------------------------------------

@test "lint-agents-md: stdin via '-' arg with complete content exits 0" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    run bash -c "bash '$LINT_SCRIPT' - < '${TMP_DIR}/AGENTS.md'"
    [ "$status" -eq 0 ]
}

@test "lint-agents-md: stdin via '-' arg with missing section exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Scope" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    run bash -c "bash '$LINT_SCRIPT' - < '${TMP_DIR}/AGENTS-broken.md'"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Scope"* ]]
    [[ "$output" == *"<stdin>"* ]]
}

# ---------------------------------------------------------------------------
# Tests: LINT_AGENTS_MD_FILE environment variable
# ---------------------------------------------------------------------------

@test "lint-agents-md: LINT_AGENTS_MD_FILE env var with complete file exits 0" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    LINT_AGENTS_MD_FILE="${TMP_DIR}/AGENTS.md" run bash "$LINT_SCRIPT"
    [ "$status" -eq 0 ]
}

@test "lint-agents-md: LINT_AGENTS_MD_FILE env var with missing section exits 1" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Permitted Actions" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    LINT_AGENTS_MD_FILE="${TMP_DIR}/AGENTS-broken.md" run bash "$LINT_SCRIPT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"## Permitted Actions"* ]]
}

@test "lint-agents-md: LINT_AGENTS_MD_FILE='-' reads stdin" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    run bash -c "LINT_AGENTS_MD_FILE=- bash '$LINT_SCRIPT' < '${TMP_DIR}/AGENTS.md'"
    [ "$status" -eq 0 ]
}

@test "lint-agents-md: positional arg takes precedence over LINT_AGENTS_MD_FILE" {
    _write_complete_agents_md "${TMP_DIR}/AGENTS.md"
    grep -v "^## Scope" "${TMP_DIR}/AGENTS.md" > "${TMP_DIR}/AGENTS-broken.md"
    # env var points at broken file, but positional arg should win
    LINT_AGENTS_MD_FILE="${TMP_DIR}/AGENTS-broken.md" run bash "$LINT_SCRIPT" "${TMP_DIR}/AGENTS.md"
    [ "$status" -eq 0 ]
}

@test "lint-agents-md: LINT_AGENTS_MD_FILE pointing to missing file exits 1" {
    LINT_AGENTS_MD_FILE="${TMP_DIR}/does-not-exist.md" run bash "$LINT_SCRIPT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"file not found"* ]]
}
