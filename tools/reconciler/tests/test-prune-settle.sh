#!/usr/bin/env bash
# tests/test-prune-settle.sh — Verify HIBERNATE_SETTLE_SECONDS replaces magic sleep 2
#
# Tests that:
#   1. apply.sh uses sleep "$HIBERNATE_SETTLE_SECONDS" (not a hardcoded number)
#   2. The value defaults to 2 when the env var is unset
#   3. Setting HIBERNATE_SETTLE_SECONDS=0 causes sleep to be called with "0"
#
# Usage:
#   bash tests/test-prune-settle.sh
#   # Exit code 0 = all tests passed, non-zero = failure

set -euo pipefail

if (( BASH_VERSINFO[0] < 4 )); then
    echo "SKIP: bash 4+ required (got ${BASH_VERSION})" >&2
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
APPLY_SH="${REPO_ROOT}/scripts/apply.sh"

PASS=0
FAIL=0

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

# ── test: --help output documents HIBERNATE_SETTLE_SECONDS ───────────────────

test_help_documents_hibernate_settle_seconds() {
    echo ""
    echo "Test: --help output mentions HIBERNATE_SETTLE_SECONDS"

    local found
    found="$(grep -c 'HIBERNATE_SETTLE_SECONDS' "$APPLY_SH" || true)"
    _assert_eq "HIBERNATE_SETTLE_SECONDS appears in apply.sh" "1" "$([ "$found" -ge 1 ] && echo 1 || echo 0)"

    # Verify it appears specifically in the usage() function body
    local in_usage
    in_usage="$(awk '/^usage\(\)/{found=1} found && /HIBERNATE_SETTLE_SECONDS/{print; exit}' "$APPLY_SH")"
    _assert_eq "HIBERNATE_SETTLE_SECONDS documented in usage()" "1" "$([ -n "$in_usage" ] && echo 1 || echo 0)"
}

# ── test: static grep — confirm sleep uses the variable, not a literal ────────

test_no_hardcoded_sleep_2() {
    echo ""
    echo "Test: apply.sh does not contain hardcoded 'sleep 2'"

    # Allow 'sleep 2' only in comments; the actual call must use the variable.
    local hardcoded_count
    hardcoded_count="$(grep -cE '^\s+sleep 2\s*$' "$APPLY_SH" || true)"
    _assert_eq "no bare 'sleep 2' lines in apply.sh" "0" "$hardcoded_count"
}

test_sleep_uses_variable() {
    echo ""
    echo "Test: apply.sh contains sleep \"\$HIBERNATE_SETTLE_SECONDS\""

    local found
    found="$(grep -cE 'sleep "\$HIBERNATE_SETTLE_SECONDS"' "$APPLY_SH" || true)"
    _assert_eq "sleep variable call exists in apply.sh" "1" "$found"
}

test_default_value_is_2() {
    echo ""
    echo "Test: HIBERNATE_SETTLE_SECONDS defaults to 2 when unset"

    local default_line
    default_line="$(grep 'HIBERNATE_SETTLE_SECONDS=' "$APPLY_SH" | grep ':-2')"
    _assert_eq "default value is 2" "0" "$( [[ -n "$default_line" ]] && echo 0 || echo 1 )"
}

# ── test: runtime — mock sleep and confirm it is called with the right value ──

_source_apply_functions() {
    # shellcheck source=scripts/lib/config.sh
    source "${REPO_ROOT}/scripts/lib/config.sh"
    # shellcheck source=scripts/lib/api.sh
    source "${REPO_ROOT}/scripts/lib/api.sh"
    # shellcheck source=scripts/lib/reconcile.sh
    source "${REPO_ROOT}/scripts/lib/reconcile.sh"
    # shellcheck source=scripts/lib/report.sh
    source "${REPO_ROOT}/scripts/lib/report.sh"

    local stripped
    stripped="$(grep -v \
        -e '^#!/' \
        -e '^set -' \
        -e 'SCRIPT_DIR=' \
        -e 'REPO_ROOT=' \
        -e '# shellcheck' \
        -e '^source ' \
        -e '^main "\$@"$' \
        "$APPLY_SH")"
    eval "$stripped"
}

_make_agent_yaml() {
    local name="$1"
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
  desiredState: active
YAML
    echo "$tmpfile"
}

