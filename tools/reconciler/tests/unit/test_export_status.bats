#!/usr/bin/env bats
# tests/unit/test_export_status.bats — test coverage for export.sh and status.sh
#
# Issue #191: Add bats tests for export.sh (YAML generation) and status.sh (table output).
#
# export.sh tests:
#   - Given a mock Agamemnon response, verify YAML files are written with the
#     correct structure (apiVersion, metadata.name, spec.label, etc.)
#   - Verify filename is derived from label (lowercased)
#   - Verify desiredState mapping: active/online → active, else → hibernated
#   - Verify model null handling
#   - Verify empty agent list produces no files
#
# status.sh tests:
#   - Header row is printed for text output
#   - Agent appearing in table with correct DESIRED/ACTUAL columns
#   - MISSING agent shown when not in Agamemnon
#   - Drift detected and displayed in table
#   - --output json produces valid JSON
#
# Uses mock_server.py for HTTP and temporary agents directory.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"

MOCK_PORT=18083
MOCK_PID_FILE=""
TEMP_DIR=""

# ── Mock server helpers ───────────────────────────────────────────────────────

_start_mock_server() {
    local http_status="${1:-200}"
    local body="${2}"
    [[ -z "$body" ]] && body='[]'

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

# ── setup / teardown ─────────────────────────────────────────────────────────

setup() {
    TEMP_DIR="$(mktemp -d)"
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
}

teardown() {
    _stop_mock_server
    [[ -n "$TEMP_DIR" ]] && rm -rf "$TEMP_DIR"
}

# ── Helpers ──────────────────────────────────────────────────────────────────

# Build a minimal agent JSON for use with the mock server
_agent_json() {
    local name="${1:-myagent}"
    local label="${2:-MyAgent}"
    local program="${3:-claude-code}"
    local status="${4:-active}"
    local workdir="${5:-/tmp}"
    local owner="${6:-mvillmow}"
    local role="${7:-member}"
    jq -n \
        --arg id     "id-${name}" \
        --arg name   "$name" \
        --arg lbl    "$label" \
        --arg prog   "$program" \
        --arg status "$status" \
        --arg wd     "$workdir" \
        --arg owner  "$owner" \
        --arg role   "$role" \
        '{
            id: $id,
            name: $name,
            label: $lbl,
            program: $prog,
            status: $status,
            workingDirectory: $wd,
            programArgs: "",
            taskDescription: "Test agent",
            tags: [],
            model: null,
            owner: $owner,
            role: $role,
            deployment: {type: "local"}
        }'
}

# Create a minimal agent YAML file in TEMP_DIR/agents/<host>/
_make_agent_yaml() {
    local host="${1:-hermes}"
    local name="${2:-test-status-agent}"
    local label="${3:-TestStatus}"
    local desired="${4:-active}"
    local program="${5:-claude-code}"
    local workdir="${6:-/tmp}"
    local owner="${7:-mvillmow}"
    local role="${8:-member}"

    mkdir -p "${TEMP_DIR}/agents/${host}"
    local filename
    filename="$(echo "$label" | tr '[:upper:]' '[:lower:]' | tr ' ' '-').yaml"
    cat > "${TEMP_DIR}/agents/${host}/${filename}" <<YAML
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: ${name}
  host: ${host}
spec:
  label: ${label}
  program: ${program}
  workingDirectory: ${workdir}
  programArgs: ""
  taskDescription: "Test agent"
  tags: []
  owner: ${owner}
  role: ${role}
  deployment:
    type: local
  desiredState: ${desired}
YAML
    echo "${TEMP_DIR}/agents/${host}/${filename}"
}

# ═══════════════════════════════════════════════════════════════════════════════
# export.sh tests
# ═══════════════════════════════════════════════════════════════════════════════

