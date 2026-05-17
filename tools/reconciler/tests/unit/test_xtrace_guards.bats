#!/usr/bin/env bats
# tests/unit/test_xtrace_guards.bats
#
# Issue #431: test coverage for check-xtrace-guards.sh.
#
# The linter statically flags shell scripts that expand ${AGAMEMNON_API_KEY}
# outside of a set +x / set -x guard block, preventing API key leakage in
# bash -x trace output.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
DETECTOR="${SCRIPT_DIR}/scripts/check-xtrace-guards.sh"

TMP_DIR=""

setup() {
    TMP_DIR="${SCRIPT_DIR}/_xtrace_test_$$_${RANDOM}"
    mkdir -p "$TMP_DIR"
}

teardown() {
    if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
        rm -rf "$TMP_DIR"
    fi
}

# ---------------------------------------------------------------------------
# Test 1: bare ${AGAMEMNON_API_KEY} in curl without guard → violation
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: bare expansion in curl exits 1" {
    local f="${TMP_DIR}/bare.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"AGAMEMNON_API_KEY expanded without xtrace guard"* ]]
}

# ---------------------------------------------------------------------------
# Test 2: bare $AGAMEMNON_API_KEY (no braces) without guard → violation
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: bare expansion without braces exits 1" {
    local f="${TMP_DIR}/bare-no-braces.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
curl -H "X-API-Key: $AGAMEMNON_API_KEY" http://example.com
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 1 ]
    [[ "$output" == *"AGAMEMNON_API_KEY expanded without xtrace guard"* ]]
}

# ---------------------------------------------------------------------------
# Test 3: expansion inside { set +x; } / set -x guard → clean
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: expansion inside brace guard exits 0" {
    local f="${TMP_DIR}/guarded.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
local _had_xtrace=0
if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
{ set +x; } 2>/dev/null
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com
if [[ $_had_xtrace -eq 1 ]]; then set -x; fi
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 4: expansion inside bare set +x / set -x guard → clean
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: expansion inside bare set +x guard exits 0" {
    local f="${TMP_DIR}/bare-guard.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
set +x
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com
set -x
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 5: suppression annotation → clean
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: suppressed with xtrace-lint: ok exits 0" {
    local f="${TMP_DIR}/suppressed.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com # xtrace-lint: ok
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 6: variable assignment (normalisation) → clean
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: AGAMEMNON_API_KEY= assignment exits 0" {
    local f="${TMP_DIR}/assign.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
AGAMEMNON_API_KEY="${AGAMEMNON_API_KEY:-}"
echo "ready"
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 7: comment-only reference → clean
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: comment-only reference exits 0" {
    local f="${TMP_DIR}/comment.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
# Use ${AGAMEMNON_API_KEY} for authentication
echo "hello"
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 8: violation after guard block ends → violation
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: expansion after guard block ends exits 1" {
    local f="${TMP_DIR}/post-guard.sh"
    cat > "$f" <<'SH'
#!/usr/bin/env bash
{ set +x; } 2>/dev/null
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com/guarded
if [[ $_had_xtrace -eq 1 ]]; then set -x; fi
curl -H "Authorization: Bearer ${AGAMEMNON_API_KEY}" http://example.com/unguarded
SH
    run bash "$DETECTOR" "$f"
    [ "$status" -eq 1 ]
}

# ---------------------------------------------------------------------------
# Test 9: scripts/lib/api.sh in repo passes the linter
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: repo api.sh exits 0" {
    run bash "$DETECTOR" "${SCRIPT_DIR}/scripts/lib/api.sh"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Test 10: default full-repo scan exits 0 (all expansions are guarded)
# ---------------------------------------------------------------------------

@test "check-xtrace-guards: full-repo default scan exits 0" {
    run bash "$DETECTOR"
    [ "$status" -eq 0 ]
}
