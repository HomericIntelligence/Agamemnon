#!/usr/bin/env bats
# tests/unit/test_export_default_owner.bats — test MYRMIDONS_DEFAULT_OWNER resolution
#
# Issue #404: export.sh resolved $(whoami) once per agent in export_agent().
# Fix: main() exports MYRMIDONS_DEFAULT_OWNER after resolving it, so subsequent
# calls inside export_agent() find the variable already set.
#
# Tests:
#   - whoami is called at most once when MYRMIDONS_DEFAULT_OWNER is unset (multi-agent)
#   - whoami is never called when MYRMIDONS_DEFAULT_OWNER is pre-set
#   - spec.owner in generated YAML uses the fallback when API returns null owner

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"

MOCK_PORT=18087
MOCK_PID_FILE=""
TEMP_DIR=""

# ── Mock server helpers ───────────────────────────────────────────────────────

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

# ── setup / teardown ─────────────────────────────────────────────────────────

setup() {
    TEMP_DIR="$(mktemp -d)"
    export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
    unset MYRMIDONS_DEFAULT_OWNER
}

teardown() {
    _stop_mock_server
    [[ -n "$TEMP_DIR" ]] && rm -rf "$TEMP_DIR"
}

# ── Helpers ──────────────────────────────────────────────────────────────────

# Build a minimal agent JSON with null owner (to trigger the fallback path)
_agent_json_null_owner() {
    local name="${1:-agent}"
    local label="${2:-Agent}"
    jq -n \
        --arg name  "$name" \
        --arg lbl   "$label" \
        '{
            id: ("id-" + $name),
            name: $name,
            label: $lbl,
            program: "claude-code",
            status: "active",
            workingDirectory: "/tmp",
            programArgs: "",
            taskDescription: "Test agent",
            tags: [],
            model: null,
            owner: null,
            role: "member",
            deployment: {type: "local"}
        }'
}

# Run export.sh with a mock `whoami` injected via PATH.
# Sets WHOAMI_CALL_COUNT_FILE so tests can count invocations.
_run_export_with_mock_whoami() {
    local mock_body="$1"

    # Create a mock whoami that records each call
    mkdir -p "${TEMP_DIR}/mocks"
    export WHOAMI_CALL_COUNT_FILE="${TEMP_DIR}/whoami_calls"
    cat > "${TEMP_DIR}/mocks/whoami" <<'EOF'
#!/usr/bin/env bash
echo "1" >> "${WHOAMI_CALL_COUNT_FILE}"
echo "mockuser"
EOF
    chmod +x "${TEMP_DIR}/mocks/whoami"
    export PATH="${TEMP_DIR}/mocks:${PATH}"

    _start_mock_server 200 "$mock_body"

    local wrapper="${TEMP_DIR}/export_wrapper.sh"
    cat > "$wrapper" <<WRAPPER
#!/usr/bin/env bash
set -euo pipefail
export AGAMEMNON_URL="http://127.0.0.1:${MOCK_PORT}"
export PATH="${TEMP_DIR}/mocks:\${PATH}"
export WHOAMI_CALL_COUNT_FILE="${WHOAMI_CALL_COUNT_FILE}"

SCRIPT_DIR_REAL="${SCRIPT_DIR}"
TEMP_DIR_REAL="${TEMP_DIR}"

HOST="hermes"
OUTPUT_DIR="\${TEMP_DIR_REAL}/agents/\${HOST}"

# shellcheck source=scripts/lib/log.sh
source "\${SCRIPT_DIR_REAL}/scripts/lib/log.sh"
# shellcheck source=scripts/lib/api.sh
source "\${SCRIPT_DIR_REAL}/scripts/lib/api.sh"

check_jq() { command -v jq &>/dev/null; }

program_to_image() {
    local prog="\$1"
    case "\$prog" in
        claude-code|claude) echo "achaean-claude:latest" ;;
        *) echo "achaean-worker:latest" ;;
    esac
}

label_to_stem() {
    local lbl="\$1"
    echo "\${lbl}" | tr '[:upper:]' '[:lower:]' | tr ' ' '-'
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
    owner="\$(echo "\$agent_json" | jq -r --arg default_owner "\${MYRMIDONS_DEFAULT_OWNER:-\$(whoami)}" '.owner // \$default_owner')"
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
    label_lower="\$(label_to_stem "\$label")"
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
}

main() {
    check_jq
    agamemnon_check_connection
    mkdir -p "\${OUTPUT_DIR}"

    local effective_owner="\${MYRMIDONS_DEFAULT_OWNER:-\$(whoami)}"
    export MYRMIDONS_DEFAULT_OWNER="\$effective_owner"

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

# ── Tests ─────────────────────────────────────────────────────────────────────

@test "export.sh: whoami called at most once when MYRMIDONS_DEFAULT_OWNER unset and multiple agents exported" {
    local agent1 agent2 agent3 mock_body
    agent1="$(_agent_json_null_owner "agent-one" "AgentOne")"
    agent2="$(_agent_json_null_owner "agent-two" "AgentTwo")"
    agent3="$(_agent_json_null_owner "agent-three" "AgentThree")"
    mock_body="$(echo "[$agent1,$agent2,$agent3]")"

    unset MYRMIDONS_DEFAULT_OWNER
    _run_export_with_mock_whoami "$mock_body"

    [[ "$status" -eq 0 ]]

    local call_count=0
    if [[ -f "$WHOAMI_CALL_COUNT_FILE" ]]; then
        call_count="$(wc -l < "$WHOAMI_CALL_COUNT_FILE")"
    fi
    # whoami should be called at most once (resolved once in main(), not per-agent)
    [[ "$call_count" -le 1 ]]
}

@test "export.sh: whoami not called when MYRMIDONS_DEFAULT_OWNER is pre-set" {
    local agent1 agent2 mock_body
    agent1="$(_agent_json_null_owner "agent-alpha" "AgentAlpha")"
    agent2="$(_agent_json_null_owner "agent-beta" "AgentBeta")"
    mock_body="$(echo "[$agent1,$agent2]")"

    export MYRMIDONS_DEFAULT_OWNER="presetuser"
    _run_export_with_mock_whoami "$mock_body"

    [[ "$status" -eq 0 ]]

    local call_count=0
    if [[ -f "$WHOAMI_CALL_COUNT_FILE" ]]; then
        call_count="$(wc -l < "$WHOAMI_CALL_COUNT_FILE")"
    fi
    # whoami must not be called at all when the variable is already set
    [[ "$call_count" -eq 0 ]]
}

@test "export.sh: fallback owner written to spec.owner when API returns null owner" {
    local agent1 mock_body
    agent1="$(_agent_json_null_owner "null-owner-agent" "NullOwnerAgent")"
    mock_body="$(echo "[$agent1]")"

    unset MYRMIDONS_DEFAULT_OWNER
    _run_export_with_mock_whoami "$mock_body"

    [[ "$status" -eq 0 ]]

    local outfile="${TEMP_DIR}/agents/hermes/nullowneragent.yaml"
    [[ -f "$outfile" ]]

    local owner_val
    owner_val="$(yq eval '.spec.owner' "$outfile")"
    # The fallback should be "mockuser" (from our mock whoami)
    [[ "$owner_val" == "mockuser" ]]
}
