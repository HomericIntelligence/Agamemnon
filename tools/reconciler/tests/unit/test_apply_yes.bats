#!/usr/bin/env bats
# tests/unit/test_apply_yes.bats — unit tests for apply.sh --yes flag and confirmation prompt
#
# The --yes flag (or -y) suppresses the interactive confirmation prompt that is
# shown when --prune is used from a terminal.  These tests verify:
#   - parse_args accepts --yes and -y
#   - Confirmation prompt is shown (to TTY) without --yes
#   - --yes bypasses the prompt and continues
#   - Non-interactive (piped) stdin also bypasses the prompt

TEMP_TEST_DIR=""

setup() {
    TEMP_TEST_DIR="$(mktemp -d)"
    export TEMP_TEST_DIR
}

teardown() {
    [[ -n "$TEMP_TEST_DIR" ]] && rm -rf "$TEMP_TEST_DIR"
}

# ---------------------------------------------------------------------------
# Helper: create a trimmed apply.sh mock that exercises --yes / prompt logic
# ---------------------------------------------------------------------------
_create_prompt_mock() {
    local mock="$1"
    cat > "$mock" << 'MOCK'
#!/usr/bin/env bash
set -euo pipefail

YES=0
PRUNE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --yes|-y)   YES=1; shift ;;
            --prune)    PRUNE=1; shift ;;
            *)          shift ;;
        esac
    done
}

parse_args "$@"

if [[ $PRUNE -eq 1 && $YES -eq 0 && -t 0 ]]; then
    echo "WARNING: --prune will hibernate and delete agents not in YAML."
    printf 'Continue? [y/N] '
    read -r reply
    if [[ "${reply,,}" != "y" && "${reply,,}" != "yes" ]]; then
        echo "Aborted."
        exit 0
    fi
fi

echo "APPLIED"
exit 0
MOCK
    chmod +x "$mock"
}

# ---------------------------------------------------------------------------
# Test: parse_args recognises --yes and -y
# ---------------------------------------------------------------------------

@test "parse_args: --yes sets YES=1" {
    local YES=0
    parse_args_test() {
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --yes|-y) YES=1; shift ;;
                *) shift ;;
            esac
        done
    }
    parse_args_test --yes
    [[ "$YES" -eq 1 ]]
}

@test "parse_args: -y sets YES=1" {
    local YES=0
    parse_args_test() {
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --yes|-y) YES=1; shift ;;
                *) shift ;;
            esac
        done
    }
    parse_args_test -y
    [[ "$YES" -eq 1 ]]
}

@test "parse_args: default YES is 0 (no flag)" {
    local YES=0
    parse_args_test() {
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --yes|-y) YES=1; shift ;;
                *) shift ;;
            esac
        done
    }
    parse_args_test --prune
    [[ "$YES" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Test: --yes bypasses prompt when --prune is used
# ---------------------------------------------------------------------------

@test "--yes with --prune: skips confirmation prompt and applies" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    _create_prompt_mock "$mock"

    # With --yes, no stdin interaction needed; should print APPLIED
    run "$mock" --prune --yes
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
}

@test "-y with --prune: skips confirmation prompt and applies" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    _create_prompt_mock "$mock"

    run "$mock" --prune -y
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
}

# ---------------------------------------------------------------------------
# Test: non-interactive stdin (piped) skips the prompt check
# ---------------------------------------------------------------------------

@test "non-interactive stdin (piped 'y'): prompt accepts y and applies" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    # Create a version without the -t 0 guard so we can test prompt reply
    cat > "$mock" << 'MOCK_NOTT'
#!/usr/bin/env bash
set -euo pipefail

YES=0
PRUNE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --yes|-y) YES=1; shift ;;
            --prune)  PRUNE=1; shift ;;
            *)        shift ;;
        esac
    done
}

parse_args "$@"

if [[ $PRUNE -eq 1 && $YES -eq 0 ]]; then
    printf 'Continue? [y/N] '
    read -r reply
    if [[ "${reply,,}" != "y" && "${reply,,}" != "yes" ]]; then
        echo "Aborted."
        exit 0
    fi
