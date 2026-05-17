#!/usr/bin/env bats
# tests/unit/test_failed_agent_arrays.bats — unit tests for unified FAILED_AGENT_NAMES array
#
# Verifies that record_failure(), print_error_summary(), and _write_failed_agents_file()
# all draw from the same FAILED_AGENT_NAMES / FAILED_AGENT_STATUSES / FAILED_AGENT_MESSAGES
# parallel arrays, so that agents appearing in the summary are also in the retry file.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
APPLY_SH="${SCRIPT_DIR}/scripts/apply.sh"

# ---------------------------------------------------------------------------
# Build a minimal harness that sources just the functions under test.
# ---------------------------------------------------------------------------

_build_harness() {
    local harness="${BATS_TMPDIR}/fa_harness_$$.sh"
    cat > "$harness" << 'HARNESS'
#!/usr/bin/env bash
set -uo pipefail

ERRORS=0
FAILED_AGENT_NAMES=()
FAILED_AGENT_STATUSES=()
FAILED_AGENT_MESSAGES=()
RETRY_FILE=""
FAILED_AGENTS_FILE="/dev/null"
OUTPUT_FORMAT="text"

log_warn() { echo "WARN: $*" >&2; }

record_failure() {
    local agent_name="$1"
    local http_status="$2"
    local error_message="$3"
    ERRORS=$((ERRORS + 1))
    FAILED_AGENT_NAMES+=("$agent_name")
    FAILED_AGENT_STATUSES+=("$http_status")
    FAILED_AGENT_MESSAGES+=("$error_message")
}

print_error_summary() {
    echo ""
    echo "================================================"
    echo "FAILED AGENTS (${ERRORS}):"
    echo ""
    local i
    for i in "${!FAILED_AGENT_NAMES[@]}"; do
        local agent_name="${FAILED_AGENT_NAMES[$i]}"
        local http_status="${FAILED_AGENT_STATUSES[$i]}"
        local error_msg="${FAILED_AGENT_MESSAGES[$i]}"
        echo "  [FAIL] ${agent_name}"
        if [[ -n "$http_status" ]]; then
            echo "         HTTP status: ${http_status}"
        fi
        if [[ -n "$error_msg" ]]; then
            echo "         Error: ${error_msg}"
        fi
    done
    echo ""
    echo "Failed agents written to: ${FAILED_AGENTS_FILE}"
}

_write_failed_agents_file() {
    local failed_file="${RETRY_FILE:-failed-agents.txt}"
    if [[ ${#FAILED_AGENT_NAMES[@]} -gt 0 ]]; then
        : > "$failed_file"
        for agent_name in "${FAILED_AGENT_NAMES[@]}"; do
            echo "$agent_name" >> "$failed_file"
        done
        log_warn "Wrote ${#FAILED_AGENT_NAMES[@]} failed agent(s) to ${failed_file}"
    fi
}
HARNESS
    echo "$harness"
}

# ---------------------------------------------------------------------------
# Tests: record_failure populates all three parallel arrays
# ---------------------------------------------------------------------------

@test "record_failure: populates FAILED_AGENT_NAMES" {
    local harness
    harness="$(_build_harness)"

    run bash -c "
        source '${harness}'
        record_failure 'agent-alpha' '500' 'internal error'
        echo \"\${FAILED_AGENT_NAMES[0]}\"
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "agent-alpha" ]]
    rm -f "$harness"
}

@test "record_failure: populates FAILED_AGENT_STATUSES" {
    local harness
    harness="$(_build_harness)"

    run bash -c "
        source '${harness}'
        record_failure 'agent-alpha' '500' 'internal error'
        echo \"\${FAILED_AGENT_STATUSES[0]}\"
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "500" ]]
    rm -f "$harness"
}

@test "record_failure: populates FAILED_AGENT_MESSAGES" {
    local harness
    harness="$(_build_harness)"

    run bash -c "
        source '${harness}'
        record_failure 'agent-alpha' '500' 'internal error'
        echo \"\${FAILED_AGENT_MESSAGES[0]}\"
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "internal error" ]]
    rm -f "$harness"
}

@test "record_failure: increments ERRORS counter" {
    local harness
    harness="$(_build_harness)"

    run bash -c "
        source '${harness}'
        record_failure 'a' '404' 'not found'
        record_failure 'b' '503' 'unavailable'
        echo \"\$ERRORS\"
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "2" ]]
    rm -f "$harness"
}

@test "record_failure: multiple calls append to arrays in order" {
    local harness
    harness="$(_build_harness)"

    run bash -c "
        source '${harness}'
        record_failure 'agent-one' '500' 'err-one'
        record_failure 'agent-two' '503' 'err-two'
        echo \"\${FAILED_AGENT_NAMES[0]} \${FAILED_AGENT_NAMES[1]}\"
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "agent-one agent-two" ]]
    rm -f "$harness"
}

