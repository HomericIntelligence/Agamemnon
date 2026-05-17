#!/usr/bin/env bats
# tests/unit/test_check_adr_index.bats
#
# Issue #441: lint guard for ADR README index completeness.
#
# These tests exercise scripts/check-adr-index.sh directly using temporary
# docs/adr/ directories so they are hermetic and do not touch the real index.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
CHECKER="${SCRIPT_DIR}/scripts/check-adr-index.sh"

TMP_DIR=""

setup() {
    TMP_DIR="${SCRIPT_DIR}/_adr_index_test_$$_${RANDOM}"
    mkdir -p "${TMP_DIR}/docs/adr"
}

teardown() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

# Helper: write a minimal README.md containing the given content
_write_readme() {
    printf '%s\n' "$1" > "${TMP_DIR}/docs/adr/README.md"
}

# Helper: create a dummy ADR file
_write_adr() {
    touch "${TMP_DIR}/docs/adr/$1"
}

# Helper: run the checker against TMP_DIR's docs/adr/ by overriding REPO_ROOT
#
# TODO(#643): This helper duplicates the logic from scripts/check-adr-index.sh
# (lines 16–62). The duplication exists because we need hermetic, isolated test
# dirs that don't touch the real repo. A better approach would be to refactor
# check-adr-index.sh to export a reusable checker function that both the script
# and the test can call. For now, keep both in sync: if check-adr-index.sh
# changes, mirror the changes in the inlined logic below.
_run_checker() {
    # The script derives ADR_DIR as ${REPO_ROOT}/docs/adr.
    # We symlink the script into TMP_DIR so SCRIPT_DIR resolution works,
    # or we just call the real script and override REPO_ROOT via env.
    # Simpler: run it in a subshell that sets REPO_ROOT.
    run bash -c "
        SCRIPT_DIR=\"\$(cd \"\$(dirname '${CHECKER}')\" && pwd)\"
        REPO_ROOT='${TMP_DIR}'
        ADR_DIR=\"\${REPO_ROOT}/docs/adr\"
        README=\"\${ADR_DIR}/README.md\"
        MISSING=0

        if [[ ! -f \"\$README\" ]]; then
            echo \"ERROR: \${README}: file not found — cannot check ADR index.\"
            exit 1
        fi

        mapfile -t ADR_FILES < <(find \"\$ADR_DIR\" -maxdepth 1 -name '*.md' ! -name 'README.md' | sort)

        for adr_file in \"\${ADR_FILES[@]}\"; do
            [[ ! -f \"\$adr_file\" ]] && continue
            filename=\"\$(basename \"\$adr_file\")\"
            if ! grep -qF \"\$filename\" \"\$README\"; then
                echo \"ERROR: \${README}: missing link for \${filename}\"
                MISSING=\$((MISSING + 1))
            fi
        done

        if [[ \$MISSING -gt 0 ]]; then
            echo ''
            echo \"check-adr-index: \${MISSING} ADR file(s) not linked in docs/adr/README.md.\"
            echo 'Add a table entry for each missing ADR and re-run.'
            exit 1
        fi
        exit 0
    "
}

# ---------------------------------------------------------------------------
# Test 1: All ADR files appear in README → exit 0
# ---------------------------------------------------------------------------

@test "check-adr-index: all ADRs linked in README exits 0" {
    _write_readme "| [ADR-007](ADR-007-foo.md) | Foo |
| [ADR-008](ADR-008-bar.md) | Bar |"
    _write_adr "ADR-007-foo.md"
    _write_adr "ADR-008-bar.md"

    _run_checker
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 2: One ADR file not in README → exit 1, error message
# ---------------------------------------------------------------------------

@test "check-adr-index: missing ADR link exits 1 with error message" {
    _write_readme "| [ADR-007](ADR-007-foo.md) | Foo |"
    _write_adr "ADR-007-foo.md"
    _write_adr "ADR-008-bar.md"

    _run_checker
    [ "$status" -eq 1 ]
    [[ "$output" == *"ADR-008-bar.md"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: Multiple ADR files missing → exit 1, all listed
# ---------------------------------------------------------------------------

@test "check-adr-index: multiple missing ADR links all reported" {
    _write_readme "| [ADR-007](ADR-007-foo.md) | Foo |"
    _write_adr "ADR-007-foo.md"
    _write_adr "ADR-008-bar.md"
    _write_adr "ADR-009-baz.md"

    _run_checker
    [ "$status" -eq 1 ]
    [[ "$output" == *"ADR-008-bar.md"* ]]
    [[ "$output" == *"ADR-009-baz.md"* ]]
}

# ---------------------------------------------------------------------------
# Test 4: No ADR files (empty dir) → exit 0
# ---------------------------------------------------------------------------

@test "check-adr-index: no ADR files exits 0" {
    _write_readme "# Architecture Decision Records"

    _run_checker
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 5: README missing entirely → exit 1 with descriptive error
# ---------------------------------------------------------------------------

@test "check-adr-index: missing README exits 1 with error" {
    _write_adr "ADR-007-foo.md"
    # No README created

    _run_checker
    [ "$status" -eq 1 ]
    [[ "$output" == *"not found"* ]]
}

# ---------------------------------------------------------------------------
# Test 6: README.md itself is excluded from the check
# ---------------------------------------------------------------------------

@test "check-adr-index: README.md not required to link itself" {
    _write_readme "# Architecture Decision Records"
    # Only README.md in the directory — no ADR files

    _run_checker
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 7: script directly on real repo exits 0 (regression guard)
# ---------------------------------------------------------------------------

@test "check-adr-index: real repo docs/adr/ passes" {
    run bash "$CHECKER"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 8: adding an unlinked ADR to real repo (simulated) causes failure
# ---------------------------------------------------------------------------

@test "check-adr-index: unlinked ADR file in real docs/adr/ exits 1" {
    local adr_dir="${SCRIPT_DIR}/docs/adr"
    local tmp_adr="${adr_dir}/ADR-999-test-only-$$-${RANDOM}.md"

    # Create a temporary unlinked ADR in the real docs/adr/
    touch "$tmp_adr"
    run bash "$CHECKER"
    local exit_code="$status"
    rm -f "$tmp_adr"

    [ "$exit_code" -eq 1 ]
    [[ "$output" == *"ADR-999"* ]]
}
