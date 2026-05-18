#!/usr/bin/env bats
# tests/unit/test_xtrace_exposure_lint.bats
#
# Issue #430: test coverage for check-xtrace-exposure.sh.
#
# The lint script scans shell scripts for curl invocations that expand
# ${AGAMEMNON_API_KEY} or ${_AUTH_HEADERS...} without a { set +x; } xtrace guard.
# These tests verify it catches violating files and passes clean ones.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
LINT_SCRIPT="${SCRIPT_DIR}/scripts/check-xtrace-exposure.sh"

TMP_DIR=""

setup() {
    TMP_DIR="$(mktemp -d "${SCRIPT_DIR}/_xtrace_lint_test_$$_XXXXXX")"
}

teardown() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

# ---------------------------------------------------------------------------
# Test 1: file with inline ${AGAMEMNON_API_KEY} in a curl call is flagged
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: flags curl with inline AGAMEMNON_API_KEY" {
    local f="${TMP_DIR}/bad_key.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://localhost/v1/agents
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"xtrace guard"* || "$output" == *"violation"* ]]
}

# ---------------------------------------------------------------------------
# Test 2: file with inline ${_AUTH_HEADERS[@]} in a curl call is flagged
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: flags curl with inline _AUTH_HEADERS expansion" {
    local f="${TMP_DIR}/bad_headers.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
curl "${_AUTH_HEADERS[@]}" http://localhost/v1/agents
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"xtrace guard"* || "$output" == *"violation"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: a properly guarded curl (with { set +x; } on same line) is not flagged
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: accepts curl with set+x guard on same line" {
    local f="${TMP_DIR}/guarded.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
{ set +x; } 2>/dev/null; curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://localhost/v1/agents
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 4: comment lines mentioning curl and ${AGAMEMNON_API_KEY} are not flagged
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: ignores comment lines" {
    local f="${TMP_DIR}/comment.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
# curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com
echo "safe"
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 5: file with no curl calls passes
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: passes file with no curl calls" {
    local f="${TMP_DIR}/no_curl.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
echo "hello world"
AGAMEMNON_API_KEY=test
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 6: file with curl but no auth expansion passes
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: passes curl without auth variable expansion" {
    local f="${TMP_DIR}/safe_curl.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
curl -s http://localhost/v1/health
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 7: multiple violations in one file are all reported
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: reports multiple violations in one file" {
    local f="${TMP_DIR}/multi.sh"
    cat > "$f" <<'EOF'
#!/usr/bin/env bash
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://localhost/v1/health
curl "${_AUTH_HEADERS[@]}" http://localhost/v1/agents
EOF
    run bash "$LINT_SCRIPT" "$f"
    [ "$status" -eq 1 ]
    # Should report 2 violations
    [[ "$output" == *"2 violation"* ]]
}

# ---------------------------------------------------------------------------
# Test 8: clean repo scripts pass the lint check
# ---------------------------------------------------------------------------

@test "check-xtrace-exposure: all repo scripts pass after fix" {
    run bash "$LINT_SCRIPT"
    [ "$status" -eq 0 ]
}
