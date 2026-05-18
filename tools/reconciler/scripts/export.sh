#!/usr/bin/env bash
# scripts/export.sh — Bootstrap: export current Agamemnon agents to YAML
#
# Reads the current agent registry from Agamemnon and writes one YAML
# file per agent into agents/<host>/.
#
# Usage:
#   ./scripts/export.sh hermes
#   ./scripts/export.sh                # defaults to "hermes"
#
# Environment:
#   MYRMIDONS_DEFAULT_OWNER  Fallback owner written to exported agent YAMLs when
#                            the Agamemnon API returns no owner. Defaults to $(whoami).
#
# This is the bootstrap script. Run it once to seed Myrmidons
# from the current Agamemnon state.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=scripts/lib/config.sh
source "${SCRIPT_DIR}/lib/config.sh"
# shellcheck source=scripts/lib/log.sh
source "${SCRIPT_DIR}/lib/log.sh"
# shellcheck source=scripts/lib/api.sh
source "${SCRIPT_DIR}/lib/api.sh"

load_config

HOST="${1:-hermes}"
OUTPUT_DIR="${REPO_ROOT}/agents/${HOST}"

main() {
    # Validate AGAMEMNON_URL format early (#118)
    validate_agamemnon_url

    check_jq
    agamemnon_check_connection

    # Resolve the effective owner once into a function-local variable with a
    # distinct name. We deliberately do NOT `export` it: bash subshells
    # (including the pipe-fed `while` loop below) already inherit non-exported
    # shell variables, so `export_agent` sees the value without polluting the
    # parent shell's environment when this script is `source`d.
    #
    # NB: we cannot use `local MYRMIDONS_DEFAULT_OWNER=...` here because bash
    # keeps function locals visible to the EXIT trap when `exit` is called
    # from inside the function — which would leak a name-collision back into
    # the sourcing shell's trap context (see test_export_no_env_leak.bats).
    # See issue #526 (follow-up from #404).
    local _effective_owner="${MYRMIDONS_DEFAULT_OWNER:-$(whoami)}"
    log_debug "Default owner for exported agents: ${_effective_owner}"

    log_info "Exporting agents from Agamemnon (${AGAMEMNON_URL}) for host: ${HOST}"
    log_info ""

    mkdir -p "${OUTPUT_DIR}"

    local agents_json
    agents_json="$(agamemnon_list_agents)"

    local count
    count="$(echo "$agents_json" | jq 'length')"

    if [[ "$count" -eq 0 ]]; then
        log_info "No agents found in Agamemnon."
        exit 0
    fi

    echo "$agents_json" | jq -c '.[]' | while IFS= read -r agent; do
        export_agent "$agent"
    done

    log_info ""
    log_info "Exported ${count} agents to ${OUTPUT_DIR}/"
}

check_jq() {
    local missing=()
    if ! command -v jq &>/dev/null; then
        missing+=("jq")
    fi
    if ! command -v yq &>/dev/null; then
        missing+=("yq")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required tools: ${missing[*]}"
        log_error "  Install jq: apt install jq / brew install jq"
        log_error "  Install yq: https://github.com/mikefarah/yq"
        exit 1
    fi
}

# Derive the canonical filename stem from a label (lowercase, spaces→hyphens)
# Convention: filename = lowercase(spec.label) + ".yaml"
label_to_stem() {
    local lbl="$1"
    echo "${lbl}" | tr '[:upper:]' '[:lower:]' | tr ' ' '-'
}

# Map program name → default AchaeanFleet image name
program_to_image() {
    local prog="$1"
    case "$prog" in
        claude-code|claude) echo "achaean-claude:latest" ;;
        codex)              echo "achaean-codex:latest" ;;
        aider)              echo "achaean-aider:latest" ;;
        goose)              echo "achaean-goose:latest" ;;
        cline)              echo "achaean-cline:latest" ;;
        opencode)           echo "achaean-opencode:latest" ;;
        codebuff)           echo "achaean-codebuff:latest" ;;
        ampcode)            echo "achaean-ampcode:latest" ;;
        none|worker|"")     echo "achaean-worker:latest" ;;
        *)                  echo "achaean-worker:latest" ;;
    esac
}

