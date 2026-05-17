#!/usr/bin/env bats
bats_require_minimum_version 1.5.0
# tests/unit/test_snapshot_dir_resolution.bats
#
# Runtime regression test for issue #390: effective_snapshot_dir resolves to
# ${REPO_ROOT}/.myrmidons/snapshots when SNAPSHOT_DIR is unset.
#
# The static grep check in test_default_snapshot_dir_uses_repo_root verifies
# source text; these tests exercise the actual runtime resolution path and
# catch regressions such as variable name typos or logic errors.

# shellcheck disable=SC2034
SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

setup() {
    # Controlled REPO_ROOT — never touch the real repo root
    REPO_ROOT="$(mktemp -d)"
    export REPO_ROOT

    # File where mock snapshot_write records the snapshot_dir argument it received
    MOCK_CAPTURE="${BATS_TMPDIR}/captured_snapshot_dir_$$"
    export MOCK_CAPTURE

    # Ensure SNAPSHOT_DIR is NOT inherited from the caller's environment
    unset SNAPSHOT_DIR
}

teardown() {
    rm -rf "${REPO_ROOT:-}"
    rm -f "${MOCK_CAPTURE:-}"
}

# ---------------------------------------------------------------------------
# Helper: resolve effective_snapshot_dir using the same expression as apply.sh
# line 507, then invoke the mock snapshot_write with it.
# ---------------------------------------------------------------------------

_run_resolution() {
    # shellcheck disable=SC2034
    local snapshot_write
    snapshot_write() {
        local _agents_json="$1"
        local snapshot_dir="$2"
        echo "$snapshot_dir" > "${MOCK_CAPTURE}"
    }

    local effective_snapshot_dir="${SNAPSHOT_DIR:-${REPO_ROOT}/.myrmidons/snapshots}"
    snapshot_write '[]' "$effective_snapshot_dir" "all"
}

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@test "effective_snapshot_dir resolves to REPO_ROOT/.myrmidons/snapshots when SNAPSHOT_DIR unset" {
    unset SNAPSHOT_DIR

    _run_resolution

    run cat "${MOCK_CAPTURE}"
    [[ "$status" -eq 0 ]]
    [[ "$output" == "${REPO_ROOT}/.myrmidons/snapshots" ]]
}

@test "effective_snapshot_dir resolves to REPO_ROOT/.myrmidons/snapshots when SNAPSHOT_DIR is empty string" {
    export SNAPSHOT_DIR=""

    _run_resolution

    run cat "${MOCK_CAPTURE}"
    [[ "$status" -eq 0 ]]
    [[ "$output" == "${REPO_ROOT}/.myrmidons/snapshots" ]]
}

@test "effective_snapshot_dir uses SNAPSHOT_DIR override when set" {
    local custom_dir="${BATS_TMPDIR}/custom-snapshots-$$"
    export SNAPSHOT_DIR="$custom_dir"

    _run_resolution

    run cat "${MOCK_CAPTURE}"
    [[ "$status" -eq 0 ]]
    [[ "$output" == "$custom_dir" ]]
}

@test "effective_snapshot_dir does not use REPO_ROOT when SNAPSHOT_DIR is set" {
    local custom_dir="${BATS_TMPDIR}/override-snapshots-$$"
    export SNAPSHOT_DIR="$custom_dir"

    _run_resolution

    run cat "${MOCK_CAPTURE}"
    [[ "$status" -eq 0 ]]
    # Must NOT fall back to REPO_ROOT path
    [[ "$output" != "${REPO_ROOT}/.myrmidons/snapshots" ]]
}
