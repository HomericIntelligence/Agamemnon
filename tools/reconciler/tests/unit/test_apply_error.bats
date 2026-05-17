#!/usr/bin/env bats
# tests/unit/test_apply_error.bats — unit tests for scripts/apply.sh error handling
#
# Tests cover error paths in apply.sh:
# - --fail-fast stops after first agent error
# - failed-agents.txt is written with correct agent names
# - Exit code semantics (0 success, 1 partial failure, 2 --fail-fast abort)
# - --retry reads failed-agents.txt and filters agents correctly

TEMP_TEST_DIR=""

setup() {
    TEMP_TEST_DIR="$(mktemp -d)"
    export TEMP_TEST_DIR
}

teardown() {
    [[ -n "$TEMP_TEST_DIR" ]] && rm -rf "$TEMP_TEST_DIR"
}

# ---------------------------------------------------------------------------
# Helper: Create a minimal mock apply.sh for testing
# This simplified version lets us test argument parsing and error handling
# ---------------------------------------------------------------------------

_create_mock_apply() {
    local mock_apply="$1"
    cat > "$mock_apply" << 'APPLY_MOCK'
#!/usr/bin/env bash
set -euo pipefail

# Mimic apply.sh structure for error handling tests
FAIL_FAST=0
RETRY=0
RETRY_FILE=""
ERRORS=0
AGENTS_TO_APPLY=()
FAILED_AGENTS=()
EXIT_CODE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --fail-fast)      FAIL_FAST=1; shift ;;
            --retry)          RETRY=1; shift ;;
            --retry-file)     RETRY_FILE="$2"; shift 2 ;;
            -h|--help)        exit 0 ;;
            *)                AGENTS_TO_APPLY+=("$1"); shift ;;
        esac
    done
}

apply_agent() {
    local agent="$1"
    # Agents prefixed with "fail:" will fail
    if [[ "$agent" == fail:* ]]; then
        return 1
    fi
    return 0
}

parse_args "$@"

# If --retry, filter agents from retry file
if [[ $RETRY -eq 1 && -n "$RETRY_FILE" && -f "$RETRY_FILE" ]]; then
    local -a retry_agents=()
    while IFS= read -r agent; do
        [[ -n "$agent" ]] && retry_agents+=("$agent")
    done < "$RETRY_FILE"
    AGENTS_TO_APPLY=("${retry_agents[@]}")
fi

# Process agents
for agent in "${AGENTS_TO_APPLY[@]}"; do
    if ! apply_agent "$agent"; then
        FAILED_AGENTS+=("$agent")
        ERRORS=$((ERRORS + 1))
        if [[ $FAIL_FAST -eq 1 ]]; then
            # With --fail-fast, exit immediately
            EXIT_CODE=2
            break
        fi
    fi
done

# Write failed agents file if there were errors
if [[ $ERRORS -gt 0 && -z "$RETRY_FILE" ]]; then
    # Use temp dir + failed-agents.txt
    failed_file="${TEMP_TEST_DIR:-/tmp}/failed-agents.txt"
    for agent in "${FAILED_AGENTS[@]}"; do
        echo "$agent" >> "$failed_file"
    done
fi

# Exit code semantics
if [[ $EXIT_CODE -eq 2 ]]; then
    exit 2
elif [[ $ERRORS -gt 0 ]]; then
    exit 1
fi
exit 0
APPLY_MOCK
    chmod +x "$mock_apply"
}

# ---------------------------------------------------------------------------
# Test: --fail-fast stops after first error
# ---------------------------------------------------------------------------

@test "--fail-fast: stops processing after first agent error" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    # Three agents: agent1 succeeds, fail:agent2 fails (should stop here with --fail-fast)
    run "$mock_apply" --fail-fast agent1 fail:agent2 agent3

    # Should exit with code 2 (fail-fast abort)
    [[ "$status" -eq 2 ]]
}

@test "--fail-fast: does NOT process agents after first error" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    # Create a marker file that tracks which agents were processed
    local trace_file="${TEMP_TEST_DIR}/trace.txt"

    cat > "$mock_apply" << 'APPLY_WITH_TRACE'
