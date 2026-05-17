#!/usr/bin/env bats
# tests/integration/test_apply_prune_convergence.bats
#
# Integration tests for the --prune path through verify_convergence (#409).
#
# These tests invoke apply.sh --prune end-to-end against a mock Agamemnon server
# using a routes config file. The mock API returns an "unmanaged-agent" that is
# NOT present in any YAML under the test host directory, so handle_unmanaged()
# prunes it and _PRUNED_NAMES gets populated.
#
# Routes design for "fail" scenario (prune-convergence-fail.json):
#   - All GET /api/v1/agents → unmanaged-agent still present (stateless default)
#   - DELETE /api/v1/agents/* → 200
#
#   The convergence re-fetch still sees the agent → verify_convergence fails →
#   apply.sh exits 1.
#
# Routes design for "ok" scenario (prune-convergence-ok.json):
#   - First GET /api/v1/agents → unmanaged-agent in list (once: true route)
#   - DELETE /api/v1/agents/* → 200
#   - All subsequent GETs → [] (default_body is empty list)
#
#   handle_unmanaged() sees the agent on the initial list and prunes it.
#   verify_convergence re-fetches and gets [] → agent gone → exits 0.
#
# Covers issue #409 (missing integration test for _PRUNED_NAMES convergence path).

# shellcheck source=scripts/lib/api.sh

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/tests/fixtures"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"
APPLY_SH="${SCRIPT_DIR}/scripts/apply.sh"
MOCK_PORT=18090
MOCK_PID_FILE="/tmp/bats-prune-convergence-mock-$$.pid"

# ---------------------------------------------------------------------------
# Mock server helpers
# ---------------------------------------------------------------------------

_start_mock_server_routes() {
    local routes_file="$1"
    python3 "${HELPERS_DIR}/mock_server.py" "$MOCK_PORT" --routes "$routes_file" \
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
# Setup / teardown
# ---------------------------------------------------------------------------

_PRUNE_TEST_HOST=""
_PRUNE_SNAPSHOT_DIR=""
_PRUNE_LOCK_FILE=""

_setup_test_agent() {
    _PRUNE_TEST_HOST="prunetest-$$-${RANDOM}"
    mkdir -p "${SCRIPT_DIR}/agents/${_PRUNE_TEST_HOST}"
    # Create a managed agent YAML — its name differs from "unmanaged-agent" so
    # handle_unmanaged() will flag "unmanaged-agent" as unmanaged and prune it.
    printf 'apiVersion: myrmidons/v1\nkind: Agent\nmetadata:\n  name: managed-agent\n  host: %s\nspec:\n  label: Managed Agent\n  program: claude-code\n  model: null\n  workingDirectory: /tmp/managed\n  programArgs: ""\n  taskDescription: "managed agent for prune convergence test"\n  tags: []\n  owner: ci\n  role: member\n  deployment:\n    type: local\n  desiredState: active\n' \
        "$_PRUNE_TEST_HOST" \
        > "${SCRIPT_DIR}/agents/${_PRUNE_TEST_HOST}/managed-agent.yaml"
}

_cleanup_test_agent() {
    if [[ -n "${_PRUNE_TEST_HOST:-}" ]]; then
        rm -rf "${SCRIPT_DIR}/agents/${_PRUNE_TEST_HOST}"
        _PRUNE_TEST_HOST=""
    fi
}

setup() {
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
    export NO_COLOR=1
    export HIBERNATE_SETTLE_SECONDS=0
    _PRUNE_SNAPSHOT_DIR="$(mktemp -d)"
    _PRUNE_LOCK_FILE="$(mktemp -u /tmp/bats-prune-lock-$$.XXXXXX)"
    export AIM_LOCK_FILE="$_PRUNE_LOCK_FILE"
    _setup_test_agent
}

teardown() {
    _stop_mock_server
    _cleanup_test_agent
    rm -rf "${_PRUNE_SNAPSHOT_DIR:-}"
    rm -f "${_PRUNE_LOCK_FILE:-}"
    unset NO_COLOR HIBERNATE_SETTLE_SECONDS AIM_LOCK_FILE
}

# ---------------------------------------------------------------------------
# Test 1: exit code 1 when pruned agent is still returned by the API
#
# Routes design (prune-convergence-fail.json):
#   All GET /api/v1/agents  → unmanaged-agent present (default_body)
#   DELETE /api/v1/agents/* → 200
#
# handle_unmanaged() prunes "unmanaged-agent"; verify_convergence re-fetches
# and still sees it in the list → reports convergence failure → apply exits 1.
# ---------------------------------------------------------------------------

@test "apply --prune: exit code 1 when pruned agent still returned by API after deletion" {
    _start_mock_server_routes "${FIXTURES_DIR}/prune-convergence-fail.json"

    run bash "$APPLY_SH" "$_PRUNE_TEST_HOST" --prune --yes \
        --snapshot-dir "$_PRUNE_SNAPSHOT_DIR" 2>&1 || true

    [[ "$status" -eq 1 ]]
    [[ "$output" == *"[-] Pruning unmanaged: unmanaged-agent"* ]]
    [[ "$output" == *"pruned but still present in API (convergence failed)"* ]]
    [[ "$output" == *"Convergence:"*"failed"* ]]
}

# ---------------------------------------------------------------------------
# Test 2: exit code 0 when pruned agent is no longer returned by the API
#
# Routes design (prune-convergence-ok.json):
#   First GET /api/v1/agents → unmanaged-agent present (once: true route)
#   DELETE /api/v1/agents/* → 200
#   Subsequent GETs         → [] (default_body)
#
# handle_unmanaged() sees the agent on the initial list and prunes it.
# verify_convergence re-fetches, gets [] → agent is gone → exits 0.
# ---------------------------------------------------------------------------

@test "apply --prune: exit code 0 when pruned agent no longer returned by API" {
    _start_mock_server_routes "${FIXTURES_DIR}/prune-convergence-ok.json"

    run bash "$APPLY_SH" "$_PRUNE_TEST_HOST" --prune --yes \
        --snapshot-dir "$_PRUNE_SNAPSHOT_DIR" 2>&1

    [[ "$status" -eq 0 ]]
    [[ "$output" == *"[-] Pruning unmanaged: unmanaged-agent"* ]]
    [[ "$output" != *"pruned but still present in API"* ]]
}
