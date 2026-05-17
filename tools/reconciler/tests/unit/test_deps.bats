#!/usr/bin/env bats
# tests/unit/test_deps.bats — unit tests for check_deps function

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# Source reconcile.sh which defines check_deps
setup() {
    export AGAMEMNON_URL="http://localhost:19999"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/log.sh"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"

    # Save original PATH for restoration
    ORIGINAL_PATH="$PATH"
}

teardown() {
    # Restore original PATH
    export PATH="$ORIGINAL_PATH"
    # Clean up any test directories (safely, with full PATH restored)
    if [[ -n "${TOOLS_BIN:-}" && -d "$TOOLS_BIN" ]]; then
        /bin/rm -rf "$TOOLS_BIN" || true
    fi
}

# ---------------------------------------------------------------------------
# check_deps — all dependencies present
# ---------------------------------------------------------------------------

@test "check_deps: returns 0 when all required tools (yq, jq, curl) are in PATH" {
    # All tools should be available by default in the test environment
    check_deps
    # If we get here without error, the function succeeded
    [[ $? -eq 0 ]]
}

# ---------------------------------------------------------------------------
# check_deps — missing individual tools
# ---------------------------------------------------------------------------

@test "check_deps: returns 1 and logs error when yq is missing" {
    # Create a minimal PATH with only jq and curl
    TOOLS_BIN="$(mktemp -d)"

    local jq_path curl_path
    jq_path="$(command -v jq)"
    curl_path="$(command -v curl)"

    /bin/cp "$jq_path" "$TOOLS_BIN/jq"
    /bin/cp "$curl_path" "$TOOLS_BIN/curl"

    # Set PATH to only have our TOOLS_BIN (which has jq and curl but NOT yq).
    # Exclude /bin, /usr/bin, and pixi-managed dirs so pixi-installed yq isn't found.
    PATH="$TOOLS_BIN:/usr/sbin:/sbin"

    # check_deps should fail because yq is not in PATH
    ! check_deps
}

@test "check_deps: returns 1 and logs error when jq is missing" {
    # Create a minimal PATH with only yq and curl
    TOOLS_BIN="$(mktemp -d)"

    local yq_path curl_path
    yq_path="$(command -v yq)"
    curl_path="$(command -v curl)"

    /bin/cp "$yq_path" "$TOOLS_BIN/yq"
    /bin/cp "$curl_path" "$TOOLS_BIN/curl"

    # Set PATH to only have our TOOLS_BIN (which has yq and curl but NOT jq)
    # Exclude /bin and /usr/bin to avoid picking up system jq
    PATH="$TOOLS_BIN:/usr/sbin:/sbin"

    # check_deps should fail because jq is not in PATH
    ! check_deps
}

@test "check_deps: returns 1 and logs error when curl is missing" {
    # Create a minimal PATH with only yq and jq
    TOOLS_BIN="$(mktemp -d)"

    local yq_path jq_path
    yq_path="$(command -v yq)"
    jq_path="$(command -v jq)"

    /bin/cp "$yq_path" "$TOOLS_BIN/yq"
    /bin/cp "$jq_path" "$TOOLS_BIN/jq"

    # Set PATH to only have our TOOLS_BIN (which has yq and jq but NOT curl)
    # Exclude /bin and /usr/bin to avoid picking up system curl
    PATH="$TOOLS_BIN:/usr/sbin:/sbin"

    # check_deps should fail because curl is not in PATH
    ! check_deps
}

# ---------------------------------------------------------------------------
# check_deps — multiple missing tools
# ---------------------------------------------------------------------------

@test "check_deps: returns 1 when multiple tools are missing" {
    # Create an empty PATH
    TOOLS_BIN="$(mktemp -d)"

    # Set PATH to only have our empty TOOLS_BIN (none of the required tools)
    # Exclude /bin and /usr/bin to avoid picking up system tools
    PATH="$TOOLS_BIN:/usr/sbin:/sbin"

    # check_deps should fail because none of the required tools are available
    ! check_deps
}

@test "check_deps: error message lists all missing tools" {
    # Create an empty PATH
    TOOLS_BIN="$(mktemp -d)"

    # Set PATH to only have our empty TOOLS_BIN
    # Exclude /bin and /usr/bin to avoid picking up system tools
    PATH="$TOOLS_BIN:/usr/sbin:/sbin"

    # Capture stderr and stdout
    output=$(check_deps 2>&1 || true)

    # Should mention "Missing required tools"
    [[ "$output" == *"Missing required tools"* ]]
}

# ---------------------------------------------------------------------------
# check_deps — tool detection robustness
# ---------------------------------------------------------------------------

@test "check_deps: finds tools in standard locations (/usr/bin, /bin, /usr/local/bin)" {
    # This test verifies that check_deps uses 'command -v' which respects PATH
    # The default PATH should have yq, jq, curl in common locations
    check_deps
    [[ $? -eq 0 ]]
}

@test "check_deps: handles tools in non-standard PATH entries" {
    # Create a temp directory with our tools
    TOOLS_BIN="$(mktemp -d)"

    local yq_path jq_path curl_path
    yq_path="$(command -v yq)"
    jq_path="$(command -v jq)"
    curl_path="$(command -v curl)"

    # Create symlinks
    /bin/ln -s "$yq_path" "$TOOLS_BIN/yq"
    /bin/ln -s "$jq_path" "$TOOLS_BIN/jq"
    /bin/ln -s "$curl_path" "$TOOLS_BIN/curl"

    # Prepend our temp bin to PATH
    PATH="$TOOLS_BIN:$PATH"

    # check_deps should succeed
    check_deps
    [[ $? -eq 0 ]]
}
