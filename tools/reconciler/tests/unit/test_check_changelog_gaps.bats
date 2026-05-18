#!/usr/bin/env bats
# tests/unit/test_check_changelog_gaps.bats
#
# Issue #451: unit tests for scripts/check-changelog-gaps.py
#
# Each test creates a minimal temp git repo with controlled commits and a
# controlled CHANGELOG.md, then runs the script and asserts exit code + output.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
CHECK_SCRIPT="${SCRIPT_DIR}/scripts/check-changelog-gaps.py"
TMP_REPO=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_init_repo() {
    TMP_REPO="${SCRIPT_DIR}/_changelog_test_$$_${RANDOM}"
    mkdir -p "$TMP_REPO"
    git -C "$TMP_REPO" init -q
    git -C "$TMP_REPO" config user.email "test@example.com"
    git -C "$TMP_REPO" config user.name "Test"
    git -C "$TMP_REPO" config commit.gpgsign false
    git -C "$TMP_REPO" config tag.gpgSign false
    # Initial commit + tag so we have a base
    touch "${TMP_REPO}/.gitkeep"
    git -C "$TMP_REPO" add .
    git -C "$TMP_REPO" commit -q --no-verify -m "chore: initial commit"
    git -C "$TMP_REPO" tag v0.1.0
}

_add_commit() {
    local msg="$1"
    echo "$msg" >> "${TMP_REPO}/file.txt"
    git -C "$TMP_REPO" add file.txt
    git -C "$TMP_REPO" commit -q --no-verify -m "$msg"
}

_write_changelog() {
    local body="$1"
    cat > "${TMP_REPO}/CHANGELOG.md" <<EOF
# Changelog

## [Unreleased]

${body}

## [0.1.0] - 2026-01-01

### Added
- Initial release
EOF
}

_run_script() {
    (cd "$TMP_REPO" && python3 "$CHECK_SCRIPT" "$@") 2>&1
}

# ---------------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------------

setup() {
    _init_repo
}

teardown() {
    if [[ -n "$TMP_REPO" && -d "$TMP_REPO" ]]; then
        rm -rf "$TMP_REPO"
    fi
}

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@test "exit 0 when no qualifying commits since tag" {
    # Only a chore commit — not feat/fix/docs
    _add_commit "chore: update README"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "exit 0 when feat commit hash appears in [Unreleased]" {
    _add_commit "feat: add shiny new feature"
    local hash
    hash=$(git -C "$TMP_REPO" log -1 --format="%h")
    _write_changelog "### Added
- add shiny new feature (${hash})"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "exit 0 when feat commit description fragment appears in [Unreleased]" {
    _add_commit "feat: implement webhook support"
    _write_changelog "### Added
- implement webhook support for notifications"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "exit 1 when feat commit has no [Unreleased] entry" {
    _add_commit "feat: add missing feature"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 1 ]
    [[ "$output" == *"add missing feature"* ]]
}

@test "exit 1 when fix commit has no [Unreleased] entry" {
    _add_commit "fix: correct null pointer dereference"
    _write_changelog "### Fixed"

    run _run_script
    [ "$status" -eq 1 ]
    [[ "$output" == *"correct null pointer"* ]]
}

@test "exit 1 when docs commit has no [Unreleased] entry" {
    _add_commit "docs: update API reference"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 1 ]
    [[ "$output" == *"update api reference"* || "$output" == *"update API reference"* ]]
}

@test "exit 0 when [skip changelog] in commit subject" {
    _add_commit "feat: hidden feature [skip changelog]"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "exit 0 when merge commits are excluded (no-merges flag)" {
    # Merge commits are filtered by --no-merges in the script; this test
    # verifies a commit message with 'Merge' is not manually checked
    _add_commit "chore: Merge branch feature into main"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "exit 0 for feat with scope when entry present" {
    _add_commit "feat(core): add retry logic"
    _write_changelog "### Added
- add retry logic for core operations"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "exit 1 for feat with scope when entry missing" {
    _add_commit "feat(auth): add OAuth2 support"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 1 ]
    [[ "$output" == *"add oauth2 support"* || "$output" == *"add OAuth2 support"* ]]
}

@test "exit 0 with no release tag — warns and skips" {
    # Delete the v0.1.0 tag so no tags exist
    git -C "$TMP_REPO" tag -d v0.1.0 > /dev/null 2>&1
    _add_commit "feat: untagged feature"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 0 ]
    [[ "$output" == *"no release tag"* || "$output" == *"WARNING"* ]]
}

@test "exit 1 when multiple gaps found — all listed" {
    _add_commit "feat: feature alpha"
    _add_commit "fix: bug beta"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 1 ]
    [[ "$output" == *"feature alpha"* ]]
    [[ "$output" == *"bug beta"* ]]
}

@test "exit 0 when only refactor commits — not in CHANGELOG_TYPES" {
    _add_commit "refactor: extract helper module"
    _add_commit "ci: add parallel job"
    _add_commit "test: add unit tests for parser"
    _write_changelog "### Added"

    run _run_script
    [ "$status" -eq 0 ]
}

@test "--since flag overrides tag detection" {
    # Add a second tag, then a feat commit after it
    git -C "$TMP_REPO" tag v0.2.0
    _add_commit "feat: post-v0.2.0 feature"
    _write_changelog "### Added
- post-v0.2.0 feature enhancements"

    # With --since v0.2.0, script checks only commits after v0.2.0
    run _run_script --since v0.2.0
    [ "$status" -eq 0 ]
}

@test "--changelog flag points to custom path" {
    _add_commit "feat: use custom changelog path"
    local custom_cl="${TMP_REPO}/docs/CHANGES.md"
    mkdir -p "${TMP_REPO}/docs"
    cat > "$custom_cl" <<EOF
# Changelog

## [Unreleased]

### Added
- use custom changelog path feature

## [0.1.0] - 2026-01-01
EOF

    run _run_script --changelog "$custom_cl"
    [ "$status" -eq 0 ]
}

@test "exit 0 when all qualifying commits are covered" {
    _add_commit "feat: full coverage feature"
    _add_commit "fix: full coverage fix"
    _add_commit "docs: full coverage docs update"
    local hash1 hash2 hash3
    hash1=$(git -C "$TMP_REPO" log --format="%h" | sed -n '3p')
    hash2=$(git -C "$TMP_REPO" log --format="%h" | sed -n '2p')
    hash3=$(git -C "$TMP_REPO" log --format="%h" | sed -n '1p')
    _write_changelog "### Added
- full coverage feature (${hash1})
### Fixed
- full coverage fix (${hash2})
### Changed
- full coverage docs update (${hash3})"

    run _run_script
    [ "$status" -eq 0 ]
}