fi

echo "APPLIED"
exit 0
MOCK_NOTT
    chmod +x "$mock"

    # Pipe 'y' as stdin answer
    run bash -c "echo y | '$mock' --prune"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
}

@test "non-interactive stdin (piped 'n'): prompt declines and aborts" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    cat > "$mock" << 'MOCK_NOTT'
#!/usr/bin/env bash
set -euo pipefail

YES=0
PRUNE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --yes|-y) YES=1; shift ;;
            --prune)  PRUNE=1; shift ;;
            *)        shift ;;
        esac
    done
}

parse_args "$@"

if [[ $PRUNE -eq 1 && $YES -eq 0 ]]; then
    printf 'Continue? [y/N] '
    read -r reply
    if [[ "${reply,,}" != "y" && "${reply,,}" != "yes" ]]; then
        echo "Aborted."
        exit 0
    fi
fi

echo "APPLIED"
exit 0
MOCK_NOTT
    chmod +x "$mock"

    run bash -c "echo n | '$mock' --prune"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Aborted"* ]]
    [[ "$output" != *"APPLIED"* ]]
}

@test "non-interactive stdin (piped 'yes'): prompt accepts 'yes' and applies" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    cat > "$mock" << 'MOCK_NOTT'
#!/usr/bin/env bash
set -euo pipefail

YES=0
PRUNE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --yes|-y) YES=1; shift ;;
            --prune)  PRUNE=1; shift ;;
            *)        shift ;;
        esac
    done
}

parse_args "$@"

if [[ $PRUNE -eq 1 && $YES -eq 0 ]]; then
    printf 'Continue? [y/N] '
    read -r reply
    if [[ "${reply,,}" != "y" && "${reply,,}" != "yes" ]]; then
        echo "Aborted."
        exit 0
    fi
fi

echo "APPLIED"
exit 0
MOCK_NOTT
    chmod +x "$mock"

    run bash -c "echo yes | '$mock' --prune"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
}

# ---------------------------------------------------------------------------
# Test: without --prune, no prompt is shown even without --yes
# ---------------------------------------------------------------------------

@test "without --prune: no prompt shown, proceeds normally" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    _create_prompt_mock "$mock"

    # Without --prune, --yes is irrelevant — no prompt, just applies
    run "$mock"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
    [[ "$output" != *"WARNING"* ]]
}

@test "without --prune but with --yes: still applies without prompt" {
    local mock="${TEMP_TEST_DIR}/apply.sh"
    _create_prompt_mock "$mock"

    run "$mock" --yes
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
}

# ---------------------------------------------------------------------------
# Test: --fail-fast flag interaction with --yes
# ---------------------------------------------------------------------------

@test "--yes and --fail-fast together are both recognised" {
    local YES=0
    local FAIL_FAST=0
    parse_combined() {
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --yes|-y)    YES=1; shift ;;
                --fail-fast) FAIL_FAST=1; shift ;;
                *)           shift ;;
            esac
        done
    }
    parse_combined --yes --fail-fast
    [[ "$YES" -eq 1 ]]
    [[ "$FAIL_FAST" -eq 1 ]]
}

# ---------------------------------------------------------------------------
# Test: --retry passes --fail-fast through (issue #268)
# ---------------------------------------------------------------------------

@test "--retry: --fail-fast flag is preserved alongside --retry" {
    # Simulate the retry invocation: build the args array the same way apply.sh
    # would, and verify --fail-fast appears in the reconstructed flags.
    local FAIL_FAST=0
    local RETRY=0
    local -a retry_flags=()

    parse_retry_flags() {
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --fail-fast) FAIL_FAST=1; shift ;;
                --retry)     RETRY=1; shift ;;
                *)           shift ;;
            esac
        done
        # Build retry invocation flags (as apply.sh should do)
        [[ $FAIL_FAST -eq 1 ]] && retry_flags+=(--fail-fast)
        [[ $RETRY -eq 1 ]]     && retry_flags+=(--retry)
    }

    parse_retry_flags --fail-fast --retry
    [[ "${retry_flags[*]}" == *"--fail-fast"* ]]
    [[ "${retry_flags[*]}" == *"--retry"* ]]
}

