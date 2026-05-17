#!/usr/bin/env bats
# tests/unit/test_check_docs_crossref.bats
#
# Issue #387: test coverage for scripts/check-docs-crossref.sh.
#
# The script asserts:
#   1. AGENTS.md exists at the repo root.
#   2. CLAUDE.md exists at the repo root.
#   3. CLAUDE.md contains a reference to AGENTS.md.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
CROSSREF_SCRIPT="${SCRIPT_DIR}/scripts/check-docs-crossref.sh"

TMP_DIR=""

setup() {
    TMP_DIR="${SCRIPT_DIR}/_crossref_test_$$_${RANDOM}"
    mkdir -p "$TMP_DIR"
}

teardown() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

# ---------------------------------------------------------------------------
# Helper: run the script with REPO_ROOT overridden to TMP_DIR by temporarily
# symlinking scripts/ into the temp dir and calling it from there.
# We use env var injection via a wrapper to avoid modifying the script itself.
# Instead, we copy the script and patch REPO_ROOT inline.
# ---------------------------------------------------------------------------

_run_check() {
    # Run the script with SCRIPT_DIR pointing to a scripts/ subdir of TMP_DIR
    # so that REPO_ROOT resolves to TMP_DIR.
    local tmp_scripts="${TMP_DIR}/scripts"
    mkdir -p "$tmp_scripts"
    cp "$CROSSREF_SCRIPT" "${tmp_scripts}/check-docs-crossref.sh"
    chmod +x "${tmp_scripts}/check-docs-crossref.sh"
    run bash "${tmp_scripts}/check-docs-crossref.sh"
}

# ---------------------------------------------------------------------------
# Test 1: Both files present with reference → exit 0
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: AGENTS.md present and CLAUDE.md has reference → exits 0" {
    echo "# AGENTS.md content" > "${TMP_DIR}/AGENTS.md"
    cat > "${TMP_DIR}/CLAUDE.md" <<'EOF'
# My Project

> For agent safety boundaries and permitted tool use, see [AGENTS.md](AGENTS.md).

Some other content here.
EOF

    _run_check
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASSED"* ]]
}

# ---------------------------------------------------------------------------
# Test 2: AGENTS.md missing → exit 1
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: AGENTS.md missing → exits 1" {
    cat > "${TMP_DIR}/CLAUDE.md" <<'EOF'
# My Project

> For agent safety boundaries and permitted tool use, see [AGENTS.md](AGENTS.md).
EOF
    # No AGENTS.md created

    _run_check
    [ "$status" -eq 1 ]
    [[ "$output" == *"AGENTS.md not found"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: CLAUDE.md missing reference → exit 1
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: CLAUDE.md has no reference to AGENTS.md → exits 1" {
    echo "# AGENTS.md content" > "${TMP_DIR}/AGENTS.md"
    cat > "${TMP_DIR}/CLAUDE.md" <<'EOF'
# My Project

Some content with no cross-reference to the safety doc.
EOF

    _run_check
    [ "$status" -eq 1 ]
    [[ "$output" == *"no reference to AGENTS.md"* ]]
}

# ---------------------------------------------------------------------------
# Test 4: CLAUDE.md missing entirely → exit 1
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: CLAUDE.md missing entirely → exits 1" {
    echo "# AGENTS.md content" > "${TMP_DIR}/AGENTS.md"
    # No CLAUDE.md created

    _run_check
    [ "$status" -eq 1 ]
    [[ "$output" == *"CLAUDE.md not found"* ]]
}

# ---------------------------------------------------------------------------
# Test 5: Both files missing → exit 1 with 2 violations
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: both AGENTS.md and CLAUDE.md missing → exits 1" {
    # Neither file created

    _run_check
    [ "$status" -eq 1 ]
    [[ "$output" == *"AGENTS.md not found"* ]]
    [[ "$output" == *"CLAUDE.md not found"* ]]
    [[ "$output" == *"2 violation(s)"* ]]
}

# ---------------------------------------------------------------------------
# Test 6: AGENTS.md absent but reference exists in CLAUDE.md → exit 1
# (AGENTS.md existence check runs first; both failures reported)
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: reference present in CLAUDE.md but AGENTS.md absent → exits 1" {
    cat > "${TMP_DIR}/CLAUDE.md" <<'EOF'
# My Project

> See [AGENTS.md](AGENTS.md) for safety boundaries.
EOF
    # No AGENTS.md created

    _run_check
    [ "$status" -eq 1 ]
    [[ "$output" == *"AGENTS.md not found"* ]]
}

# ---------------------------------------------------------------------------
# Test 7: Reference to AGENTS.md appears only in a code block → still passes
# (grep matches anywhere; benign false-positive is acceptable)
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: reference to AGENTS.md in code block still passes" {
    echo "# AGENTS.md" > "${TMP_DIR}/AGENTS.md"
    cat > "${TMP_DIR}/CLAUDE.md" <<'EOF'
# My Project

```
cat AGENTS.md
```
EOF

    _run_check
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 8: Violation count reported correctly for single violation
# ---------------------------------------------------------------------------

@test "check-docs-crossref.sh: single violation reports 1 violation" {
    echo "# AGENTS.md content" > "${TMP_DIR}/AGENTS.md"
    echo "# CLAUDE.md with no crossref" > "${TMP_DIR}/CLAUDE.md"

    _run_check
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 violation(s)"* ]]
}