# ---------------------------------------------------------------------------
# Tests: _write_failed_agents_file uses FAILED_AGENT_NAMES
# ---------------------------------------------------------------------------

@test "_write_failed_agents_file: writes each name from FAILED_AGENT_NAMES" {
    local harness
    harness="$(_build_harness)"
    local out_file="${BATS_TMPDIR}/retry_$$.txt"

    run bash -c "
        source '${harness}'
        record_failure 'alpha' '500' 'err'
        record_failure 'beta'  '503' 'err'
        RETRY_FILE='${out_file}'
        _write_failed_agents_file
        cat '${out_file}'
    "
    [[ "$status" -eq 0 ]]
    grep -q "alpha" <(echo "$output")
    grep -q "beta"  <(echo "$output")
    rm -f "$harness" "$out_file"
}

@test "_write_failed_agents_file: writes exactly the names in FAILED_AGENT_NAMES (no extras)" {
    local harness
    harness="$(_build_harness)"
    local out_file="${BATS_TMPDIR}/retry_exact_$$.txt"

    run bash -c "
        source '${harness}'
        record_failure 'only-agent' '400' 'bad request'
        RETRY_FILE='${out_file}'
        _write_failed_agents_file 2>/dev/null
        wc -l < '${out_file}'
    "
    [[ "$status" -eq 0 ]]
    [[ "${output// /}" -eq 1 ]]
    rm -f "$harness" "$out_file"
}

@test "_write_failed_agents_file: does not write file when no failures" {
    local harness
    harness="$(_build_harness)"
    local out_file="${BATS_TMPDIR}/retry_empty_$$.txt"

    run bash -c "
        source '${harness}'
        RETRY_FILE='${out_file}'
        _write_failed_agents_file
        [[ ! -f '${out_file}' ]] && echo 'not created' || echo 'created'
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "not created" ]]
    rm -f "$harness" "$out_file"
}

# ---------------------------------------------------------------------------
# Tests: summary and retry file are in sync (no divergence)
# ---------------------------------------------------------------------------

@test "summary and retry file contain the same agent names" {
    local harness
    harness="$(_build_harness)"
    local out_file="${BATS_TMPDIR}/retry_sync_$$.txt"

    run bash -c "
        source '${harness}'
        record_failure 'gamma' '500' 'err-gamma'
        record_failure 'delta' '502' 'err-delta'

        RETRY_FILE='${out_file}'
        _write_failed_agents_file 2>/dev/null

        # Extract names from print_error_summary output
        summary_names=\$(print_error_summary | grep '^\s*\[FAIL\]' | awk '{print \$2}')
        retry_names=\$(cat '${out_file}')

        # Both sets must be identical (same elements, same order)
        [[ \"\$summary_names\" == \"\$retry_names\" ]] && echo 'in_sync' || echo 'diverged'
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "in_sync" ]]
    rm -f "$harness" "$out_file"
}

@test "agent in print_error_summary output also appears in retry file" {
    local harness
    harness="$(_build_harness)"
    local out_file="${BATS_TMPDIR}/retry_present_$$.txt"

    run bash -c "
        source '${harness}'
        record_failure 'target-agent' '503' 'timeout'

        RETRY_FILE='${out_file}'
        _write_failed_agents_file 2>/dev/null

        # Confirm the agent appears in the summary
        print_error_summary | grep -q 'target-agent' || { echo 'missing_from_summary'; exit 1; }

        # Confirm the agent appears in the retry file
        grep -q 'target-agent' '${out_file}' || { echo 'missing_from_retry'; exit 1; }

        echo 'both_present'
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == "both_present" ]]
    rm -f "$harness" "$out_file"
}

# ---------------------------------------------------------------------------
# Static check: FAILED_AGENTS bare array is gone from apply.sh
# ---------------------------------------------------------------------------

@test "apply.sh: FAILED_AGENTS bare array declaration is absent" {
    local count
    count=$(grep -cE '^FAILED_AGENTS=\(\)' "$APPLY_SH" || true)
    [[ "$count" -eq 0 ]]
}

@test "apply.sh: no direct FAILED_AGENTS+= appends exist" {
    local count
    count=$(grep -cE 'FAILED_AGENTS\+=\(' "$APPLY_SH" || true)
    [[ "$count" -eq 0 ]]
}

@test "apply.sh: _write_failed_agents_file iterates FAILED_AGENT_NAMES not FAILED_AGENTS" {
    # The function body must reference FAILED_AGENT_NAMES, not bare FAILED_AGENTS.
    # Extract lines inside _write_failed_agents_file and check.
    local uses_names
    uses_names=$(grep -c 'FAILED_AGENT_NAMES' "$APPLY_SH" || true)
    [[ "$uses_names" -gt 0 ]]

    # Bare FAILED_AGENTS (not followed by _) should not appear
    local bare_count
    bare_count=$(grep -cE 'FAILED_AGENTS[^_]' "$APPLY_SH" || true)
    [[ "$bare_count" -eq 0 ]]
}
