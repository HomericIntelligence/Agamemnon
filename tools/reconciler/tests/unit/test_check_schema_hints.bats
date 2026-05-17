#!/usr/bin/env bats
bats_require_minimum_version 1.5.0
# tests/unit/test_check_schema_hints.bats
#
# Issue #467: backfill BATS unit tests for scripts/check-schema-hints.sh.
#
# Mirrors the cases in tests/test-check-schema-hints.sh so the hook is covered
# by the standard `bats test-unit` task and CI output is consistent.
#
# The script accepts explicit file arguments, so each test simply invokes the
# checker against a temporary fixture file — no REPO_ROOT override needed.

# shellcheck disable=SC2034
SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
CHECKER="${SCRIPT_DIR}/scripts/check-schema-hints.sh"

TMP_DIR=""

setup() {
    TMP_DIR="$(mktemp -d)"
}

teardown() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

# Helper: write content to a YAML file under TMP_DIR.
_write_yaml() {
    local name="$1"
    local content="$2"
    local path="${TMP_DIR}/${name}"
    mkdir -p "$(dirname "$path")"
    printf '%s' "$content" > "$path"
    printf '%s\n' "$path"
}

# ---------------------------------------------------------------------------
# Test: correct canonical hint on line 1 → exit 0
# ---------------------------------------------------------------------------

@test "check-schema-hints: correct canonical \$schema= hint exits 0" {
    file="$(_write_yaml correct.yaml '# yaml-language-server: $schema=../../schemas/agent-v1.schema.json
apiVersion: myrmidons/v1
kind: Agent
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "check-schema-hints: canonical hint with URL schema exits 0" {
    file="$(_write_yaml url.yaml '# yaml-language-server: $schema=https://example.com/agent-v1.schema.json
apiVersion: myrmidons/v1
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test: missing hint entirely → exit 1
# ---------------------------------------------------------------------------

@test "check-schema-hints: missing hint on line 1 exits 1 with diagnostic" {
    file="$(_write_yaml missing.yaml 'apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: test-agent
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing or malformed"* ]]
    [[ "$output" == *"yaml-language-server"* ]]
}

# ---------------------------------------------------------------------------
# Test: malformed — missing "$schema=" prefix entirely
# ---------------------------------------------------------------------------

@test "check-schema-hints: malformed hint missing \$schema= prefix exits 1" {
    file="$(_write_yaml malformed-no-prefix.yaml '# yaml-language-server: =../../schemas/agent-v1.schema.json
apiVersion: myrmidons/v1
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing or malformed"* ]]
}

# ---------------------------------------------------------------------------
# Test: malformed — "schema=" without leading dollar sign (the #69 bug)
# ---------------------------------------------------------------------------

@test "check-schema-hints: malformed hint schema= without dollar exits 1" {
    file="$(_write_yaml malformed-no-dollar.yaml '# yaml-language-server: schema=../../schemas/agent-v1.schema.json
apiVersion: myrmidons/v1
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing or malformed"* ]]
}

# ---------------------------------------------------------------------------
# Test: malformed — wrong case ($Schema= instead of $schema=)
# ---------------------------------------------------------------------------

@test "check-schema-hints: malformed hint with wrong case \$Schema= exits 1" {
    file="$(_write_yaml malformed-case.yaml '# yaml-language-server: $Schema=../../schemas/agent-v1.schema.json
apiVersion: myrmidons/v1
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# Test: suppression annotation on line 1 → exit 0
# ---------------------------------------------------------------------------

@test "check-schema-hints: # schema-hint-skip suppression exits 0" {
    file="$(_write_yaml suppressed.yaml '# schema-hint-skip: template file, schema path varies per deployment target
apiVersion: myrmidons/v1
kind: Agent
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Test: files under a _templates/ directory are skipped (no enforcement)
# ---------------------------------------------------------------------------

@test "check-schema-hints: file under _templates/ is skipped" {
    file="$(_write_yaml _templates/no-hint.yaml 'apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: template-agent
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Edge case: empty file → exit 1 (head -1 yields empty first line)
# ---------------------------------------------------------------------------

@test "check-schema-hints: empty file exits 1" {
    file="${TMP_DIR}/empty.yaml"
    : > "$file"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing or malformed"* ]]
}

# ---------------------------------------------------------------------------
# Edge case: file whose only content is a non-hint comment → exit 1
# ---------------------------------------------------------------------------

@test "check-schema-hints: comment-only file without hint exits 1" {
    file="$(_write_yaml comment-only.yaml '# Just a header comment, no schema hint here
# more comments
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing or malformed"* ]]
}

# ---------------------------------------------------------------------------
# Edge case: hint correct but not on line 1 → exit 1
# ---------------------------------------------------------------------------

@test "check-schema-hints: correct hint on line 2 (not line 1) exits 1" {
    file="$(_write_yaml hint-line-2.yaml '# header comment
# yaml-language-server: $schema=../../schemas/agent-v1.schema.json
apiVersion: myrmidons/v1
')"
    run bash "$CHECKER" "$file"
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# Non-existent file: silently skipped, exits 0
# ---------------------------------------------------------------------------

@test "check-schema-hints: non-existent file path is silently skipped" {
    run bash "$CHECKER" "${TMP_DIR}/does-not-exist.yaml"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Multi-file: violation count reflects all bad files
# ---------------------------------------------------------------------------

@test "check-schema-hints: multiple bad files reported with violation count" {
    good="$(_write_yaml good.yaml '# yaml-language-server: $schema=../../schemas/agent-v1.schema.json
apiVersion: myrmidons/v1
')"
    bad1="$(_write_yaml bad1.yaml 'apiVersion: myrmidons/v1
')"
    bad2="$(_write_yaml bad2.yaml '# yaml-language-server: schema=foo
apiVersion: myrmidons/v1
')"
    run bash "$CHECKER" "$good" "$bad1" "$bad2"
    [ "$status" -eq 1 ]
    [[ "$output" == *"2 violation(s)"* ]]
    [[ "$output" == *"bad1.yaml"* ]]
    [[ "$output" == *"bad2.yaml"* ]]
}

# ---------------------------------------------------------------------------
# Regression guard: real repo default scan exits 0
# ---------------------------------------------------------------------------

@test "check-schema-hints: real repo agents/ and fleets/ scan exits 0" {
    run bash "$CHECKER"
    [ "$status" -eq 0 ]
}