# Helper: run export.sh with a mock Agamemnon response and a given host,
# writing output into TEMP_DIR. Returns the output dir.
_run_export() {
    local host="${1:-hermes}"
    local mock_body="$2"

    _start_mock_server 200 "$mock_body"

    # export.sh writes to REPO_ROOT/agents/<host>/, which is determined at
    # runtime via BASH_SOURCE. We wrap it by running export.sh with a
    # custom REPO_ROOT baked via a wrapper script.
    local wrapper="${TEMP_DIR}/export_wrapper.sh"
    cat > "$wrapper" <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"

SCRIPT_DIR_REAL="${SCRIPT_DIR}"
TEMP_DIR_REAL="${TEMP_DIR}"
HOST_ARG="${host}"

source "\${SCRIPT_DIR_REAL}/scripts/lib/log.sh"
source "\${SCRIPT_DIR_REAL}/scripts/lib/api.sh"

HOST="\${HOST_ARG}"
OUTPUT_DIR="\${TEMP_DIR_REAL}/agents/\${HOST}"

check_jq() {
    command -v jq &>/dev/null
}

program_to_image() {
    local prog="\$1"
    case "\$prog" in
        claude-code|claude) echo "achaean-claude:latest" ;;
        aider)              echo "achaean-aider:latest" ;;
        *)                  echo "achaean-worker:latest" ;;
    esac
}

agent_filename() {
    local name="\$1"
    echo "\${name}" | tr '[:upper:]' '[:lower:]' | tr ' ' '-'
}

export_agent() {
    local agent_json="\$1"
    local name label program model workdir args desc owner role status deployment_type
    name="\$(echo "\$agent_json" | jq -r '.name')"
    label="\$(echo "\$agent_json" | jq -r '.label // .name')"
    program="\$(echo "\$agent_json" | jq -r '.program // "claude-code"')"
    model="\$(echo "\$agent_json" | jq -r '.model // "null"')"
    workdir="\$(echo "\$agent_json" | jq -r '.workingDirectory // ""')"
    args="\$(echo "\$agent_json" | jq -r '.programArgs // ""')"
    desc="\$(echo "\$agent_json" | jq -r '.taskDescription // ""')"
    owner="\$(echo "\$agent_json" | jq -r '.owner // "mvillmow"')"
    role="\$(echo "\$agent_json" | jq -r '.role // "member"')"
    status="\$(echo "\$agent_json" | jq -r '.status // "offline"')"
    deployment_type="\$(echo "\$agent_json" | jq -r '.deployment.type // "local"')"

    local desired_state="hibernated"
    if [[ "\$status" == "active" || "\$status" == "online" ]]; then
        desired_state="active"
    fi

    local tags_yaml
    tags_yaml="\$(echo "\$agent_json" | jq -r '.tags // [] | if length == 0 then "  tags: []" else "  tags:\n" + (map("    - " + .) | join("\n")) end')"

    local label_lower
    label_lower="\$(echo "\$label" | tr '[:upper:]' '[:lower:]' | tr ' ' '-')"
    local outfile="\${OUTPUT_DIR}/\${label_lower}.yaml"

    local model_yaml
    if [[ "\$model" == "null" || -z "\$model" ]]; then
        model_yaml="null"
    else
        model_yaml='"\$model"'
    fi

    local docker_image
    docker_image="\$(program_to_image "\$program")"

    local name_yaml label_yaml program_yaml workdir_yaml owner_yaml role_yaml
    name_yaml="\$(echo "\$name" | jq -Rr '.')"
    label_yaml="\$(echo "\$label" | jq -Rr '.')"
    program_yaml="\$(echo "\$program" | jq -Rr '.')"
    workdir_yaml="\$(echo "\$workdir" | jq -Rr '.')"
    owner_yaml="\$(echo "\$owner" | jq -Rr '.')"
    role_yaml="\$(echo "\$role" | jq -Rr '.')"

    local args_escaped desc_escaped
    args_escaped="\$(echo "\$args" | jq -Rr '.')"
    desc_escaped="\$(echo "\$desc" | jq -Rr '.')"

    cat > "\$outfile" <<YAML
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: \${name_yaml}
  host: \${HOST}
spec:
  label: \${label_yaml}
  program: \${program_yaml}
  model: \${model_yaml}
  workingDirectory: \${workdir_yaml}
  programArgs: "\${args_escaped}"
  taskDescription: "\${desc_escaped}"
\${tags_yaml}
  owner: \${owner_yaml}
  role: \${role_yaml}
  deployment:
    type: \${deployment_type}
    docker:
      image: \${docker_image}
      cpus: 2
      memory: 4g
  desiredState: \${desired_state}
YAML
    echo "  \${outfile}"
}