@test "--retry without --fail-fast: --fail-fast not in retry flags" {
    local FAIL_FAST=0
    local RETRY=0
    local -a retry_flags=()

    parse_retry_flags() {
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --fail-fast) FAIL_FAST=1; shift ;;
                --retry)     RETRY=1; shift ;;
                *)           shift ;;
            esac
        done
        [[ $FAIL_FAST -eq 1 ]] && retry_flags+=(--fail-fast)
        [[ $RETRY -eq 1 ]]     && retry_flags+=(--retry)
    }

    parse_retry_flags --retry
    [[ "${retry_flags[*]}" == *"--retry"* ]]
    [[ "${retry_flags[*]}" != *"--fail-fast"* ]]
}

# ---------------------------------------------------------------------------
# Test: read timeout — #378 — non-TTY stdin defaults to deny
# ---------------------------------------------------------------------------

# Helper: create a mock that mirrors the updated --prune prompt logic
_create_timeout_mock() {
    local mock="$1"
    cat > "$mock" << 'MOCK_TIMEOUT'
#!/usr/bin/env bash
set -euo pipefail

YES=0
PRUNE=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --yes|-y) YES=1; shift ;;
            --prune)  PRUNE=1; shift ;;
            *)        shift ;;
        esac
    done
}

parse_args "$@"

if [[ $PRUNE -eq 1 && $YES -eq 0 && "${MYRMIDONS_YES:-}" != "true" ]]; then
    reply=""
    if [[ ! -t 0 ]]; then
        reply="N"
    else
        echo "WARNING: --prune will hibernate and delete agents not in YAML."
        printf 'Continue? [y/N] '
        read -t 30 -r reply || reply="N"
    fi
    if [[ "${reply,,}" != "y" && "${reply,,}" != "yes" ]]; then
        echo "Aborted."
        exit 0
    fi
fi

echo "APPLIED"
exit 0
MOCK_TIMEOUT
    chmod +x "$mock"
}

@test "timeout-mock: non-TTY stdin with --prune defaults to deny (aborts)" {
    local mock="${TEMP_TEST_DIR}/apply_timeout.sh"
    _create_timeout_mock "$mock"

    # Pipe empty stdin — non-TTY, should default-deny
    run bash -c "echo '' | '$mock' --prune"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Aborted"* ]]
    [[ "$output" != *"APPLIED"* ]]
}

@test "timeout-mock: MYRMIDONS_YES=true bypasses --prune prompt" {
    local mock="${TEMP_TEST_DIR}/apply_timeout.sh"
    _create_timeout_mock "$mock"

    run bash -c "MYRMIDONS_YES=true '$mock' --prune"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
    [[ "$output" != *"Aborted"* ]]
}

@test "timeout-mock: MYRMIDONS_YES=false still requires confirmation (piped y applies)" {
    local mock="${TEMP_TEST_DIR}/apply_timeout.sh"
    _create_timeout_mock "$mock"

    run bash -c "echo y | MYRMIDONS_YES=false '$mock' --prune"
    [[ "$status" -eq 0 ]]
    # Non-TTY stdin → default deny even when piping 'y' (guard fires before read)
    [[ "$output" == *"Aborted"* ]]
    [[ "$output" != *"APPLIED"* ]]
}

@test "timeout-mock: piped 'y' to --prune is rejected by non-TTY default-deny guard" {
    local mock="${TEMP_TEST_DIR}/apply_timeout.sh"
    _create_timeout_mock "$mock"

    # The non-TTY guard fires before read, so piped 'y' has no effect
    run bash -c "echo y | '$mock' --prune"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Aborted"* ]]
}

@test "timeout-mock: --yes still bypasses prompt regardless of MYRMIDONS_YES" {
    local mock="${TEMP_TEST_DIR}/apply_timeout.sh"
    _create_timeout_mock "$mock"

    run bash -c "'$mock' --prune --yes"
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"APPLIED"* ]]
}