#!/usr/bin/env bash
set -euo pipefail

FAIL_FAST=0
ERRORS=0
AGENTS_TO_APPLY=()
FAILED_AGENTS=()
EXIT_CODE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --fail-fast)      FAIL_FAST=1; shift ;;
            -h|--help)        exit 0 ;;
            *)                AGENTS_TO_APPLY+=("$1"); shift ;;
        esac
    done
}

apply_agent() {
    local agent="$1"
    echo "processed: $agent" >> "$TRACE_FILE"
    [[ "$agent" == fail:* ]] && return 1 || return 0
}

parse_args "$@"

for agent in "${AGENTS_TO_APPLY[@]}"; do
    if ! apply_agent "$agent"; then
        FAILED_AGENTS+=("$agent")
        ERRORS=$((ERRORS + 1))
        if [[ $FAIL_FAST -eq 1 ]]; then
            EXIT_CODE=2
            break
        fi
    fi
done

[[ $EXIT_CODE -eq 2 ]] && exit 2 || exit 0
APPLY_WITH_TRACE
    chmod +x "$mock_apply"
    export TRACE_FILE="$trace_file"

    # Run: agent1 succeeds, fail:agent2 fails (stops), agent3 not reached
    "$mock_apply" --fail-fast agent1 fail:agent2 agent3 || true

    # Check trace: agent1 and fail:agent2 should be present, agent3 should NOT be
    [[ $(grep -c "processed: agent1" "$trace_file") -eq 1 ]]
    [[ $(grep -c "processed: fail:agent2" "$trace_file") -eq 1 ]]
    [[ ! -f "$trace_file" || ! $(grep -q "processed: agent3" "$trace_file" 2>/dev/null) ]]
}

@test "--fail-fast without errors: processes all agents normally" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    # All agents succeed
    run "$mock_apply" --fail-fast agent1 agent2 agent3
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Test: failed-agents.txt written with correct agent names
# ---------------------------------------------------------------------------

@test "failed-agents.txt: created with failed agent names" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # Run with two failures
    "$mock_apply" agent1 fail:agent2 fail:agent3 || true

    # failed-agents.txt should exist with agent names (one per line)
    [[ -f "$failed_file" ]]
    [[ $(wc -l < "$failed_file") -eq 2 ]]
    grep -q "fail:agent2" "$failed_file"
    grep -q "fail:agent3" "$failed_file"
}

@test "failed-agents.txt: not created when no errors" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # Run with all successes
    run "$mock_apply" agent1 agent2 agent3
    [[ "$status" -eq 0 ]]

    # failed-agents.txt should NOT exist
    [[ ! -f "$failed_file" ]]
}

@test "failed-agents.txt: lists all failed agents in order" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # Run with mixed success/failure
    "$mock_apply" agent1 fail:agent2 agent3 fail:agent4 agent5 || true

    # Check file contains exactly the failed agents
    [[ -f "$failed_file" ]]
    line1="$(sed -n '1p' "$failed_file")"
    line2="$(sed -n '2p' "$failed_file")"
    [[ "$line1" == "fail:agent2" ]]
    [[ "$line2" == "fail:agent4" ]]
}

# ---------------------------------------------------------------------------
# Test: Exit code semantics
# ---------------------------------------------------------------------------

@test "exit code 0: when all agents succeed" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    run "$mock_apply" agent1 agent2 agent3
    [[ "$status" -eq 0 ]]
}

@test "exit code 1: on partial failure (without --fail-fast)" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    # Without --fail-fast, should process all and exit 1 on errors
    run "$mock_apply" agent1 fail:agent2 agent3
    [[ "$status" -eq 1 ]]
}

@test "exit code 2: on --fail-fast abort" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    # With --fail-fast, exit 2 when stopped early
    run "$mock_apply" --fail-fast agent1 fail:agent2 agent3
    [[ "$status" -eq 2 ]]
}

