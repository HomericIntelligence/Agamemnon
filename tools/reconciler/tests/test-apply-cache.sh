#!/usr/bin/env bash
# tests/test-apply-cache.sh — Verify apply.sh makes O(1) agamemnon_list_agents calls
#
# These tests mock the Agamemnon API functions and source apply.sh internals
# to assert that the reconciliation loop does not re-fetch the agent list after
# every operation. See issue #6.
#
# Usage:
#   bash tests/test-apply-cache.sh
#   # Exit code 0 = all tests passed, non-zero = failure

set -euo pipefail

# Skip entire file on bash < 4 ([[ ]], arrays with [@], and other features require bash 4+)
if (( BASH_VERSINFO[0] < 4 )); then
    echo "SKIP: bash 4+ required (got ${BASH_VERSION})" >&2
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PASS=0
FAIL=0

# ── helpers ──────────────────────────────────────────────────────────────────

_assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS: ${desc}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}"
        echo "        expected: ${expected}"
        echo "        actual:   ${actual}"
        FAIL=$((FAIL + 1))
    fi
}

# Create a temporary YAML agent fixture and print its path.
_make_agent_yaml() {
    local name="$1"
    local desired_state="${2:-active}"
    local tmpfile
    tmpfile="$(mktemp /tmp/test-agent-XXXXXX.yaml)"
    cat > "$tmpfile" <<YAML
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: ${name}
  host: hermes
spec:
  label: ${name}
  program: claude-code
  model: null
  workingDirectory: /tmp
  programArgs: ""
  taskDescription: "test agent"
  tags: []
  owner: testuser
  role: member
  deployment:
    type: local
  desiredState: ${desired_state}
YAML
    echo "$tmpfile"
}

# ── test scaffolding ──────────────────────────────────────────────────────────
#
# We source reconcile.sh and api.sh, then override the API functions with mocks
# before sourcing the apply.sh function definitions. apply.sh calls `main "$@"`
# at the bottom, so we prevent that by defining a guard variable.

_setup_env() {
    export AGAMEMNON_URL="http://mock-agamemnon"
    # Use a temp file to count calls across subshell boundaries.
    export _LIST_CALL_COUNT_FILE
    _LIST_CALL_COUNT_FILE="$(mktemp)"
    echo "0" > "$_LIST_CALL_COUNT_FILE"

    # Override check_deps so we don't need yq/jq/curl on PATH to be Agamemnon-aware
    check_deps() { return 0; }
    agamemnon_check_connection() { return 0; }
}

_get_list_call_count() {
    cat "$_LIST_CALL_COUNT_FILE"
}

# ── mock factories ────────────────────────────────────────────────────────────

# Mock agamemnon_list_agents: increments counter file, returns provided JSON.
_mock_list_agents() {
    local fixture_json="$1"
    # Export fixture so subshells can access it
    export _LIST_AGENTS_FIXTURE="$fixture_json"
    agamemnon_list_agents() {
        local c
        c="$(cat "$_LIST_CALL_COUNT_FILE")"
        echo "$((c + 1))" > "$_LIST_CALL_COUNT_FILE"
        echo "$_LIST_AGENTS_FIXTURE"
    }
}

# Mock agamemnon_create_agent: returns a new agent JSON with a generated id.
_mock_create_agent() {
    local new_agent_json="$1"
    eval "agamemnon_create_agent() { echo '${new_agent_json}'; }"
}

# Mock other mutating calls to no-ops.
_mock_mutating_calls() {
    agamemnon_wake_agent()      { return 0; }
    agamemnon_hibernate_agent() { return 0; }
    agamemnon_update_agent()    { return 0; }
    agamemnon_delete_agent()    { return 0; }
}

# ── source apply.sh without running main() ───────────────────────────────────
#
# apply.sh ends with `main "$@"`. We prevent that by overriding main before
# sourcing, then restoring after.

_source_apply_functions() {
    local apply_sh="${REPO_ROOT}/scripts/apply.sh"

    # Source the lib dependencies directly with their real paths.
    # shellcheck source=scripts/lib/config.sh
    source "${REPO_ROOT}/scripts/lib/config.sh"
    # shellcheck source=scripts/lib/api.sh
    source "${REPO_ROOT}/scripts/lib/api.sh"
    # shellcheck source=scripts/lib/reconcile.sh
    source "${REPO_ROOT}/scripts/lib/reconcile.sh"
    # shellcheck source=scripts/lib/report.sh
    source "${REPO_ROOT}/scripts/lib/report.sh"

    # Eval apply.sh stripping lines that would re-source libs or run main.
    # Also strip SCRIPT_DIR/REPO_ROOT setup (already set) and shebang/set lines.
    local stripped
    stripped="$(grep -v \
        -e '^#!/' \
        -e '^set -' \
        -e 'SCRIPT_DIR=' \
        -e 'REPO_ROOT=' \
        -e '# shellcheck' \
        -e '^source ' \
        -e '^main "\$@"$' \
        "$apply_sh")"
    eval "$stripped"
}

