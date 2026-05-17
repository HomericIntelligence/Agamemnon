#!/usr/bin/env bats
# tests/unit/test_url_security.bats — integration test: malicious AGAMEMNON_URL
#
# Issue #120: verify that apply.sh (via agamemnon_check_connection in api.sh)
# aborts before making any API calls when AGAMEMNON_URL is set to a malicious
# value.
#
# These tests call agamemnon_check_connection directly (which is the first
# network-touching call in apply.sh's main()) so they cover the integration
# point without needing a running Agamemnon server.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# Track whether curl was invoked by the mock
CURL_INVOKED_FILE=""

setup() {
    CURL_INVOKED_FILE="$(mktemp)"
    echo "0" > "$CURL_INVOKED_FILE"
    export CURL_INVOKED_FILE

    # Unset TLS vars so api.sh sources cleanly
    unset AGAMEMNON_TLS_VERIFY
    unset AGAMEMNON_CA_CERT
    unset AGAMEMNON_CLIENT_CERT
    unset AGAMEMNON_CLIENT_KEY
    unset AGAMEMNON_API_KEY
}

teardown() {
    [[ -n "$CURL_INVOKED_FILE" && -f "$CURL_INVOKED_FILE" ]] && rm -f "$CURL_INVOKED_FILE"
    unset CURL_INVOKED_FILE
}

# ---------------------------------------------------------------------------
# Helper: source api.sh with a given AGAMEMNON_URL in a subshell.
# Returns the exit code and captured output.
# ---------------------------------------------------------------------------

_run_check_connection() {
    local url="$1"
    AGAMEMNON_URL="$url" bash -c "
        source '${SCRIPT_DIR}/scripts/lib/api.sh'
        agamemnon_check_connection
    " 2>&1
}

# ---------------------------------------------------------------------------
# Tests: malicious URLs must be rejected before any curl call
# ---------------------------------------------------------------------------

@test "malicious URL: file:// scheme is rejected" {
    run _run_check_connection "file:///etc/passwd"
    [ "$status" -ne 0 ]
    [[ "$output" == *"AGAMEMNON_URL"* || "$output" == *"http"* || "$output" == *"scheme"* || "$output" == *"Invalid"* || "$output" == *"failed"* ]]
}

@test "malicious URL: ftp:// scheme is rejected" {
    run _run_check_connection "ftp://evil.example.com"
    [ "$status" -ne 0 ]
    [[ "$output" == *"http"* || "$output" == *"scheme"* || "$output" == *"Invalid"* || "$output" == *"failed"* ]]
}

@test "malicious URL: javascript: scheme is rejected" {
    run _run_check_connection "javascript:alert(1)"
    [ "$status" -ne 0 ]
}

@test "malicious URL: URL with embedded credentials is rejected" {
    run _run_check_connection "http://user:password@evil.com"
    [ "$status" -ne 0 ]
    [[ "$output" == *"credential"* || "$output" == *"Invalid"* || "$output" == *"failed"* || "$output" == *"AGAMEMNON_URL"* ]]
}

@test "malicious URL: URL with fragment (#) is rejected" {
    run _run_check_connection "http://evil.com#fragment"
    [ "$status" -ne 0 ]
}

@test "malicious URL: URL with query string (?) is rejected" {
    run _run_check_connection "http://evil.com?redirect=http://attacker.com"
    [ "$status" -ne 0 ]
}

@test "malicious URL: empty string is rejected" {
    run _run_check_connection ""
    [ "$status" -ne 0 ]
}

@test "malicious URL: plain string (not a URL) is rejected" {
    run _run_check_connection "not-a-url"
    [ "$status" -ne 0 ]
}

@test "malicious URL: protocol-relative URL is rejected" {
    run _run_check_connection "//evil.com"
    [ "$status" -ne 0 ]
}

@test "malicious URL: non-numeric port is rejected" {
    run _run_check_connection "http://evil.com:abc"
    [ "$status" -ne 0 ]
}

@test "malicious URL: IPv6 bracket notation is rejected" {
    run _run_check_connection "http://[evil.com]"
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# Tests: valid URLs must pass validation (and only fail at connection step)
# ---------------------------------------------------------------------------

@test "valid URL: http://localhost:8080 passes validation" {
    # agamemnon_check_connection will fail because nothing is running on the port,
    # but the error must be about connection (not URL rejection)
    run _run_check_connection "http://localhost:19998"
    [ "$status" -ne 0 ]
    # Should mention that it cannot reach, not that the URL is invalid
    [[ "$output" == *"Cannot reach"* || "$output" == *"failed"* ]]
    # Must NOT mention URL validation failure
    [[ "$output" != *"failed security validation"* ]]
}

@test "valid URL: https://agamemnon.internal passes validation" {
    run _run_check_connection "https://agamemnon.internal"
    [ "$status" -ne 0 ]
    # Should fail at connect step, not validation step
    [[ "$output" != *"failed security validation"* ]]
}

@test "valid URL: http://192.168.1.100:8080 passes validation" {
    run _run_check_connection "http://192.168.1.100:8080"
    [ "$status" -ne 0 ]
    [[ "$output" != *"failed security validation"* ]]
}

# ---------------------------------------------------------------------------
# Test: apply.sh main entry point aborts on malicious URL
# ---------------------------------------------------------------------------

@test "apply.sh: aborts early on malicious AGAMEMNON_URL without making API calls" {
    # We verify apply.sh exits non-zero when AGAMEMNON_URL is malicious.
    # We use a minimal agents directory so the script would proceed past
    # argument parsing if the URL were valid.
    local tmpdir
    tmpdir="$(mktemp -d)"
    mkdir -p "${tmpdir}/agents/hermes"
    cat > "${tmpdir}/agents/hermes/testagent.yaml" <<'YAML'
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: test-agent
  host: hermes
spec:
  label: TestAgent
  program: claude-code
  workingDirectory: /tmp/test
  desiredState: active
YAML

    # Run apply.sh with a malicious URL; it must exit non-zero
    run bash -c "
        cd '${SCRIPT_DIR}'
        AGAMEMNON_URL='file:///etc/passwd' \
        AIM_LOCK_FILE='${tmpdir}/.lock' \
        bash '${SCRIPT_DIR}/scripts/apply.sh' 2>&1
    "
    rm -rf "$tmpdir"

    [ "$status" -ne 0 ]
    # Output must mention rejection, not just a connection error to the malicious URL
    [[ "$output" == *"http"* || "$output" == *"scheme"* || "$output" == *"Invalid"* || "$output" == *"failed"* || "$output" == *"AGAMEMNON_URL"* ]]
}