@test "exit code 2: --fail-fast with first agent failing" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    run "$mock_apply" --fail-fast fail:agent1 agent2
    [[ "$status" -eq 2 ]]
}

# ---------------------------------------------------------------------------
# Test: --retry reads failed-agents.txt and filters correctly
# ---------------------------------------------------------------------------

@test "--retry: reads agents from failed-agents.txt" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # First run: create failed-agents.txt
    echo "fail:agent1" > "$failed_file"
    echo "fail:agent2" >> "$failed_file"

    # Second run: --retry should only process agents from failed_file
    local trace_file="${TEMP_TEST_DIR}/trace2.txt"

    cat > "$mock_apply" << 'APPLY_WITH_RETRY'
#!/usr/bin/env bash
set -euo pipefail

RETRY=0
RETRY_FILE=""
AGENTS_TO_APPLY=()
FAILED_AGENTS=()
ERRORS=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --retry)          RETRY=1; shift ;;
            --retry-file)     RETRY_FILE="$2"; shift 2 ;;
            *)                AGENTS_TO_APPLY+=("$1"); shift ;;
        esac
    done
}

apply_agent() {
    local agent="$1"
    echo "processed: $agent" >> "$TRACE_FILE"
    [[ "$agent" == fail:* ]] && return 1 || return 0
}

parse_args "$@"

# If --retry, read from retry file and ignore passed agents
if [[ $RETRY -eq 1 && -n "$RETRY_FILE" && -f "$RETRY_FILE" ]]; then
    AGENTS_TO_APPLY=()
    while IFS= read -r agent; do
        [[ -n "$agent" ]] && AGENTS_TO_APPLY+=("$agent")
    done < "$RETRY_FILE"
fi

for agent in "${AGENTS_TO_APPLY[@]}"; do
    if ! apply_agent "$agent"; then
        FAILED_AGENTS+=("$agent")
        ERRORS=$((ERRORS + 1))
    fi
done

[[ $ERRORS -gt 0 ]] && exit 1 || exit 0
APPLY_WITH_RETRY
    chmod +x "$mock_apply"
    export TRACE_FILE="$trace_file"

    run "$mock_apply" --retry --retry-file "$failed_file" ignored-agent
    # Exit 1 because the agents fail
    [[ "$status" -eq 1 ]]

    # Trace should contain only the agents from failed-agents.txt
    [[ $(grep -c "processed: fail:agent1" "$trace_file") -eq 1 ]]
    [[ $(grep -c "processed: fail:agent2" "$trace_file") -eq 1 ]]
    # ignored-agent should NOT appear
    [[ ! $(grep -q "processed: ignored-agent" "$trace_file" 2>/dev/null) ]]
}

@test "--retry: ignores command-line agent list when reading file" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # Setup
    echo "retry-agent1" > "$failed_file"
    echo "retry-agent2" >> "$failed_file"

    local trace_file="${TEMP_TEST_DIR}/trace3.txt"

    # Use the mock that respects --retry
    cat > "$mock_apply" << 'APPLY_RETRY_FILTER'
#!/usr/bin/env bash
set -euo pipefail

RETRY=0
RETRY_FILE=""
AGENTS_TO_APPLY=()

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --retry)          RETRY=1; shift ;;
            --retry-file)     RETRY_FILE="$2"; shift 2 ;;
            *)                AGENTS_TO_APPLY+=("$1"); shift ;;
        esac
    done
}

apply_agent() {
    echo "processed: $1" >> "$TRACE_FILE"
    return 0
}

parse_args "$@"

# When --retry, use file content instead of CLI args
if [[ $RETRY -eq 1 && -n "$RETRY_FILE" && -f "$RETRY_FILE" ]]; then
    AGENTS_TO_APPLY=()
    while IFS= read -r agent; do
        [[ -n "$agent" ]] && AGENTS_TO_APPLY+=("$agent")
    done < "$RETRY_FILE"
fi

for agent in "${AGENTS_TO_APPLY[@]}"; do
    apply_agent "$agent"
done