# ── test cases ────────────────────────────────────────────────────────────────

test_no_creates_one_list_call() {
    echo ""
    echo "Test: all-unchanged run makes exactly 1 agamemnon_list_agents call"

    _setup_env

    # Fixture: two existing agents
    local fixture
    fixture='[{"id":"id-alpha","name":"alpha","status":"online","label":"alpha","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"test agent","tags":""},{"id":"id-beta","name":"beta","status":"online","label":"beta","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"test agent","tags":""}]'

    _mock_list_agents "$fixture"
    _mock_mutating_calls

    # Two YAML files that match the existing agents exactly (will be UNCHANGED)
    local yaml_alpha yaml_beta
    yaml_alpha="$(_make_agent_yaml alpha active)"
    yaml_beta="$(_make_agent_yaml beta active)"

    # Run the reconciliation loop directly (bypass main's arg parsing / dep check)
    local agents_json
    agents_json="$(agamemnon_list_agents)"
    local yaml_files=("$yaml_alpha" "$yaml_beta")

    for yaml_file in "${yaml_files[@]}"; do
        _LAST_CREATED_AGENT_JSON=""
        # The test asserts on list-call count, not on apply_agent's rc. Capture
        # rc explicitly so a regression in apply_agent's exit semantics is
        # still visible in the test transcript (rather than silently masked).
        _apply_rc=0
        apply_agent "$yaml_file" "$agents_json" > /dev/null 2>&1 || _apply_rc=$?
        if [[ "$_apply_rc" -ne 0 ]]; then
            echo "  (apply_agent rc=${_apply_rc} — not asserted)" >&2
        fi
        if [[ -n "$_LAST_CREATED_AGENT_JSON" ]]; then
            agents_json="$(echo "$agents_json" | jq --argjson new "$_LAST_CREATED_AGENT_JSON" '. + [$new]')"
        fi
    done

    rm -f "$yaml_alpha" "$yaml_beta"

    # Initial fetch counts as 1; loop should add 0 more.
    _assert_eq "list call count == 1 (no creates)" "1" "$(_get_list_call_count)"
}

test_creates_update_cache_without_refetch() {
    echo ""
    echo "Test: k creates cause 1 list call total (cache updated in-memory)"

    _setup_env

    # Fixture: no existing agents
    local fixture='[]'
    _mock_list_agents "$fixture"
    _mock_mutating_calls

    # Mock create to return a new agent JSON
    local new_agent='{"id":"new-id-1","name":"gamma","status":"offline","label":"gamma","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"test agent","tags":""}'
    _mock_create_agent "$new_agent"

    local yaml_gamma
    yaml_gamma="$(_make_agent_yaml gamma active)"

    local agents_json
    agents_json="$(agamemnon_list_agents)"
    local yaml_files=("$yaml_gamma")

    for yaml_file in "${yaml_files[@]}"; do
        _LAST_CREATED_AGENT_JSON=""
        # The test asserts on list-call count, not on apply_agent's rc. Capture
        # rc explicitly so a regression in apply_agent's exit semantics is
        # still visible in the test transcript (rather than silently masked).
        _apply_rc=0
        apply_agent "$yaml_file" "$agents_json" > /dev/null 2>&1 || _apply_rc=$?
        if [[ "$_apply_rc" -ne 0 ]]; then
            echo "  (apply_agent rc=${_apply_rc} — not asserted)" >&2
        fi
        if [[ -n "$_LAST_CREATED_AGENT_JSON" ]]; then
            agents_json="$(echo "$agents_json" | jq --argjson new "$_LAST_CREATED_AGENT_JSON" '. + [$new]')"
        fi
    done

    rm -f "$yaml_gamma"

    # 1 initial fetch; create should NOT trigger another list call.
    _assert_eq "list call count == 1 after 1 create" "1" "$(_get_list_call_count)"
}