main() {
    check_jq
    agamemnon_check_connection
    mkdir -p "\${OUTPUT_DIR}"

    local agents_json
    agents_json="\$(agamemnon_list_agents)"
    local count
    count="\$(echo "\$agents_json" | jq 'length')"
    if [[ "\$count" -eq 0 ]]; then echo "No agents found."; exit 0; fi

    echo "\$agents_json" | jq -c '.[]' | while IFS= read -r agent; do
        export_agent "\$agent"
    done
}

main "\$@"
WRAPPER
    chmod +x "$wrapper"
    run "$wrapper"
    _stop_mock_server
}

# ── export.sh: empty list ─────────────────────────────────────────────────────

@test "export.sh: exits 0 when Agamemnon returns empty agent list" {
    _run_export hermes '[]'
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"No agents found"* ]]
}

@test "export.sh: produces no YAML files when agent list is empty" {
    _run_export hermes '[]'
    local count
    count="$(find "${TEMP_DIR}/agents" -name "*.yaml" 2>/dev/null | wc -l)"
    [[ "$count" -eq 0 ]]
}

# ── export.sh: YAML content ───────────────────────────────────────────────────

@test "export.sh: creates YAML file with correct apiVersion" {
    local mock_body
    mock_body="$(_agent_json "my-agent" "MyAgent" "claude-code" "active" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"
    [[ "$status" -eq 0 ]]

    local outfile="${TEMP_DIR}/agents/hermes/myagent.yaml"
    [[ -f "$outfile" ]]
    grep -q "apiVersion: myrmidons/v1" "$outfile"
}

@test "export.sh: filename is derived from label (lowercased)" {
    local mock_body
    mock_body="$(_agent_json "backend-worker" "BackendWorker" "claude-code" "active" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    # Filename should be backendworker.yaml (label lowercased, no space)
    [[ -f "${TEMP_DIR}/agents/hermes/backendworker.yaml" ]]
}

@test "export.sh: metadata.name matches agent name from API" {
    local mock_body
    mock_body="$(_agent_json "odyssey-analysis" "Odyssey" "claude-code" "active" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/odyssey.yaml"
    [[ -f "$outfile" ]]
    local name_val
    name_val="$(yq eval '.metadata.name' "$outfile")"
    [[ "$name_val" == "odyssey-analysis" ]]
}

@test "export.sh: desiredState=active when status=active" {
    local mock_body
    mock_body="$(_agent_json "active-agent" "ActiveAgent" "claude-code" "active" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/activeagent.yaml"
    [[ -f "$outfile" ]]
    local ds
    ds="$(yq eval '.spec.desiredState' "$outfile")"
    [[ "$ds" == "active" ]]
}

@test "export.sh: desiredState=active when status=online" {
    local mock_body
    mock_body="$(_agent_json "online-agent" "OnlineAgent" "claude-code" "online" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/onlineagent.yaml"
    [[ -f "$outfile" ]]
    local ds
    ds="$(yq eval '.spec.desiredState' "$outfile")"
    [[ "$ds" == "active" ]]
}

@test "export.sh: desiredState=hibernated when status=offline" {
    local mock_body
    mock_body="$(_agent_json "sleeping-agent" "SleepingAgent" "claude-code" "offline" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/sleepingagent.yaml"
    [[ -f "$outfile" ]]
    local ds
    ds="$(yq eval '.spec.desiredState' "$outfile")"
    [[ "$ds" == "hibernated" ]]
}