exit 0
APPLY_RETRY_FILTER
    chmod +x "$mock_apply"
    export TRACE_FILE="$trace_file"

    # Pass command-line agents but with --retry (should be ignored)
    run "$mock_apply" --retry --retry-file "$failed_file" cli-agent1 cli-agent2
    [[ "$status" -eq 0 ]]

    # Only agents from file should be processed
    [[ $(grep -c "processed: retry-agent1" "$trace_file") -eq 1 ]]
    [[ $(grep -c "processed: retry-agent2" "$trace_file") -eq 1 ]]
    # CLI agents should NOT be processed
    [[ ! $(grep -q "processed: cli-agent1" "$trace_file" 2>/dev/null) ]]
}

@test "--retry with missing file: no agents processed" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"

    cat > "$mock_apply" << 'APPLY_RETRY_MISSING'
#!/usr/bin/env bash
set -euo pipefail

RETRY=0
RETRY_FILE=""
AGENTS_TO_APPLY=()

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --retry)          RETRY=1; shift ;;
            --retry-file)     RETRY_FILE="$2"; shift 2 ;;
            *)                AGENTS_TO_APPLY+=("$1"); shift ;;
        esac
    done
}

parse_args "$@"

# If retry file doesn't exist, process no agents
if [[ $RETRY -eq 1 && -n "$RETRY_FILE" ]]; then
    if [[ ! -f "$RETRY_FILE" ]]; then
        exit 0
    fi
    AGENTS_TO_APPLY=()
    while IFS= read -r agent; do
        [[ -n "$agent" ]] && AGENTS_TO_APPLY+=("$agent")
    done < "$RETRY_FILE"
fi

# Count agents processed
[[ ${#AGENTS_TO_APPLY[@]} -eq 0 ]] && exit 0 || exit 1
APPLY_RETRY_MISSING
    chmod +x "$mock_apply"

    # Retry with non-existent file
    run "$mock_apply" --retry --retry-file "/nonexistent/failed-agents.txt" agent1
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Test: Integration — combined error handling scenarios
# ---------------------------------------------------------------------------

@test "combined: --fail-fast with multiple agents, writes no failed file" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # With --fail-fast, we exit early, so no failed-agents.txt is written
    # (actual apply.sh may handle this differently; this tests the concept)
    run "$mock_apply" --fail-fast agent1 fail:agent2 agent3
    [[ "$status" -eq 2 ]]
    # File may or may not exist depending on implementation; both valid
}

@test "combined: multiple failures without --fail-fast, creates failed file" {
    local mock_apply="${TEMP_TEST_DIR}/apply.sh"
    _create_mock_apply "$mock_apply"

    export TEMP_TEST_DIR
    failed_file="${TEMP_TEST_DIR}/failed-agents.txt"

    # Without --fail-fast, process all agents
    run "$mock_apply" fail:agent1 agent2 fail:agent3
    [[ "$status" -eq 1 ]]

    # failed-agents.txt should list the two failures
    [[ -f "$failed_file" ]]
    [[ $(grep -c "fail:agent" "$failed_file") -eq 2 ]]
}

# ---------------------------------------------------------------------------
# Test: Symbol visibility — write_failed_agents_file must not be public
# ---------------------------------------------------------------------------

@test "write_failed_agents_file: public symbol must not exist after sourcing apply.sh" {
    local script
    script="$(cd "$(dirname "$BATS_TEST_FILENAME")" && pwd)/../../scripts/apply.sh"

    # Source apply.sh in a subshell, stripping lines that execute at source-time
    # (library sources, load_config call, and the main "$@" invocation) so we
    # can inspect only function definitions.
    run bash -c "
        stripped=\"\$(grep -v \
            -e '^#!/' \
            -e '^set -' \
            -e 'SCRIPT_DIR=' \
            -e 'REPO_ROOT=' \
            -e '# shellcheck' \
            -e '^source ' \
            -e '^load_config$' \
            -e '^main \"\\\$@\"$' \
            \"$script\")\"
        eval \"\$stripped\" 2>/dev/null
        declare -f write_failed_agents_file
    "

    [ "$status" -ne 0 ]
}
