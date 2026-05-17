#!/usr/bin/env bats
# tests/integration/test_diff.bats
#
# Shell-level tests for scripts/diff.sh against a mock Agamemnon server.
# Verifies output format, exit codes, and correct drift detection.
#
# Covers issue #199.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/tests/fixtures"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"
DIFF_SH="${SCRIPT_DIR}/scripts/diff.sh"
MOCK_PORT=18084
MOCK_PID_FILE="/tmp/bats-diff-mock-$$.pid"

# ---------------------------------------------------------------------------
# Mock server helpers
# ---------------------------------------------------------------------------

_start_mock_server() {
    local http_status="${1:-200}"
    local body="${2}"
    [[ -z "$body" ]] && body='[]'

    MOCK_STATUS="$http_status" MOCK_BODY="$body" \
        python3 "${HELPERS_DIR}/mock_server.py" "$MOCK_PORT" \
        > /dev/null 2>&1 &
    echo $! > "$MOCK_PID_FILE"
    sleep 0.3
}

_stop_mock_server() {
    if [[ -f "$MOCK_PID_FILE" ]]; then
        kill "$(cat "$MOCK_PID_FILE")" 2>/dev/null || true
        rm -f "$MOCK_PID_FILE"
    fi
}

# ---------------------------------------------------------------------------
# Setup: create a unique temp host directory under the real agents/ tree
# so diff.sh's BASH_SOURCE-based repo root discovery finds the YAML files.
# ---------------------------------------------------------------------------

_DIFF_TEST_HOST=""

_setup_test_agent() {
    local agent_yaml="$1"
    _DIFF_TEST_HOST="difftest-$$-${RANDOM}"
    mkdir -p "${SCRIPT_DIR}/agents/${_DIFF_TEST_HOST}"
    cp "$agent_yaml" "${SCRIPT_DIR}/agents/${_DIFF_TEST_HOST}/"
}

_cleanup_test_agent() {
    if [[ -n "${_DIFF_TEST_HOST:-}" ]]; then
        rm -rf "${SCRIPT_DIR}/agents/${_DIFF_TEST_HOST}"
        _DIFF_TEST_HOST=""
    fi
}

setup() {
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
    # Disable ANSI colors so output comparisons work reliably
    export NO_COLOR=1
}

teardown() {
    _stop_mock_server
    _cleanup_test_agent
    unset NO_COLOR
}

# ---------------------------------------------------------------------------
# Helper: place a YAML fixture in a temp host dir and run diff.sh
# ---------------------------------------------------------------------------

_run_diff() {
    local agent_yaml="$1"
    shift  # remaining args forwarded to diff.sh

    _setup_test_agent "$agent_yaml"
    run bash "$DIFF_SH" "$_DIFF_TEST_HOST" "$@"
    _cleanup_test_agent
}

# ---------------------------------------------------------------------------
# Exit codes
# ---------------------------------------------------------------------------

@test "diff.sh: exits 0 when agent matches Agamemnon exactly (no drift)" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 0 ]]
}

@test "diff.sh: exits 1 when agent is absent from Agamemnon (CREATE drift)" {
    _start_mock_server 200 '[]'

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 1 ]]
}

@test "diff.sh: exits 1 when label has drifted" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Old Label",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 1 ]]
}

@test "diff.sh: exits 1 when agent needs WAKE (offline but desired active)" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "offline",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 1 ]]
}

@test "diff.sh: exits 1 when agent needs HIBERNATE (active but desired hibernated)" {
    local mock_body
    mock_body='[{
        "id": "abc-456",
        "name": "sleeping-agent",
        "status": "active",
        "label": "Sleeping Agent",
        "program": "aider",
        "workingDirectory": "/home/mvillmow/SleepProject",
        "programArgs": "",
        "taskDescription": "A hibernated agent",
        "tags": ["hibernated"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-hibernated.yaml"

    [[ "$status" -eq 1 ]]
}

# ---------------------------------------------------------------------------
# Output format: no-drift case
# ---------------------------------------------------------------------------

@test "diff.sh: prints 'No drift detected' message when no drift" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$output" == *"No drift detected"* ]]
}

