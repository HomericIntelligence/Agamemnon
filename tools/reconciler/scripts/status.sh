#!/usr/bin/env bash
# scripts/status.sh — Compare desired vs actual state
#
# Shows a formatted table of all managed agents with their desired state,
# actual state, and whether there's any drift.
#
# Usage:
#   ./scripts/status.sh                      # Status of all agents
#   ./scripts/status.sh hermes               # Status of agents on a specific host
#   ./scripts/status.sh --output json        # Machine-readable JSON drift report
#   ./scripts/status.sh hermes --output json

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=scripts/lib/config.sh
source "${SCRIPT_DIR}/lib/config.sh"
# shellcheck source=scripts/lib/log.sh
source "${SCRIPT_DIR}/lib/log.sh"
# shellcheck source=scripts/lib/api.sh
source "${SCRIPT_DIR}/lib/api.sh"
# shellcheck source=scripts/lib/reconcile.sh
source "${SCRIPT_DIR}/lib/reconcile.sh"
# shellcheck source=scripts/lib/report.sh
source "${SCRIPT_DIR}/lib/report.sh"

load_config

HOST=""
OUTPUT_FORMAT="text"   # "text" | "json"

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --output) OUTPUT_FORMAT="$2"; shift 2 ;;
            -h|--help) usage; exit 0 ;;
            *) HOST="$1"; shift ;;
        esac
    done
}

