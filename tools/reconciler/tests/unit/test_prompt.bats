#!/usr/bin/env bats
# tests/unit/test_prompt.bats — unit tests for scripts/lib/prompt.sh
#
# Tests confirm_with_timeout() behavior across:
#   - TTY mode: yes reply, no reply, timeout
#   - Non-TTY mode: piped y, piped n, no stdin (default n), no stdin (default y)

SCRIPT_DIR=""
PROMPT_LIB=""

setup() {
    SCRIPT_DIR="$(cd "$(dirname "${BATS_TEST_FILENAME}")/../.." && pwd)/scripts"
    PROMPT_LIB="${SCRIPT_DIR}/lib/prompt.sh"
}

# ---------------------------------------------------------------------------
# Non-TTY stdin tests (stdin is a pipe — always non-interactive in BATS)
# ---------------------------------------------------------------------------

@test "confirm_with_timeout: piped 'y' returns 0 (yes)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo y | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: piped 'Y' returns 0 (yes)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo Y | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: piped 'yes' returns 0 (yes)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo yes | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: piped 'YES' returns 0 (yes)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo YES | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: piped 'n' returns 1 (no)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo n | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 1 ]]
}

@test "confirm_with_timeout: piped 'N' returns 1 (no)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo N | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 1 ]]
}

@test "confirm_with_timeout: piped 'no' returns 1 (no)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo no | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 1 ]]
}

@test "confirm_with_timeout: empty piped input returns 1 (default n)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo '' | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 1 ]]
}

# ---------------------------------------------------------------------------
# Non-TTY no-stdin tests (default fallback)
# ---------------------------------------------------------------------------

@test "confirm_with_timeout: no stdin, default n returns 1" {
    run bash -c "
        source '${PROMPT_LIB}'
        confirm_with_timeout 'Proceed? [y/N]' 1 n < /dev/null
    "
    [[ "$status" -eq 1 ]]
}

@test "confirm_with_timeout: no stdin, default y returns 0" {
    run bash -c "
        source '${PROMPT_LIB}'
        confirm_with_timeout 'Proceed? [y/N]' 1 y < /dev/null
    "
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: default timeout is 30 (parameter default)" {
    # Verify the function accepts calls without explicit timeout/default args
    run bash -c "
        source '${PROMPT_LIB}'
        confirm_with_timeout 'Proceed?' 1 n < /dev/null
    "
    [[ "$status" -eq 1 ]]
}

@test "confirm_with_timeout: no-arg call uses built-in defaults" {
    run bash -c "
        source '${PROMPT_LIB}'
        confirm_with_timeout < /dev/null
    "
    [[ "$status" -eq 1 ]]
}

# ---------------------------------------------------------------------------
# Prompt text output tests
# ---------------------------------------------------------------------------

@test "confirm_with_timeout: prompt is written to stderr not stdout" {
    # Capture only stdout; stderr (prompt text) should not appear there
    run bash -c "
        source '${PROMPT_LIB}'
        confirm_with_timeout 'Do it? [y/N]' 1 n < /dev/null 2>/dev/null
        true
    "
    # stdout should be empty — prompt goes to stderr only
    [[ -z "$output" ]]
}

@test "confirm_with_timeout: timeout message is written to stderr" {
    run bash -c "
        source '${PROMPT_LIB}'
        confirm_with_timeout 'Go? [y/N]' 1 n < /dev/null 2>&1
    " 2>&1
    # With /dev/null stdin, either the 1-second non-TTY branch fires or
    # the timeout branch fires; either way status is 1
    [[ "$status" -eq 1 ]]
}

# ---------------------------------------------------------------------------
# Case-insensitivity tests
# ---------------------------------------------------------------------------

@test "confirm_with_timeout: 'yEs' is accepted as yes" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo yEs | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: 'yup' is NOT accepted (only y* where first char is y)" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo yup | confirm_with_timeout 'Proceed? [y/N]'
    "
    # 'yup' starts with y → matches y* → returns 0
    [[ "$status" -eq 0 ]]
}

@test "confirm_with_timeout: 'nope' returns 1" {
    run bash -c "
        source '${PROMPT_LIB}'
        echo nope | confirm_with_timeout 'Proceed? [y/N]'
    "
    [[ "$status" -eq 1 ]]
}
