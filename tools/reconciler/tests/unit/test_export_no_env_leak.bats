#!/usr/bin/env bats
# tests/unit/test_export_no_env_leak.bats — guard against MYRMIDONS_DEFAULT_OWNER
# leaking into the parent shell environment after export.sh runs.
#
# Issue #526 (follow-up from #404): The previous fix added
# `export MYRMIDONS_DEFAULT_OWNER=...` inside main(), which leaked the
# variable into the parent shell's environment when export.sh was `source`d.
# The fix scopes the value to a function-local shell variable — visible to
# subshells of main() (the piped `while` loop), but invisible to the parent
# shell after export.sh returns.
#
# Note: export.sh calls `exit 0` when the API returns zero agents, which
# would terminate the sourcing shell normally. We use an `EXIT` trap so we
# can still observe the post-source environment state.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"

MOCK_PORT=18091
MOCK_PID_FILE=""
TEMP_DIR=""

_start_mock_server() {
    local http_status="${1:-200}"
    local body="${2:-[]}"

    MOCK_PID_FILE="${TEMP_DIR}/mock.pid"
    MOCK_STATUS="$http_status" MOCK_BODY="$body" \
        python3 "${HELPERS_DIR}/mock_server.py" "$MOCK_PORT" \
        > /dev/null 2>&1 &
    echo $! > "$MOCK_PID_FILE"
    sleep 0.2
}

_stop_mock_server() {
    if [[ -n "$MOCK_PID_FILE" && -f "$MOCK_PID_FILE" ]]; then
        kill "$(cat "$MOCK_PID_FILE")" 2>/dev/null || true
        rm -f "$MOCK_PID_FILE"
    fi
}

setup() {
    TEMP_DIR="$(mktemp -d)"
    unset MYRMIDONS_DEFAULT_OWNER
}

teardown() {
    _stop_mock_server
    [[ -n "$TEMP_DIR" ]] && rm -rf "$TEMP_DIR"
}

_make_mock_whoami() {
    mkdir -p "${TEMP_DIR}/mocks"
    cat > "${TEMP_DIR}/mocks/whoami" <<'EOF'
#!/usr/bin/env bash
echo "mockuser"
EOF
    chmod +x "${TEMP_DIR}/mocks/whoami"
}

@test "export.sh: MYRMIDONS_DEFAULT_OWNER does not leak into parent shell after sourcing" {
    _make_mock_whoami
    _start_mock_server 200 "[]"

    local result_file="${TEMP_DIR}/result"
    local probe="${TEMP_DIR}/probe.sh"
    cat > "$probe" <<PROBE
#!/usr/bin/env bash
unset MYRMIDONS_DEFAULT_OWNER
trap '
if [[ -n \${MYRMIDONS_DEFAULT_OWNER+set} ]]; then
    echo "LEAK:\${MYRMIDONS_DEFAULT_OWNER}" > "${result_file}"
else
    echo OK > "${result_file}"
fi
' EXIT
source "${SCRIPT_DIR}/scripts/export.sh" hermes >/dev/null 2>&1
PROBE
    chmod +x "$probe"

    PATH="${TEMP_DIR}/mocks:${PATH}" AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}" \
        bash "$probe" || true
    _stop_mock_server

    local result=""
    [[ -f "$result_file" ]] && result="$(cat "$result_file")"
    echo "result=$result"
    [[ "$result" == "OK" ]]
}

@test "export.sh: parent's pre-set MYRMIDONS_DEFAULT_OWNER is preserved after sourcing" {
    _make_mock_whoami
    _start_mock_server 200 "[]"

    local result_file="${TEMP_DIR}/result"
    local probe="${TEMP_DIR}/probe.sh"
    cat > "$probe" <<PROBE
#!/usr/bin/env bash
export MYRMIDONS_DEFAULT_OWNER="caller-owner"
trap '
echo "AFTER:\${MYRMIDONS_DEFAULT_OWNER:-unset}" > "${result_file}"
' EXIT
source "${SCRIPT_DIR}/scripts/export.sh" hermes >/dev/null 2>&1
PROBE
    chmod +x "$probe"

    PATH="${TEMP_DIR}/mocks:${PATH}" AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}" \
        bash "$probe" || true
    _stop_mock_server

    local result=""
    [[ -f "$result_file" ]] && result="$(cat "$result_file")"
    echo "result=$result"
    [[ "$result" == "AFTER:caller-owner" ]]
}