test_prune_calls_sleep_with_env_value() {
    echo ""
    echo "Test: handle_unmanaged calls sleep with HIBERNATE_SETTLE_SECONDS value"

    export AGAMEMNON_URL="http://mock-agamemnon"

    # Track what sleep was called with via a temp file (survives subshells)
    local sleep_arg_file
    sleep_arg_file="$(mktemp)"

    # Capture sleep argument; override in current shell via function
    sleep() { echo "$1" > "$sleep_arg_file"; }

    check_deps()                { return 0; }
    agamemnon_check_connection(){ return 0; }
    agamemnon_hibernate_agent() { return 0; }
    agamemnon_delete_agent()    { return 0; }
    snapshot_agent()            { return 0; }
    record_failure()            { return 0; }
    report_add_agent()          { return 0; }

    _source_apply_functions

    # Override sleep again after sourcing (source may have reset environment)
    sleep() { echo "$1" > "$sleep_arg_file"; }

    # One managed YAML, one unmanaged agent in Agamemnon
    local yaml_managed
    yaml_managed="$(_make_agent_yaml managed-agent)"

    local agents_json
    agents_json='[{"id":"id-unmanaged","name":"unmanaged-agent","status":"online","label":"unmanaged-agent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"test","tags":""}]'

    # shellcheck disable=SC2034
    PRUNE=1
    # shellcheck disable=SC2034
    OUTPUT_FORMAT="text"
    # shellcheck disable=SC2034
    HIBERNATE_SETTLE_SECONDS=7

    # We assert on the captured sleep argument, not on handle_unmanaged's rc;
    # capture rc explicitly rather than silently swallowing it.
    _hu_rc=0
    handle_unmanaged "$agents_json" "$yaml_managed" > /dev/null 2>&1 || _hu_rc=$?
    if [[ "$_hu_rc" -ne 0 ]]; then
        echo "  (handle_unmanaged rc=${_hu_rc} — not asserted)" >&2
    fi

    rm -f "$yaml_managed"

    local actual_sleep_arg
    actual_sleep_arg="$(cat "$sleep_arg_file" 2>/dev/null || echo "")"
    rm -f "$sleep_arg_file"

    _assert_eq "sleep called with HIBERNATE_SETTLE_SECONDS value (7)" "7" "$actual_sleep_arg"
}

test_prune_zero_settle_no_wait() {
    echo ""
    echo "Test: HIBERNATE_SETTLE_SECONDS=0 calls sleep with 0 (fast CI path)"

    export AGAMEMNON_URL="http://mock-agamemnon"

    local sleep_arg_file
    sleep_arg_file="$(mktemp)"

    sleep() { echo "$1" > "$sleep_arg_file"; }

    check_deps()                { return 0; }
    agamemnon_check_connection(){ return 0; }
    agamemnon_hibernate_agent() { return 0; }
    agamemnon_delete_agent()    { return 0; }
    snapshot_agent()            { return 0; }
    record_failure()            { return 0; }
    report_add_agent()          { return 0; }

    _source_apply_functions

    sleep() { echo "$1" > "$sleep_arg_file"; }

    local yaml_managed
    yaml_managed="$(_make_agent_yaml managed-agent)"

    local agents_json
    agents_json='[{"id":"id-unmanaged","name":"unmanaged-agent","status":"online","label":"unmanaged-agent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"test","tags":""}]'

    # shellcheck disable=SC2034
    PRUNE=1
    # shellcheck disable=SC2034
    OUTPUT_FORMAT="text"
    # shellcheck disable=SC2034
    HIBERNATE_SETTLE_SECONDS=0

    # We assert on the captured sleep argument, not on handle_unmanaged's rc;
    # capture rc explicitly rather than silently swallowing it.
    _hu_rc=0
    handle_unmanaged "$agents_json" "$yaml_managed" > /dev/null 2>&1 || _hu_rc=$?
    if [[ "$_hu_rc" -ne 0 ]]; then
        echo "  (handle_unmanaged rc=${_hu_rc} — not asserted)" >&2
    fi

    rm -f "$yaml_managed"

    local actual_sleep_arg
    actual_sleep_arg="$(cat "$sleep_arg_file" 2>/dev/null || echo "")"
    rm -f "$sleep_arg_file"

    _assert_eq "sleep called with 0 when HIBERNATE_SETTLE_SECONDS=0" "0" "$actual_sleep_arg"
}

# ── run ───────────────────────────────────────────────────────────────────────

echo "================================================"
echo "HIBERNATE_SETTLE_SECONDS tests (issue #373)"
echo "================================================"

test_help_documents_hibernate_settle_seconds
test_no_hardcoded_sleep_2
test_sleep_uses_variable
test_default_value_is_2
test_prune_calls_sleep_with_env_value
test_prune_zero_settle_no_wait

echo ""
echo "================================================"
echo "Results: ${PASS} passed, ${FAIL} failed"
echo "================================================"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
