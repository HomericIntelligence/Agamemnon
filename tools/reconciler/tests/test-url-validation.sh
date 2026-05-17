#!/usr/bin/env bash
# tests/test-url-validation.sh — Unit tests for AGAMEMNON_URL validation
#
# Tests the _agamemnon_validate_url function added to scripts/lib/api.sh
# to prevent SSRF via environment variable injection (issue #20).
#
# Usage:
#   ./tests/test-url-validation.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Source only the validation function — avoid side effects from set -e on api.sh
# by extracting and evaluating just the function definition.
source "${REPO_ROOT}/scripts/lib/api.sh"

PASS=0
FAIL=0

assert_valid() {
    local url="$1"
    if _agamemnon_validate_url "$url" 2>/dev/null; then
        echo "  PASS (valid): ${url}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL (expected valid, got rejected): ${url}"
        FAIL=$((FAIL + 1))
    fi
}

assert_invalid() {
    local url="$1"
    if ! _agamemnon_validate_url "$url" 2>/dev/null; then
        echo "  PASS (rejected): ${url}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL (expected rejection, got accepted): ${url}"
        FAIL=$((FAIL + 1))
    fi
}

echo "Testing _agamemnon_validate_url..."
echo ""

echo "=== Valid URLs (should be accepted) ==="
assert_valid "http://localhost:8080"
assert_valid "http://localhost:23000"
assert_valid "https://agamemnon.internal"
assert_valid "http://192.168.1.100:8080"
assert_valid "https://agamemnon.example.com:443"
assert_valid "http://agamemnon-host.lan:8080"
assert_valid "http://agamemnon.internal/api"
assert_valid "https://my-host.example.com:9000/v1"

echo ""
echo "=== Invalid / malicious URLs (should be rejected) ==="
assert_invalid "ftp://evil.example.com"
assert_invalid "file:///etc/passwd"
assert_invalid "http://user:password@evil.com"
assert_invalid "http://evil.com#fragment"
assert_invalid 'http://evil.com?query=1'
assert_invalid "javascript:alert(1)"
assert_invalid ""
assert_invalid "not-a-url"
assert_invalid "//evil.com"
assert_invalid $'http://evil.com\nnewline'
assert_invalid "http://evil.com:abc"
assert_invalid "http://[evil.com]"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