export_agent() {
    local agent_json="$1"

    local name label program model workdir args desc owner role status deployment_type
    name="$(echo "$agent_json" | jq -r '.name')"
    label="$(echo "$agent_json" | jq -r '.label // .name')"
    program="$(echo "$agent_json" | jq -r '.program // "claude-code"')"
    model="$(echo "$agent_json" | jq -r '.model // "null"')"
    workdir="$(echo "$agent_json" | jq -r '.workingDirectory // ""')"
    args="$(echo "$agent_json" | jq -r '.programArgs // ""')"
    desc="$(echo "$agent_json" | jq -r '.taskDescription // ""')"
    owner="$(echo "$agent_json" | jq -r --arg default_owner "${_effective_owner:-${MYRMIDONS_DEFAULT_OWNER:-$(whoami)}}" '.owner // $default_owner')"
    role="$(echo "$agent_json" | jq -r '.role // "member"')"
    status="$(echo "$agent_json" | jq -r '.status // "offline"')"
    deployment_type="$(echo "$agent_json" | jq -r '.deployment.type // "local"')"

    # Map current status to desiredState
    local desired_state="hibernated"
    if [[ "$status" == "active" || "$status" == "online" ]]; then
        desired_state="active"
    fi

    # Build tags YAML list
    local tags_yaml
    tags_yaml="$(echo "$agent_json" | jq -r '.tags // [] | if length == 0 then "  tags: []" else "  tags:\n" + (map("    - " + .) | join("\n")) end')"

    # Derive filename from label per convention: filename = lowercase(spec.label) + ".yaml"
    local label_lower
    label_lower="$(label_to_stem "$label")"
    local outfile="${OUTPUT_DIR}/${label_lower}.yaml"

    # Warn if a stale file exists under the old name-derived path that would not be overwritten
    local name_lower
    name_lower="$(label_to_stem "$name")"
    local name_outfile="${OUTPUT_DIR}/${name_lower}.yaml"
    if [[ "$name_lower" != "$label_lower" && -e "$name_outfile" ]]; then
        log_warn "  WARN: stale file '${name_outfile}' may exist from a prior name-based export; label-derived path is '${outfile}'"
    fi

    # Handle model: if "null" string, write null (no quotes)
    local model_yaml
    if [[ "$model" == "null" || -z "$model" ]]; then
        model_yaml="null"
    else
        model_yaml="\"${model}\""
    fi

    # Map program → correct docker image
    local docker_image
    docker_image="$(program_to_image "$program")"

    # Use jq to safely quote string fields that may contain special characters.
    # Scalar YAML strings only need quoting when they contain: :, #, {, }, [, ], , &, * ! | > ' " % @ `
    # Using double-quoted YAML strings throughout is safe and simple.
    local name_yaml label_yaml program_yaml workdir_yaml owner_yaml role_yaml
    name_yaml="$(echo "$name" | jq -Rr '.')"
    label_yaml="$(echo "$label" | jq -Rr '.')"
    program_yaml="$(echo "$program" | jq -Rr '.')"
    workdir_yaml="$(echo "$workdir" | jq -Rr '.')"
    owner_yaml="$(echo "$owner" | jq -Rr '.')"
    role_yaml="$(echo "$role" | jq -Rr '.')"

    # args and desc always get explicit double-quotes in the YAML
    local args_escaped desc_escaped
    args_escaped="$(echo "$args" | jq -Rr '.')"
    desc_escaped="$(echo "$desc" | jq -Rr '.')"

    cat > "$outfile" <<YAML
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: ${name_yaml}
  host: ${HOST}
spec:
  label: ${label_yaml}
  program: ${program_yaml}
  model: ${model_yaml}
  workingDirectory: ${workdir_yaml}
  programArgs: "${args_escaped}"
  taskDescription: "${desc_escaped}"
${tags_yaml}
  owner: ${owner_yaml}
  role: ${role_yaml}
  deployment:
    type: ${deployment_type}
    docker:
      image: ${docker_image}
      cpus: 2
      memory: 4g
  desiredState: ${desired_state}
YAML

    log_info "  ${outfile}"
}

main "$@"
