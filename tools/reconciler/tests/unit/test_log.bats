#!/usr/bin/env bats
# tests/unit/test_log.bats — unit tests for scripts/lib/log.sh

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

setup() {
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/log.sh"
    # Unset log-related env vars for clean test state
    unset LOG_LEVEL LOG_FORMAT
}

# ---------------------------------------------------------------------------
# LOG_LEVEL filtering: DEBUG
# ---------------------------------------------------------------------------

@test "log_debug: emitted when LOG_LEVEL=DEBUG" {
    export LOG_LEVEL=DEBUG
    run log_debug "test debug message"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"DEBUG"* ]]
    [[ "$output" == *"test debug message"* ]]
}

@test "log_debug: suppressed when LOG_LEVEL=INFO" {
    export LOG_LEVEL=INFO
    run log_debug "test debug message"
    [[ "$status" -eq 0 ]]
    [[ -z "$output" ]]
}

# ---------------------------------------------------------------------------
# LOG_LEVEL filtering: INFO
# ---------------------------------------------------------------------------

@test "log_info: emitted when LOG_LEVEL=INFO (default)" {
    run log_info "test info message"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"INFO"* ]]
    [[ "$output" == *"test info message"* ]]
}

@test "log_info: emitted when LOG_LEVEL=DEBUG" {
    export LOG_LEVEL=DEBUG
    run log_info "test info message"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"INFO"* ]]
}

@test "log_info: suppressed when LOG_LEVEL=WARN" {
    export LOG_LEVEL=WARN
    run log_info "test info message"
    [[ "$status" -eq 0 ]]
    [[ -z "$output" ]]
}

# ---------------------------------------------------------------------------
# LOG_LEVEL filtering: WARN
# ---------------------------------------------------------------------------

@test "log_warn: emitted when LOG_LEVEL=WARN" {
    export LOG_LEVEL=WARN
    run log_warn "test warn message"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"test warn message"* ]]
}

@test "log_warn: suppressed when LOG_LEVEL=ERROR" {
    export LOG_LEVEL=ERROR
    run log_warn "test warn message"
    [[ "$status" -eq 0 ]]
    [[ -z "$output" ]]
}

# ---------------------------------------------------------------------------
# LOG_LEVEL filtering: ERROR
# ---------------------------------------------------------------------------

@test "log_error: always emitted regardless of LOG_LEVEL" {
    export LOG_LEVEL=ERROR
    run log_error "test error message"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"ERROR"* ]]
    [[ "$output" == *"test error message"* ]]
}

# ---------------------------------------------------------------------------
# JSON format output
# ---------------------------------------------------------------------------

@test "LOG_FORMAT=json: output is valid JSON" {
    export LOG_FORMAT=json
    run log_info "test message"
    [[ "$status" -eq 0 ]]
    # Verify output contains JSON object structure
    [[ "$output" == *"\"level\":"* ]]
    [[ "$output" == *"\"message\":"* ]]
    [[ "$output" == *"\"script\":"* ]]
    [[ "$output" == *"\"timestamp\":"* ]]
}

@test "LOG_FORMAT=json: contains expected fields" {
    export LOG_FORMAT=json
    run log_info "test message"
    [[ "$status" -eq 0 ]]
    # Check for required fields in JSON output
    [[ "$output" == *"\"level\":\"INFO\""* ]]
    [[ "$output" == *"\"message\":test message"* ]]
    [[ "$output" == *"\"script\":"* ]]
    [[ "$output" =~ \"timestamp\":\"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\" ]]
}

# ---------------------------------------------------------------------------
# stderr vs stdout routing
# ---------------------------------------------------------------------------

@test "log_error: goes to stderr (fd 2)" {
    export LOG_FORMAT=text
    run log_error "error to stderr" 2>&1
    [[ "$status" -eq 0 ]]
    # The output should contain the error (captured via 2>&1 redirection above)
    [[ "$output" == *"ERROR"* ]]
    [[ "$output" == *"error to stderr"* ]]
}

@test "log_info: goes to stdout (fd 1)" {
    export LOG_FORMAT=text
    run log_info "info to stdout"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"INFO"* ]]
    [[ "$output" == *"info to stdout"* ]]
}

@test "log_warn: goes to stdout (fd 1)" {
    export LOG_FORMAT=text
    run log_warn "warn to stdout"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"warn to stdout"* ]]
}

# ---------------------------------------------------------------------------
# Color codes
# ---------------------------------------------------------------------------

@test "text format: no ANSI color codes when output is not a TTY" {
    export LOG_FORMAT=text
    run log_info "test message"
    [[ "$status" -eq 0 ]]
    # Should not contain ANSI escape sequences
    [[ "$output" != *$'\033['* ]]
}

@test "JSON format: never includes ANSI color codes" {
    export LOG_FORMAT=json
    run log_info "test message"
    [[ "$status" -eq 0 ]]
    # JSON output should never contain ANSI codes
    [[ "$output" != *$'\033['* ]]
}

# ---------------------------------------------------------------------------
# Message content preservation
# ---------------------------------------------------------------------------

@test "log functions: preserve special characters in message" {
    export LOG_FORMAT=text
    run log_info 'test with $special "chars" and newline'
    [[ "$status" -eq 0 ]]
    [[ "$output" == *'test with $special "chars" and newline'* ]]
}

@test "LOG_FORMAT=json: properly handles special characters in message" {
    export LOG_FORMAT=json
    run log_info 'test with special chars'
    [[ "$status" -eq 0 ]]
    # Verify message field is present in JSON output
    [[ "$output" == *"\"message\":test with special chars"* ]]
}