usage() {
    cat <<EOF
Usage: $0 [host] [--output json]

Shows desired vs actual state for all managed agents.

Options:
  host           Only show agents for this host (default: all)
  --output json  Emit a JSON drift report to stdout
  -h, --help     Show this help

Environment:
  AGAMEMNON_URL            Agamemnon base URL (default: http://localhost:8080)
  AGAMEMNON_API_KEY        Bearer token for Agamemnon API authentication
  MYRMIDONS_DEFAULT_OWNER  Fallback owner used when the API returns no owner

Examples:
  $0                         # Human-readable table
  $0 hermes                  # Filter to hermes host
  $0 --output json | jq .    # Machine-readable drift report
EOF
}

main() {
    parse_args "$@"

    # Validate AGAMEMNON_URL format early (#118)
    validate_agamemnon_url

    check_deps
    agamemnon_check_connection

    report_init "${HOST:-all}"
    trap report_cleanup EXIT

    local agents_json
    agents_json="$(agamemnon_list_agents)"

    # Precompute a name→status lookup map once (fixes O(N) jq calls for unmanaged agents, #114)
    local status_map
    status_map="$(echo "$agents_json" | jq 'map({key: .name, value: (.status // "unknown")}) | from_entries')"

    local yaml_files
    mapfile -t yaml_files < <(get_agent_files "$HOST")

    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        # Header
        printf "%-22s %-10s %-12s %-12s %s\n" "AGENT" "HOST" "DESIRED" "ACTUAL" "DRIFT"
        printf "%-22s %-10s %-12s %-12s %s\n" "-----" "----" "-------" "------" "-----"
    fi

    for yaml_file in "${yaml_files[@]}"; do
        status_agent "$yaml_file" "$agents_json"
    done

    # Unmanaged agents
    report_unmanaged "$agents_json" "$status_map" "${yaml_files[@]}"

    if [[ "$OUTPUT_FORMAT" == "json" ]]; then
        # Produce a drift-focused report (no action counters for status)
        report_emit 0 0 0 0 0 0 0
    fi
}

status_agent() {
    local yaml_file="$1"
    local agents_json="$2"

    local name host desired_state label program workdir args desc tags model owner role deploy_type
    name="$(yq eval '.metadata.name' "$yaml_file")"
    host="$(yq eval '.metadata.host // "hermes"' "$yaml_file")"
    desired_state="$(yq eval '.spec.desiredState // "active"' "$yaml_file")"
    label="$(yq eval '.spec.label // ""' "$yaml_file")"
    program="$(yq eval '.spec.program // ""' "$yaml_file")"
    workdir="$(yq eval '.spec.workingDirectory // ""' "$yaml_file")"
    args="$(yq eval '.spec.programArgs // ""' "$yaml_file")"
    desc="$(yq eval '.spec.taskDescription // ""' "$yaml_file")"
    tags="$(yq eval '.spec.tags // [] | join(",")' "$yaml_file")"
    model="$(yq eval '.spec.model // ""' "$yaml_file")"
    owner="$(yq eval '.spec.owner // ""' "$yaml_file")"
    role="$(yq eval '.spec.role // "member"' "$yaml_file")"
    deploy_type="$(yq eval '.spec.deployment.type // "local"' "$yaml_file")"

    local actual_json
    actual_json="$(echo "$agents_json" | jq -r --arg n "$name" '.[] | select(.name == $n)')"

    if [[ -z "$actual_json" ]]; then
        if [[ "${LOG_FORMAT:-text}" == "json" ]]; then
            jq -n --arg name "$name" --arg host "$host" \
                --arg desired "$desired_state" --arg actual "MISSING" --arg drift "NEEDS CREATE" \
                '{agent:$name,host:$host,desired:$desired,actual:$actual,drift:$drift}'
        elif [[ "$OUTPUT_FORMAT" != "json" ]]; then
            printf "%-22s %-10s %-12s %-12s %s\n" \
                "${name:0:21}" "${host:0:9}" "$desired_state" "MISSING" "NEEDS CREATE"
        fi
        report_add_agent "$name" "$host" "CREATE" "$desired_state" "MISSING" "[]" ""
        return
    fi

    local actual_status
    actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

    local drift
    drift="$(compute_drift "$name" "$desired_state" "$actual_json" \
        "$label" "$program" "$workdir" "$args" "$desc" "$tags" "$model" "$owner" "$role" "$deploy_type")"

    local drift_json="[]"
    if [[ "$drift" == UPDATE:* ]]; then
        drift_json="$(build_drift_json "$drift" "$actual_json" \
            "$label" "$program" "$workdir" "$args" "$desc" "$tags" "$owner" "$role")"
    fi

    local action
    case "$drift" in
        UNCHANGED)  action="UNCHANGED" ;;
        WAKE)       action="WAKE" ;;
        HIBERNATE)  action="HIBERNATE" ;;
        UPDATE:*)   action="UPDATE" ;;
        *)          action="$drift" ;;
    esac

    report_add_agent "$name" "$host" "$action" "$desired_state" "$actual_status" "$drift_json" ""

    if [[ "${LOG_FORMAT:-text}" == "json" ]]; then
        local drift_display
        case "$drift" in
            UNCHANGED) drift_display="ok" ;;
            WAKE)      drift_display="NEEDS WAKE" ;;
            HIBERNATE) drift_display="NEEDS HIBERNATE" ;;
            UPDATE:*)  drift_display="drifted: ${drift#UPDATE:}" ;;
            *)         drift_display="$drift" ;;
        esac
        jq -n --arg name "$name" --arg host "$host" \
            --arg desired "$desired_state" --arg actual "$actual_status" --arg drift "$drift_display" \
            '{agent:$name,host:$host,desired:$desired,actual:$actual,drift:$drift}'
    elif [[ "$OUTPUT_FORMAT" != "json" ]]; then
        local drift_display
        case "$drift" in
            UNCHANGED)
                drift_display="ok"
                ;;
            WAKE)
                drift_display="NEEDS WAKE"
                ;;
            HIBERNATE)
                drift_display="NEEDS HIBERNATE"
                ;;
            UPDATE:*)
                drift_display="drifted: ${drift#UPDATE:}"
                ;;
            *)
                drift_display="$drift"
                ;;
        esac

        printf "%-22s %-10s %-12s %-12s %s\n" \
            "${name:0:21}" "${host:0:9}" "$desired_state" "$actual_status" "$drift_display"
    fi
}

report_unmanaged() {
    local agents_json="$1"
    local status_map="$2"
    shift 2

    while IFS= read -r actual_name; do
        # Use precomputed map for O(1) status lookup instead of a per-agent jq call (#114)
        local actual_status
        actual_status="$(echo "$status_map" | jq -r --arg n "$actual_name" '.[$n] // "unknown"')"

        report_add_unmanaged "$actual_name"

        if [[ "${LOG_FORMAT:-text}" == "json" ]]; then
            jq -n --arg name "$actual_name" --arg actual "$actual_status" \
                '{agent:$name,host:"-",desired:"-",actual:$actual,drift:"UNMANAGED"}'
        elif [[ "$OUTPUT_FORMAT" != "json" ]]; then
            printf "%-22s %-10s %-12s %-12s %s\n" \
                "${actual_name:0:21}" "-" "-" "$actual_status" "UNMANAGED"
        fi
    done < <(get_unmanaged_names "$agents_json" "$@")
}

main "$@"