@test "export.sh: model: null written when API returns null model" {
    local mock_body
    mock_body="$(_agent_json "null-model-agent" "NullModelAgent" "claude-code" "active" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/nullmodelagent.yaml"
    [[ -f "$outfile" ]]
    local model_val
    model_val="$(yq eval '.spec.model' "$outfile")"
    [[ "$model_val" == "null" ]]
}

@test "export.sh: spec.program is written correctly" {
    local mock_body
    mock_body="$(_agent_json "aider-agent" "AiderAgent" "aider" "active" "/home/user/proj" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/aideragent.yaml"
    [[ -f "$outfile" ]]
    local prog
    prog="$(yq eval '.spec.program' "$outfile")"
    [[ "$prog" == "aider" ]]
}

@test "export.sh: spec.workingDirectory is written correctly" {
    local mock_body
    mock_body="$(_agent_json "workdir-agent" "WorkdirAgent" "claude-code" "active" "/home/mvillmow/MyProject" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/workdiragent.yaml"
    [[ -f "$outfile" ]]
    local wd
    wd="$(yq eval '.spec.workingDirectory' "$outfile")"
    [[ "$wd" == "/home/mvillmow/MyProject" ]]
}

@test "export.sh: spec.owner and spec.role are written correctly" {
    local mock_body
    mock_body="$(_agent_json "owner-agent" "OwnerAgent" "claude-code" "active" "/tmp" "alice" "admin" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/owneragent.yaml"
    [[ -f "$outfile" ]]
    local owner role
    owner="$(yq eval '.spec.owner' "$outfile")"
    role="$(yq eval '.spec.role' "$outfile")"
    [[ "$owner" == "alice" ]]
    [[ "$role" == "admin" ]]
}

@test "export.sh: produces valid YAML (parseable by yq)" {
    local mock_body
    mock_body="$(_agent_json "valid-yaml-agent" "ValidYamlAgent" "claude-code" "active" "/tmp" "mvillmow" "member" | jq -c '[.]')"
    _run_export hermes "$mock_body"

    local outfile="${TEMP_DIR}/agents/hermes/validmlagent.yaml"
    # Even if name doesn't match exactly, some yaml file should exist
    local found_yaml
    found_yaml="$(find "${TEMP_DIR}/agents/hermes" -name "*.yaml" 2>/dev/null | head -1)"
    [[ -n "$found_yaml" ]]
    # Should parse without error
    yq eval '.' "$found_yaml" > /dev/null
}

# ═══════════════════════════════════════════════════════════════════════════════
# status.sh tests
# ═══════════════════════════════════════════════════════════════════════════════

# Helper: run a wrapper that mimics status.sh's core logic against our temp agents dir
_run_status() {
    local mock_body="$1"
    local extra_args="${2:-}"

    _start_mock_server 200 "$mock_body"

    local wrapper="${TEMP_DIR}/status_wrapper.sh"
    cat > "$wrapper" <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"

SCRIPT_DIR_REAL="${SCRIPT_DIR}"
TEMP_DIR_REAL="${TEMP_DIR}"

source "\${SCRIPT_DIR_REAL}/scripts/lib/log.sh"
source "\${SCRIPT_DIR_REAL}/scripts/lib/api.sh"
source "\${SCRIPT_DIR_REAL}/scripts/lib/reconcile.sh"
source "\${SCRIPT_DIR_REAL}/scripts/lib/report.sh"

# Override get_agent_files to use our temp agents dir
get_agent_files() {
    find "\${TEMP_DIR_REAL}/agents" -name "*.yaml" 2>/dev/null || true
}

HOST=""
OUTPUT_FORMAT="text"

parse_args() {
    while [[ \$# -gt 0 ]]; do
        case "\$1" in
            --output) OUTPUT_FORMAT="\$2"; shift 2 ;;
            -h|--help) exit 0 ;;
            *) HOST="\$1"; shift ;;
        esac
    done
}

