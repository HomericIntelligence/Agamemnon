#!/usr/bin/env bats
# tests/unit/test_apply_sc2154.bats — regression guard for issues #370 and #392
#
# Issue #370: ${repo_root} (lowercase, undefined) used instead of ${REPO_ROOT}
# in the effective_snapshot_dir fallback expression inside apply.sh::main().
# Under set -u this causes an unbound-variable abort; without set -u it writes
# snapshots to /.myrmidons/snapshots on the root filesystem.
#
# Issue #392: Wires shellcheck SC2154 (referenced-but-not-assigned) into the
# permanent lint entry points so the class of bug from #370 cannot reappear.
#
# These tests verify:
#   - REPO_ROOT is defined (non-empty) in apply.sh's global scope
#   - The snapshot_dir fallback references REPO_ROOT (correct case), not repo_root
#   - No lowercase alias repo_root is defined in apply.sh
#   - SC2154 is enabled in lint-shell.sh
#   - SC2154 is enabled in .pre-commit-config.yaml's shellcheck hook args

SCRIPT_DIR=""

setup() {
    SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
}

# ---------------------------------------------------------------------------
# #370 regression: REPO_ROOT casing
# ---------------------------------------------------------------------------

@test "apply.sh: REPO_ROOT is assigned at global scope" {
    grep -qE '^REPO_ROOT=' "${SCRIPT_DIR}/scripts/apply.sh"
}

@test "apply.sh: no lowercase repo_root reference exists" {
    # Any occurrence of \${repo_root} (lowercase) is a bug — the variable is REPO_ROOT.
    ! grep -qE '\$\{?repo_root\}?' "${SCRIPT_DIR}/scripts/apply.sh"
}

@test "apply.sh: effective_snapshot_dir fallback uses REPO_ROOT (uppercase)" {
    # The pattern must contain REPO_ROOT (not repo_root) in the snapshot dir expression.
    grep -qE 'effective_snapshot_dir=.*REPO_ROOT' "${SCRIPT_DIR}/scripts/apply.sh"
}

@test "apply.sh: MYRMIDONS_STATE_DIR uses REPO_ROOT (uppercase)" {
    grep -qE 'MYRMIDONS_STATE_DIR=.*REPO_ROOT' "${SCRIPT_DIR}/scripts/apply.sh"
}

# ---------------------------------------------------------------------------
# #392: SC2154 is wired into permanent lint entry points
# ---------------------------------------------------------------------------

@test "lint-shell.sh: SC2154 is enabled in shellcheck invocation" {
    grep -qF -- '--enable=SC2154' "${SCRIPT_DIR}/scripts/lint-shell.sh"
}

@test ".pre-commit-config.yaml: SC2154 is enabled in shellcheck hook args" {
    grep -qF -- '--enable=SC2154' "${SCRIPT_DIR}/.pre-commit-config.yaml"
}

@test "pixi.toml: SC2154 is enabled in lint env lint-shell task" {
    grep -qF -- '--enable=SC2154' "${SCRIPT_DIR}/pixi.toml"
}
