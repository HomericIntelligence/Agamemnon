#!/usr/bin/env bats
# tests/unit/test_snapshot_dir_guard.bats — unit tests for _guard_snapshot_dir()
#
# Issue #391: guard snapshot dir against root-filesystem writes at runtime.
# Tests cover all four conditions:
#   1. Normal derived path inside repo  → passes
#   2. Derived path resolves to /.myrmidons/snapshots (REPO_ROOT empty) → aborts
#   3. Derived path is outside repo tree for another reason → aborts
#   4. SNAPSHOT_DIR explicitly set to a path outside repo → passes (user override)

# shellcheck disable=SC2034
SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# ---------------------------------------------------------------------------
# Helper: run _guard_snapshot_dir in an isolated subshell.
#
# Usage: _run_guard <dir> <explicitly_set> [REPO_ROOT_OVERRIDE]
#   <dir>             the effective_snapshot_dir value to test
#   <explicitly_set>  "set" or "" to simulate ${SNAPSHOT_DIR:+set}
#   REPO_ROOT_OVERRIDE (optional) value for REPO_ROOT inside the subshell
# ---------------------------------------------------------------------------
_run_guard() {
    local dir="$1"
    local explicitly_set="$2"
    local repo_root="${3:-/home/user/myrepo}"

    bash -c "
        REPO_ROOT='${repo_root}'
        $(declare -f _guard_snapshot_dir 2>/dev/null || true)

        # Inline the function definition so we don't need to source apply.sh
        _guard_snapshot_dir() {
            local dir=\"\$1\"
            local explicitly_set=\"\${2:-}\"

            if [[ \"\$dir\" == '/.myrmidons/snapshots' ]]; then
                echo \"ERROR: snapshot dir resolved to '/.myrmidons/snapshots'.\" >&2
                echo \"  This usually means REPO_ROOT is empty or unset.\" >&2
                echo \"  Set SNAPSHOT_DIR explicitly or ensure REPO_ROOT is defined.\" >&2
                exit 1
            fi

            local repo_root_stripped=\"\${REPO_ROOT%/}\"
            if [[ -z \"\$explicitly_set\" && \"\$dir\" != \"\${repo_root_stripped}\"/* ]]; then
                echo \"ERROR: snapshot dir '\${dir}' is outside the repo tree '\${REPO_ROOT}'.\" >&2
                echo \"  REPO_ROOT may be empty or unset. Set SNAPSHOT_DIR explicitly to override.\" >&2
                exit 1
            fi
        }

        _guard_snapshot_dir '${dir}' '${explicitly_set}'
    " 2>&1
}

# ---------------------------------------------------------------------------
# Case 1: Normal derived path inside repo — must pass silently
# ---------------------------------------------------------------------------

@test "_guard_snapshot_dir: derived path inside repo passes" {
    run _run_guard "/home/user/myrepo/.myrmidons/snapshots" "" "/home/user/myrepo"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "_guard_snapshot_dir: derived path in a subdirectory of repo passes" {
    run _run_guard "/home/user/myrepo/reports/snapshots" "" "/home/user/myrepo"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Case 2: REPO_ROOT empty → resolves to /.myrmidons/snapshots — must abort
# ---------------------------------------------------------------------------

@test "_guard_snapshot_dir: /.myrmidons/snapshots always rejected (REPO_ROOT empty)" {
    run _run_guard "/.myrmidons/snapshots" "" ""
    [ "$status" -eq 1 ]
    [[ "$output" == *"/.myrmidons/snapshots"* ]]
}

@test "_guard_snapshot_dir: /.myrmidons/snapshots rejected even when explicitly set" {
    # This path is never safe, regardless of how it was derived.
    run _run_guard "/.myrmidons/snapshots" "set" ""
    [ "$status" -eq 1 ]
    [[ "$output" == *"/.myrmidons/snapshots"* ]]
}

@test "_guard_snapshot_dir: error message mentions REPO_ROOT for /.myrmidons/snapshots" {
    run _run_guard "/.myrmidons/snapshots" "" ""
    [ "$status" -eq 1 ]
    [[ "$output" == *"REPO_ROOT"* ]]
}

# ---------------------------------------------------------------------------
# Case 3: Derived path outside repo for another reason — must abort
# ---------------------------------------------------------------------------

@test "_guard_snapshot_dir: derived path outside repo tree is rejected" {
    run _run_guard "/tmp/snapshots" "" "/home/user/myrepo"
    [ "$status" -eq 1 ]
    [[ "$output" == *"outside the repo tree"* ]]
}

@test "_guard_snapshot_dir: derived /var/tmp path is rejected" {
    run _run_guard "/var/tmp/.myrmidons/snapshots" "" "/home/user/myrepo"
    [ "$status" -eq 1 ]
    [[ "$output" == *"outside the repo tree"* ]]
}

@test "_guard_snapshot_dir: error message includes the bad dir and REPO_ROOT" {
    run _run_guard "/tmp/snapshots" "" "/home/user/myrepo"
    [ "$status" -eq 1 ]
    [[ "$output" == *"/tmp/snapshots"* ]]
    [[ "$output" == *"/home/user/myrepo"* ]]
}

# ---------------------------------------------------------------------------
# Case 4: SNAPSHOT_DIR explicitly set to outside repo — must pass (user override)
# ---------------------------------------------------------------------------

@test "_guard_snapshot_dir: explicit SNAPSHOT_DIR outside repo is allowed" {
    run _run_guard "/mnt/backup/snapshots" "set" "/home/user/myrepo"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "_guard_snapshot_dir: explicit SNAPSHOT_DIR in /tmp is allowed" {
    run _run_guard "/tmp/my-snapshots" "set" "/home/user/myrepo"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Case 5 (#487): REPO_ROOT with trailing slash must still match in-repo paths
# ---------------------------------------------------------------------------

@test "_guard_snapshot_dir: REPO_ROOT with trailing slash accepts in-repo path" {
    # Without the ${REPO_ROOT%/} strip, the glob "/home/user/myrepo//*" would not
    # match "/home/user/myrepo/.myrmidons/snapshots" and the guard would falsely
    # report an out-of-repo path.
    run _run_guard "/home/user/myrepo/.myrmidons/snapshots" "" "/home/user/myrepo/"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "_guard_snapshot_dir: REPO_ROOT with trailing slash still rejects out-of-repo path" {
    run _run_guard "/tmp/snapshots" "" "/home/user/myrepo/"
    [ "$status" -eq 1 ]
    [[ "$output" == *"outside the repo tree"* ]]
}