test_cache_contains_new_agent_after_create() {
    echo ""
    echo "Test: agents_json cache contains new agent entry after create"

    _setup_env

    local fixture='[]'
    _mock_list_agents "$fixture"
    _mock_mutating_calls

    local new_agent='{"id":"new-id-2","name":"delta","status":"offline","label":"delta","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"test agent","tags":""}'
    _mock_create_agent "$new_agent"

    local yaml_delta
    yaml_delta="$(_make_agent_yaml delta active)"

    local agents_json
    agents_json="$(agamemnon_list_agents)"
    local yaml_files=("$yaml_delta")

    for yaml_file in "${yaml_files[@]}"; do
        _LAST_CREATED_AGENT_JSON=""
        # The test asserts on list-call count, not on apply_agent's rc. Capture
        # rc explicitly so a regression in apply_agent's exit semantics is
        # still visible in the test transcript (rather than silently masked).
        _apply_rc=0
        apply_agent "$yaml_file" "$agents_json" > /dev/null 2>&1 || _apply_rc=$?
        if [[ "$_apply_rc" -ne 0 ]]; then
            echo "  (apply_agent rc=${_apply_rc} — not asserted)" >&2
        fi
        if [[ -n "$_LAST_CREATED_AGENT_JSON" ]]; then
            agents_json="$(echo "$agents_json" | jq --argjson new "$_LAST_CREATED_AGENT_JSON" '. + [$new]')"
        fi
    done

    rm -f "$yaml_delta"

    # The cache should now contain the new agent
    local found_id
    found_id="$(echo "$agents_json" | jq -r '.[] | select(.name == "delta") | .id')"
    _assert_eq "new agent id in cache after create" "new-id-2" "$found_id"
}

test_multiple_creates_single_list_call() {
    echo ""
    echo "Test: 3 creates still produce only 1 agamemnon_list_agents call"

    _setup_env

    local fixture='[]'
    _mock_list_agents "$fixture"
    _mock_mutating_calls

    # Each create returns a different agent; use a file counter to survive subshells.
    local _create_count_file
    _create_count_file="$(mktemp)"
    echo "0" > "$_create_count_file"
    export _create_count_file
    agamemnon_create_agent() {
        local c
        c="$(cat "$_create_count_file")"
        c=$((c + 1))
        echo "$c" > "$_create_count_file"
        echo "{\"id\":\"new-id-${c}\",\"name\":\"agent${c}\",\"status\":\"offline\",\"label\":\"agent${c}\",\"program\":\"claude-code\",\"workingDirectory\":\"/tmp\",\"programArgs\":\"\",\"taskDescription\":\"test agent\",\"tags\":\"\"}"
    }

    local y1 y2 y3
    y1="$(_make_agent_yaml agent1 active)"
    y2="$(_make_agent_yaml agent2 active)"
    y3="$(_make_agent_yaml agent3 active)"

    local agents_json
    agents_json="$(agamemnon_list_agents)"
    local yaml_files=("$y1" "$y2" "$y3")

    for yaml_file in "${yaml_files[@]}"; do
        _LAST_CREATED_AGENT_JSON=""
        # The test asserts on list-call count, not on apply_agent's rc. Capture
        # rc explicitly so a regression in apply_agent's exit semantics is
        # still visible in the test transcript (rather than silently masked).
        _apply_rc=0
        apply_agent "$yaml_file" "$agents_json" > /dev/null 2>&1 || _apply_rc=$?
        if [[ "$_apply_rc" -ne 0 ]]; then
            echo "  (apply_agent rc=${_apply_rc} — not asserted)" >&2
        fi
        if [[ -n "$_LAST_CREATED_AGENT_JSON" ]]; then
            agents_json="$(echo "$agents_json" | jq --argjson new "$_LAST_CREATED_AGENT_JSON" '. + [$new]')"
        fi
    done

    rm -f "$y1" "$y2" "$y3" "$_create_count_file"

    _assert_eq "list call count == 1 after 3 creates" "1" "$(_get_list_call_count)"

    # All 3 created agents should be in the cache
    local cache_count
    cache_count="$(echo "$agents_json" | jq 'length')"
    _assert_eq "cache contains all 3 created agents" "3" "$cache_count"
}

# ── run ───────────────────────────────────────────────────────────────────────

echo "================================================"
echo "apply.sh cache optimization tests (issue #6)"
echo "================================================"

# Source apply.sh function definitions (without running main)
_source_apply_functions

test_no_creates_one_list_call
test_creates_update_cache_without_refetch
test_cache_contains_new_agent_after_create
test_multiple_creates_single_list_call

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"
echo "================================================"

[[ $FAIL -eq 0 ]]