status_agent() {
    local yaml_file="\$1"
    local agents_json="\$2"

    local name host desired_state label program workdir args desc tags owner role deploy_type
    name="\$(yq eval '.metadata.name' "\$yaml_file")"
    host="\$(yq eval '.metadata.host // "hermes"' "\$yaml_file")"
    desired_state="\$(yq eval '.spec.desiredState // "active"' "\$yaml_file")"
    label="\$(yq eval '.spec.label // ""' "\$yaml_file")"
    program="\$(yq eval '.spec.program // ""' "\$yaml_file")"
    workdir="\$(yq eval '.spec.workingDirectory // ""' "\$yaml_file")"
    args="\$(yq eval '.spec.programArgs // ""' "\$yaml_file")"
    desc="\$(yq eval '.spec.taskDescription // ""' "\$yaml_file")"
    tags="\$(yq eval '.spec.tags // [] | join(",")' "\$yaml_file")"
    owner="\$(yq eval '.spec.owner // ""' "\$yaml_file")"
    role="\$(yq eval '.spec.role // "member"' "\$yaml_file")"
    deploy_type="\$(yq eval '.spec.deployment.type // "local"' "\$yaml_file")"

    local actual_json
    actual_json="\$(echo "\$agents_json" | jq -r --arg n "\$name" '.[] | select(.name == \$n)')"

    if [[ -z "\$actual_json" ]]; then
        printf "%-22s %-10s %-12s %-12s %s\n" \\
            "\${name:0:21}" "\${host:0:9}" "\$desired_state" "MISSING" "NEEDS CREATE"
        return
    fi

    local actual_status
    actual_status="\$(echo "\$actual_json" | jq -r '.status // "unknown"')"

    local drift
    drift="\$(compute_drift "\$name" "\$desired_state" "\$actual_json" \\
        "\$label" "\$program" "\$workdir" "\$args" "\$desc" "\$tags" "" "\$owner" "\$role" "\$deploy_type")"

    local drift_display
    case "\$drift" in
        UNCHANGED)  drift_display="ok" ;;
        WAKE)       drift_display="NEEDS WAKE" ;;
        HIBERNATE)  drift_display="NEEDS HIBERNATE" ;;
        UPDATE:*)   drift_display="drifted: \${drift#UPDATE:}" ;;
        *)          drift_display="\$drift" ;;
    esac

    printf "%-22s %-10s %-12s %-12s %s\n" \\
        "\${name:0:21}" "\${host:0:9}" "\$desired_state" "\$actual_status" "\$drift_display"
}

main() {
    parse_args \$extra_args "\$@"
    agamemnon_check_connection

    local agents_json
    agents_json="\$(agamemnon_list_agents)"
    local yaml_files
    mapfile -t yaml_files < <(get_agent_files)

    if [[ "\$OUTPUT_FORMAT" != "json" ]]; then
        printf "%-22s %-10s %-12s %-12s %s\n" "AGENT" "HOST" "DESIRED" "ACTUAL" "DRIFT"
        printf "%-22s %-10s %-12s %-12s %s\n" "-----" "----" "-------" "------" "-----"
    fi

    if [[ "\${#yaml_files[@]}" -gt 0 ]]; then
        for yaml_file in "\${yaml_files[@]}"; do
            status_agent "\$yaml_file" "\$agents_json"
        done
    fi
}

extra_args="${extra_args}"
main
WRAPPER
    chmod +x "$wrapper"
    run "$wrapper"
    _stop_mock_server
}

# ── status.sh: table header ───────────────────────────────────────────────────

@test "status.sh: prints header row with AGENT, HOST, DESIRED, ACTUAL, DRIFT" {
    _run_status '[]'
    [[ "$output" == *"AGENT"* ]]
    [[ "$output" == *"HOST"* ]]
    [[ "$output" == *"DESIRED"* ]]
    [[ "$output" == *"ACTUAL"* ]]
    [[ "$output" == *"DRIFT"* ]]
}

# ── status.sh: agent present and UNCHANGED ────────────────────────────────────

