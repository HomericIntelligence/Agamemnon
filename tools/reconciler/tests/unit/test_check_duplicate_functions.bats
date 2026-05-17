#!/usr/bin/env bats
# tests/unit/test_check_duplicate_functions.bats
#
# Issue #410: lint guard for duplicate shell function definitions.
# Verifies scripts/check-duplicate-functions.sh behavior:
#   - clean file passes (exit 0)
#   - file with one duplicate fails (exit 1) with correct output
#   - file with two duplicates reports both
#   - suppression annotation skips the duplicate
#   - apply.sh and scripts/lib/report.sh pass today (regression guard)

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
CHECKER="${SCRIPT_DIR}/scripts/check-duplicate-functions.sh"

TEMP_DIR=""

setup() {
    TEMP_DIR="$(mktemp -d)"
    export TEMP_DIR
}

teardown() {
    [[ -n "$TEMP_DIR" ]] && rm -rf "$TEMP_DIR"
}

# ---------------------------------------------------------------------------
# Helper: write a shell fixture and run the checker against it
# ---------------------------------------------------------------------------

_write_fixture() {
    local name="$1"
    local content="$2"
    local path="${TEMP_DIR}/${name}.sh"
    printf '%s\n' "$content" > "$path"
    echo "$path"
}

# ---------------------------------------------------------------------------
# Test 1: clean file → exit 0, no output
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: clean script exits 0" {
    local f
    f="$(_write_fixture "clean" "#!/usr/bin/env bash
foo() { echo foo; }
bar() { echo bar; }
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Test 2: single duplicate → exit 1 with error message
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: single duplicate exits 1 and reports it" {
    local f
    f="$(_write_fixture "single_dup" "#!/usr/bin/env bash
foo() { echo first; }
bar() { echo bar; }
foo() { echo second; }
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"duplicate function definition 'foo'"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: two distinct duplicates → both reported
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: two duplicates both reported" {
    local f
    f="$(_write_fixture "two_dups" "#!/usr/bin/env bash
alpha() { echo a1; }
beta()  { echo b1; }
alpha() { echo a2; }
beta()  { echo b2; }
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"duplicate function definition 'alpha'"* ]]
    [[ "$output" == *"duplicate function definition 'beta'"* ]]
}

# ---------------------------------------------------------------------------
# Test 4: suppression annotation → exit 0
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: suppression annotation skips duplicate" {
    local f
    f="$(_write_fixture "suppressed" "#!/usr/bin/env bash
foo() { echo first; }
foo() { echo second; }  # allow-duplicate-function: if/else branch for TTY vs plain
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 5: duplicate without suppression on the second definition → exit 1
# (suppression only on the first definition does NOT count)
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: suppression only on first definition still fails" {
    local f
    f="$(_write_fixture "wrong_suppress" "#!/usr/bin/env bash
foo() { echo first; }  # allow-duplicate-function: this is on the wrong line
foo() { echo second; }
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"duplicate function definition 'foo'"* ]]
}

# ---------------------------------------------------------------------------
# Test 6: 'function' keyword syntax also detected
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: 'function' keyword syntax detected" {
    local f
    f="$(_write_fixture "func_kw" "#!/usr/bin/env bash
function my_func() { echo first; }
function my_func() { echo second; }
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"duplicate function definition 'my_func'"* ]]
}

# ---------------------------------------------------------------------------
# Test 7: 'function' keyword syntax with suppression → exit 0
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: 'function' keyword syntax with suppression passes" {
    local f
    f="$(_write_fixture "func_kw_supp" "#!/usr/bin/env bash
function my_func() { echo first; }
function my_func() { echo second; }  # allow-duplicate-function: conditional redefinition
")"
    run bash "$CHECKER" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 8: regression — apply.sh must pass (no real duplicates today)
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: apply.sh has no duplicate function definitions" {
    run bash "$CHECKER" "${SCRIPT_DIR}/scripts/apply.sh"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 9: regression — scripts/lib/report.sh must pass
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: scripts/lib/report.sh has no duplicate function definitions" {
    run bash "$CHECKER" "${SCRIPT_DIR}/scripts/lib/report.sh"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 10: regression — full scripts/ directory must pass (all scripts clean)
# ---------------------------------------------------------------------------

@test "check-duplicate-functions: entire scripts/ directory passes" {
    run bash "$CHECKER"
    [ "$status" -eq 0 ]
}