# ---------------------------------------------------------------------------
# Output format: drift cases
# ---------------------------------------------------------------------------

@test "diff.sh: includes agent name in output when CREATE drift detected" {
    _start_mock_server 200 '[]'

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$output" == *"test-agent"* ]]
}

@test "diff.sh: shows [+] marker when agent would be created" {
    _start_mock_server 200 '[]'

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$output" == *"[+]"* ]]
}

@test "diff.sh: shows [!] marker when agent needs WAKE" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "offline",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$output" == *"[!]"* ]]
}

@test "diff.sh: shows [z] marker when agent needs HIBERNATE" {
    local mock_body
    mock_body='[{
        "id": "abc-456",
        "name": "sleeping-agent",
        "status": "active",
        "label": "Sleeping Agent",
        "program": "aider",
        "workingDirectory": "/home/mvillmow/SleepProject",
        "programArgs": "",
        "taskDescription": "A hibernated agent",
        "tags": ["hibernated"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-hibernated.yaml"

    [[ "$output" == *"[z]"* ]]
}

@test "diff.sh: shows [~] marker when scalar field has drifted" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Stale Label",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$output" == *"[~]"* ]]
}

@test "diff.sh: shows drifted field name in output for label drift" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Stale Label",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$output" == *"label"* ]]
}

@test "diff.sh: shows old and new values in output for label drift" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Stale Label",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    # Should show old value and new value (arrow format: old → new)
    [[ "$output" == *"Stale Label"* ]]
    [[ "$output" == *"Test Agent"* ]]
}

@test "diff.sh: shows tag changes when tags have drifted" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["old-tag"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 1 ]]
    [[ "$output" == *"tags"* ]]
}

# ---------------------------------------------------------------------------
# Argument parsing: --agent filter
# ---------------------------------------------------------------------------

@test "diff.sh: --agent filter limits output to named agent" {
    # Two agents in Agamemnon; only the matching one should appear
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "offline",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml" --agent test-agent

    [[ "$output" == *"test-agent"* ]]
}

@test "diff.sh: --agent filter with non-matching name produces no drift output" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "offline",
        "label": "Test Agent",
        "program": "claude-code",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml" --agent nonexistent-agent

    # No drift because the filter excludes the drifting agent
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# API connection handling
# ---------------------------------------------------------------------------

@test "diff.sh: fails gracefully when Agamemnon is unreachable" {
    # Don't start mock server → connection refused
    export AGAMEMNON_URL="http://127.0.0.1:19999"

    _setup_test_agent "${FIXTURES_DIR}/agent-valid.yaml"
    run bash "$DIFF_SH" "$_DIFF_TEST_HOST"
    _cleanup_test_agent

    [[ "$status" -ne 0 ]]
}

# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------

@test "diff.sh: minimal agent YAML produces valid diff output" {
    _start_mock_server 200 '[]'

    _run_diff "${FIXTURES_DIR}/agent-minimal.yaml"

    # agent-minimal.yaml has name=minimal-agent; should show as [+] CREATE
    [[ "$output" == *"[+]"* ]]
    [[ "$output" == *"minimal-agent"* ]]
}

@test "diff.sh: program drift is detected and reported" {
    local mock_body
    mock_body='[{
        "id": "abc-123",
        "name": "test-agent",
        "status": "active",
        "label": "Test Agent",
        "program": "aider",
        "workingDirectory": "/home/mvillmow/TestProject",
        "programArgs": "--verbose",
        "taskDescription": "A test agent for unit tests",
        "tags": ["ci", "testing"],
        "owner": "mvillmow",
        "role": "member"
    }]'
    _start_mock_server 200 "$mock_body"

    _run_diff "${FIXTURES_DIR}/agent-valid.yaml"

    [[ "$status" -eq 1 ]]
    [[ "$output" == *"program"* ]]
}
