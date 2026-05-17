#!/usr/bin/env bats
# tests/unit/test_record_failure.bats — unit tests for record_failure() and print_error_summary()
#
# Tests verify the FAILED_AGENTS_INFO structured-string approach introduced in #394:
# - record_failure() appends "name\x01http_status\x01message" entries
# - print_error_summary() parses and formats those entries correctly

# ---------------------------------------------------------------------------
# Helper: source record_failure and print_error_summary from apply.sh
# We extract only the two functions plus the required globals to avoid
# sourcing the entire script (which has side effects).
# ---------------------------------------------------------------------------

_load_functions() {
    # Variables that apply.sh expects to exist
    ERRORS=0
    FAILED_AGENTS_INFO=()
    # shellcheck disable=SC2034
    FAILED_AGENTS_FILE="/tmp/failed-agents.txt"

    # Source only the two functions via eval (extract between markers)
    # shellcheck disable=SC1090
    eval "$(sed -n '/^record_failure()/,/^}/p' "${BATS_TEST_DIRNAME}/../../scripts/apply.sh")"
    # shellcheck disable=SC1090
    eval "$(sed -n '/^print_error_summary()/,/^}/p' "${BATS_TEST_DIRNAME}/../../scripts/apply.sh")"
}

setup() {
    _load_functions
}

# ---------------------------------------------------------------------------
# record_failure — array population
# ---------------------------------------------------------------------------

@test "record_failure: FAILED_AGENTS_INFO starts empty" {
    [[ "${#FAILED_AGENTS_INFO[@]}" -eq 0 ]]
}

@test "record_failure: increments ERRORS" {
    record_failure "agent-a" "404" "not found"
    [[ "$ERRORS" -eq 1 ]]
}

@test "record_failure: appends one entry to FAILED_AGENTS_INFO" {
    record_failure "agent-a" "404" "not found"
    [[ "${#FAILED_AGENTS_INFO[@]}" -eq 1 ]]
}

@test "record_failure: entry contains name, status, and message" {
    record_failure "agent-a" "404" "not found"
    local entry="${FAILED_AGENTS_INFO[0]}"
    local sep=$'\x01'
    IFS="$sep" read -r name status msg <<< "$entry"
    [[ "$name"   == "agent-a"   ]]
    [[ "$status" == "404"       ]]
    [[ "$msg"    == "not found" ]]
}

@test "record_failure: multiple calls produce multiple entries in order" {
    record_failure "agent-a" "404" "not found"
    record_failure "agent-b" "500" "server error"
    [[ "${#FAILED_AGENTS_INFO[@]}" -eq 2 ]]
    local sep=$'\x01'
    IFS="$sep" read -r n1 s1 m1 <<< "${FAILED_AGENTS_INFO[0]}"
    IFS="$sep" read -r n2 s2 m2 <<< "${FAILED_AGENTS_INFO[1]}"
    [[ "$n1" == "agent-a" ]] && [[ "$s1" == "404" ]] && [[ "$m1" == "not found" ]]
    [[ "$n2" == "agent-b" ]] && [[ "$s2" == "500" ]] && [[ "$m2" == "server error" ]]
}

@test "record_failure: ERRORS accumulates across multiple calls" {
    record_failure "agent-a" "404" "not found"
    record_failure "agent-b" "500" "server error"
    record_failure "agent-c" "503" "unavailable"
    [[ "$ERRORS" -eq 3 ]]
}

@test "record_failure: empty http_status stored and retrievable" {
    record_failure "agent-b" "" "timeout"
    local sep=$'\x01'
    IFS="$sep" read -r name status msg <<< "${FAILED_AGENTS_INFO[0]}"
    [[ "$name"   == "agent-b" ]]
    [[ "$status" == ""        ]]
    [[ "$msg"    == "timeout" ]]
}

@test "record_failure: empty error_message stored and retrievable" {
    record_failure "agent-c" "500" ""
    local sep=$'\x01'
    IFS="$sep" read -r name status msg <<< "${FAILED_AGENTS_INFO[0]}"
    [[ "$name"   == "agent-c" ]]
    [[ "$status" == "500"     ]]
    [[ "$msg"    == ""        ]]
}

@test "record_failure: message with pipe character is stored safely" {
    record_failure "agent-d" "500" "Failed to update fields [label|program]"
    local sep=$'\x01'
    IFS="$sep" read -r name status msg <<< "${FAILED_AGENTS_INFO[0]}"
    [[ "$name"   == "agent-d"                                    ]]
    [[ "$status" == "500"                                        ]]
    [[ "$msg"    == "Failed to update fields [label|program]"    ]]
}

# ---------------------------------------------------------------------------
# print_error_summary — output formatting
# ---------------------------------------------------------------------------

@test "print_error_summary: prints FAIL tag for single failure" {
    record_failure "agent-a" "404" "not found"
    ERRORS=1
    run print_error_summary
    [[ "${output}" == *"[FAIL] agent-a"* ]]
}

@test "print_error_summary: includes HTTP status line when present" {
    record_failure "agent-a" "404" "not found"
    ERRORS=1
    run print_error_summary
    [[ "${output}" == *"HTTP status: 404"* ]]
}

@test "print_error_summary: includes error message line when present" {
    record_failure "agent-a" "404" "not found"
    ERRORS=1
    run print_error_summary
    [[ "${output}" == *"Error: not found"* ]]
}

@test "print_error_summary: omits HTTP status line when empty" {
    record_failure "agent-b" "" "timeout"
    ERRORS=1
    run print_error_summary
    [[ "${output}" != *"HTTP status:"* ]]
}

@test "print_error_summary: omits error line when message is empty" {
    record_failure "agent-c" "500" ""
    ERRORS=1
    run print_error_summary
    [[ "${output}" != *"Error:"* ]]
}

@test "print_error_summary: prints all failures for multiple entries" {
    record_failure "agent-a" "404" "not found"
    record_failure "agent-b" "500" "server error"
    ERRORS=2
    run print_error_summary
    [[ "${output}" == *"[FAIL] agent-a"* ]]
    [[ "${output}" == *"[FAIL] agent-b"* ]]
}

@test "print_error_summary: prints failures in insertion order" {
    record_failure "first-agent"  "400" "bad request"
    record_failure "second-agent" "500" "server error"
    ERRORS=2
    run print_error_summary
    local first_pos second_pos
    first_pos=$(echo "$output" | grep -n "first-agent"  | head -1 | cut -d: -f1)
    second_pos=$(echo "$output" | grep -n "second-agent" | head -1 | cut -d: -f1)
    [[ "$first_pos" -lt "$second_pos" ]]
}

@test "print_error_summary: FAILED AGENTS count matches ERRORS" {
    record_failure "agent-a" "404" "not found"
    record_failure "agent-b" "500" "server error"
    ERRORS=2
    run print_error_summary
    [[ "${output}" == *"FAILED AGENTS (2)"* ]]
}

@test "print_error_summary: message with pipe character renders correctly" {
    record_failure "agent-d" "500" "Failed to update fields [label|program]"
    ERRORS=1
    run print_error_summary
    [[ "${output}" == *"Error: Failed to update fields [label|program]"* ]]
}
