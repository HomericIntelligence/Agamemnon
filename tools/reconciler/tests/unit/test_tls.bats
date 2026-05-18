#!/usr/bin/env bats
# tests/unit/test_tls.bats — unit tests for TLS flag wiring in scripts/lib/api.sh
#
# Tests the _agamemnon_build_tls_flags() function and its integration with curl.
# Covers edge cases: no TLS vars, TLS_VERIFY=false warning, all vars set,
# and asymmetric cert/key configuration.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

setup() {
    # Unset all TLS-related variables before each test
    unset AGAMEMNON_TLS_VERIFY
    unset AGAMEMNON_CA_CERT
    unset AGAMEMNON_CLIENT_CERT
    unset AGAMEMNON_CLIENT_KEY
    unset AGAMEMNON_API_KEY

    # Set a default URL for testing
    export AGAMEMNON_URL="http://127.0.0.1:18080"

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/api.sh"
}

teardown() {
    unset AGAMEMNON_TLS_VERIFY
    unset AGAMEMNON_CA_CERT
    unset AGAMEMNON_CLIENT_CERT
    unset AGAMEMNON_CLIENT_KEY
    unset AGAMEMNON_API_KEY
}

# ---------------------------------------------------------------------------
# _agamemnon_build_tls_flags tests
# ---------------------------------------------------------------------------

@test "_agamemnon_build_tls_flags: produces empty array when no TLS vars are set" {
    _agamemnon_build_tls_flags
    # Array should be empty; length via ${#array[@]} should be 0
    [[ ${#_AGAMEMNON_TLS_FLAGS[@]} -eq 0 ]]
}

@test "_agamemnon_build_tls_flags: adds --insecure when AGAMEMNON_TLS_VERIFY=false" {
    export AGAMEMNON_TLS_VERIFY="false"
    _agamemnon_build_tls_flags
    # Check that --insecure is in the array
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --insecure "* ]]
}

@test "_agamemnon_build_tls_flags: emits warning to stderr when AGAMEMNON_TLS_VERIFY=false" {
    export AGAMEMNON_TLS_VERIFY="false"
    # Capture stderr during test setup
    run bash -c "source ${SCRIPT_DIR}/scripts/lib/api.sh 2>&1"
    [[ "$output" == *"WARNING"* ]]
    [[ "$output" == *"TLS verification is DISABLED"* ]]
}

@test "_agamemnon_build_tls_flags: adds --insecure when AGAMEMNON_TLS_VERIFY=0" {
    export AGAMEMNON_TLS_VERIFY="0"
    _agamemnon_build_tls_flags
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --insecure "* ]]
}

@test "_agamemnon_build_tls_flags: adds --cacert when AGAMEMNON_CA_CERT is set" {
    export AGAMEMNON_CA_CERT="/path/to/ca-bundle.pem"
    _agamemnon_build_tls_flags
    # Check for both --cacert and the path
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --cacert "* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *"/path/to/ca-bundle.pem"* ]]
}

@test "_agamemnon_build_tls_flags: adds --cert when AGAMEMNON_CLIENT_CERT is set" {
    export AGAMEMNON_CLIENT_CERT="/path/to/client-cert.pem"
    _agamemnon_build_tls_flags
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --cert "* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *"/path/to/client-cert.pem"* ]]
}

@test "_agamemnon_build_tls_flags: adds --key when AGAMEMNON_CLIENT_KEY is set" {
    export AGAMEMNON_CLIENT_KEY="/path/to/client-key.pem"
    _agamemnon_build_tls_flags
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --key "* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *"/path/to/client-key.pem"* ]]
}

@test "_agamemnon_build_tls_flags: includes all four flags when all TLS vars are set" {
    export AGAMEMNON_TLS_VERIFY="false"
    export AGAMEMNON_CA_CERT="/path/to/ca-bundle.pem"
    export AGAMEMNON_CLIENT_CERT="/path/to/client-cert.pem"
    export AGAMEMNON_CLIENT_KEY="/path/to/client-key.pem"
    _agamemnon_build_tls_flags
    # Verify all four flags are present
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --insecure "* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --cacert "* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --cert "* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --key "* ]]
    # Also check that the paths are preserved
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *"/path/to/ca-bundle.pem"* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *"/path/to/client-cert.pem"* ]]
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *"/path/to/client-key.pem"* ]]
}

@test "_agamemnon_build_tls_flags: handles CLIENT_CERT without CLIENT_KEY" {
    export AGAMEMNON_CLIENT_CERT="/path/to/client-cert.pem"
    # Deliberately not setting CLIENT_KEY
    _agamemnon_build_tls_flags
    # Should still have --cert even if KEY is missing
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --cert "* ]]
    # Should NOT have --key
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " != *" --key "* ]]
}

@test "_agamemnon_build_tls_flags: handles CLIENT_KEY without CLIENT_CERT" {
    export AGAMEMNON_CLIENT_KEY="/path/to/client-key.pem"
    # Deliberately not setting CLIENT_CERT
    _agamemnon_build_tls_flags
    # Should have --key even if CERT is missing
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " == *" --key "* ]]
    # Should NOT have --cert
    [[ " ${_AGAMEMNON_TLS_FLAGS[*]} " != *" --cert "* ]]
}