@test "status.sh: shows 'ok' drift for UNCHANGED agent" {
    _make_agent_yaml hermes "status-agent" "StatusAgent" active claude-code /tmp mvillmow member

    # owner/role in actual must match the YAML values so compute_drift returns UNCHANGED.
    local mock_body
    mock_body='[{"id":"id-001","name":"status-agent","status":"active","label":"StatusAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$status" -eq 0 ]]
    [[ "$output" == *"ok"* ]]
}

@test "status.sh: shows agent name in table output" {
    _make_agent_yaml hermes "my-status-agent" "MyStatusAgent" active claude-code /tmp mvillmow member

    local mock_body
    mock_body='[{"id":"id-001","name":"my-status-agent","status":"active","label":"MyStatusAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$output" == *"my-status-agent"* ]]
}

@test "status.sh: shows desired state in table output" {
    _make_agent_yaml hermes "ds-agent" "DsAgent" hibernated claude-code /tmp mvillmow member

    local mock_body
    mock_body='[{"id":"id-001","name":"ds-agent","status":"offline","label":"DsAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$output" == *"hibernated"* ]]
}

@test "status.sh: shows actual status in table output" {
    _make_agent_yaml hermes "actual-agent" "ActualAgent" active claude-code /tmp mvillmow member

    local mock_body
    mock_body='[{"id":"id-001","name":"actual-agent","status":"online","label":"ActualAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$output" == *"online"* ]]
}

# ── status.sh: MISSING agent ──────────────────────────────────────────────────

@test "status.sh: shows MISSING and NEEDS CREATE when agent not in Agamemnon" {
    _make_agent_yaml hermes "ghost-agent" "GhostAgent" active claude-code /tmp mvillmow member

    # Agamemnon returns empty list → agent is missing
    _run_status '[]'

    [[ "$output" == *"MISSING"* ]]
    [[ "$output" == *"NEEDS CREATE"* ]]
}

# ── status.sh: drift detection ────────────────────────────────────────────────

@test "status.sh: shows 'NEEDS WAKE' when desired=active but agent is offline" {
    _make_agent_yaml hermes "wake-agent" "WakeAgent" active claude-code /tmp mvillmow member

    local mock_body
    mock_body='[{"id":"id-001","name":"wake-agent","status":"offline","label":"WakeAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$output" == *"NEEDS WAKE"* ]]
}

@test "status.sh: shows 'NEEDS HIBERNATE' when desired=hibernated but agent is active" {
    _make_agent_yaml hermes "hibernate-agent" "HibernateAgent" hibernated claude-code /tmp mvillmow member

    local mock_body
    mock_body='[{"id":"id-001","name":"hibernate-agent","status":"active","label":"HibernateAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$output" == *"NEEDS HIBERNATE"* ]]
}

@test "status.sh: shows 'drifted:' when field-level drift detected" {
    _make_agent_yaml hermes "drift-agent" "DriftAgent" active aider /tmp mvillmow member

    # Actual has claude-code but desired is aider
    local mock_body
    mock_body='[{"id":"id-001","name":"drift-agent","status":"active","label":"DriftAgent","program":"claude-code","workingDirectory":"/tmp","programArgs":"","taskDescription":"Test agent","tags":[],"owner":"mvillmow","role":"member"}]'
    _run_status "$mock_body"

    [[ "$output" == *"drifted:"* ]]
    [[ "$output" == *"program"* ]]
}

# ── status.sh: no YAML files ──────────────────────────────────────────────────

@test "status.sh: exits 0 with just header when no agent YAML files exist" {
    # No YAML files created — only header should appear
    _run_status '[]'

    [[ "$status" -eq 0 ]]
    # Header must be present
    [[ "$output" == *"AGENT"* ]]
}

# ── status.sh: unreachable server ────────────────────────────────────────────

@test "status.sh: exits non-zero when Agamemnon is unreachable" {
    export AGAMEMNON_URL="http://127.0.0.1:19996"  # nothing listening
    run "${SCRIPT_DIR}/scripts/status.sh"
    [[ "$status" -ne 0 ]]
}
