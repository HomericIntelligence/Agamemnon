#!/usr/bin/env bash
# scripts/plan.sh — Dry-run: show what apply.sh would do
#
# Compares desired state (YAML files) against actual state (Agamemnon API)
# and prints what changes would be made. Makes NO changes.
#
# Usage:
#   ./scripts/plan.sh                  # Plan all agents on all hosts
#   ./scripts/plan.sh hermes           # Plan agents for a specific host
#   ./scripts/plan.sh --output json    # Emit machine-readable JSON summary
#
# Exit codes:
#   0 = no changes needed
#   1 = changes would be made (or error)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

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
FLEET_NAME=""
OUTPUT_FORMAT="text"   # "text" | "json" (#179)
PRUNE=0

# Counters for JSON output
CREATE_COUNT=0
UPDATE_COUNT=0
WAKE_COUNT=0
HIBERNATE_COUNT=0
UNCHANGED_COUNT=0

# Accumulate planned_changes entries (one JSON object per line, NDJSON)
_PLAN_CHANGES_TMP=""

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -h|--help)  usage; exit 0 ;;
            --dry-run)  shift ;;
            --fleet)    FLEET_NAME="$2"; shift 2 ;;
            --output)   OUTPUT_FORMAT="$2"; shift 2 ;;
            --webhook)  shift 2 ;;  # Accepted but ignored in plan
            --prune)    PRUNE=1; shift ;;
            -*) echo "ERROR: Unknown argument: $1" >&2; usage; exit 1 ;;
            *) HOST="$1"; shift ;;
        esac
    done
}

usage() {
    echo "Usage: $0 [host] [--fleet <name>] [--output json] [--prune]"
    echo ""
    echo "Shows what apply.sh would do without making any changes."
    echo ""
    echo "Options:"
    echo "  host           Only plan agents for this host (default: all)"
    echo "  --fleet <name> Only plan agents in the named fleet"
    echo "  --output json  Emit a machine-readable JSON plan report to stdout"
    echo "  --prune        Also show which unmanaged agents would be pruned"
    echo "  -h, --help     Show this help"
    echo ""
    echo "Environment:"
    echo "  AGAMEMNON_URL            Agamemnon base URL (default: http://localhost:8080)"
    echo "  AGAMEMNON_API_KEY        Bearer token for Agamemnon API authentication"
    echo "  MYRMIDONS_DEFAULT_OWNER  Fallback owner used when the API returns no owner"
    echo ""
    echo "Examples:"
    echo "  $0                        # Plan all agents"
    echo "  $0 hermes                 # Plan agents on hermes"
    echo "  $0 --fleet dev-mesh       # Plan agents in the dev-mesh fleet"
    echo "  $0 --output json          # Machine-readable JSON summary"
    echo "  $0 hermes --output json | jq .summary"
    echo "  $0 --prune                # Plan including unmanaged agent removal"
}

# plan_report_unmanaged wraps reconcile.sh's report_unmanaged to avoid
# shadowing if a local function of the same name were ever defined here.
# (#273: prevent name collision with reconcile.sh's report_unmanaged)
plan_report_unmanaged() {
    report_unmanaged "$@"
}

# Record one planned change entry into the temp file
_plan_record_change() {
    local name="$1"
    local action="$2"       # CREATE | UPDATE | WAKE | HIBERNATE | UNCHANGED
    local details="${3:-}"  # human-readable detail string

    jq -n \
        --arg name "$name" \
        --arg action "$action" \
        --arg details "$details" \
        '{name: $name, action: $action, details: $details}' >> "$_PLAN_CHANGES_TMP"
}

main() {
    parse_args "$@"

    # Validate AGAMEMNON_URL format early (#118)
    validate_agamemnon_url

    # Validate HOST argument against known agents/ subdirectories (#149)
    if [[ -n "$HOST" && ! -d "${REPO_ROOT}/agents/${HOST}" ]]; then
        log_error "Host '${HOST}' not found — no agents/${HOST}/ directory exists."
        log_error "  Known hosts: $(find "${REPO_ROOT}/agents" -mindepth 1 -maxdepth 1 -type d \
            ! -name '_templates' -printf '%f ' 2>/dev/null || echo '(none)')"
        exit 1
    fi

    check_deps
    agamemnon_check_connection

    # Initialise temp file for planned changes and register fleet cleanup
    _PLAN_CHANGES_TMP="$(mktemp)"
    trap 'rm -f "$_PLAN_CHANGES_TMP"; cleanup_fleet_tmpdir' EXIT

    # NOTE: apply.sh fetches agents_json once and refreshes it after each
    # change (cache optimization). In dry-run / plan mode no changes are made,
    # so a single fetch is sufficient and the per-agent refresh is intentionally
    # skipped. (#96)
    local agents_json
    agents_json="$(agamemnon_list_agents)"

    local yaml_files
    mapfile -t yaml_files < <(get_agent_files "$HOST" "$FLEET_NAME")

    if [[ ${#yaml_files[@]} -eq 0 ]]; then
        if [[ "$OUTPUT_FORMAT" == "json" ]]; then
            _emit_json_plan 0
        else
            log_info "No agent YAML files found."
        fi
        exit 0
    fi

    local has_changes=0

    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        local prune_indicator=""
        [[ $PRUNE -eq 1 ]] && prune_indicator=" +prune"
        log_info "Plan for ${AGAMEMNON_URL} (dry-run${prune_indicator} — no changes will be made)"
        log_info "================================================================"
        log_info ""
    fi

    for yaml_file in "${yaml_files[@]}"; do
        plan_agent "$yaml_file" "$agents_json" || has_changes=1
    done

    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        # Report unmanaged agents (in Agamemnon but not in YAML).
        # When --fleet is active, scope the check to only agents that belong to the
        # fleet so agents managed by other fleets are not flagged as unmanaged.
        local scoped_agents_json
        if [[ -n "$FLEET_NAME" ]]; then
            local fleet_names_json
            fleet_names_json="$(for f in "${yaml_files[@]}"; do yq eval '.metadata.name' "$f"; done | jq -Rsc 'split("\n") | map(select(length > 0))')"
            scoped_agents_json="$(echo "$agents_json" | jq --argjson names "$fleet_names_json" '[.[] | select(.name as $n | $names | index($n) != null)]')"
        else
            scoped_agents_json="$agents_json"
        fi
        log_info ""
        log_info "Checking for unmanaged agents..."
        if [[ $PRUNE -eq 1 ]]; then
            local PRUNE_COUNT=0
            # Show which unmanaged agents would be pruned
            while IFS= read -r unmanaged_name; do
                log_warn "[-] PRUNE ${unmanaged_name} (unmanaged — would be hibernated and deleted)"
                PRUNE_COUNT=$((PRUNE_COUNT + 1))
                has_changes=1
            done < <(get_unmanaged_names "$scoped_agents_json" "${yaml_files[@]}")
        else
            plan_report_unmanaged "$scoped_agents_json" "${yaml_files[@]}"
        fi

        log_info ""
        if [[ $has_changes -eq 0 ]]; then
            log_info "No changes needed. Desired state matches actual state."
            exit 0
        else
            local summary="created=${CREATE_COUNT} updated=${UPDATE_COUNT} woken=${WAKE_COUNT} hibernated=${HIBERNATE_COUNT}"
            [[ $PRUNE -eq 1 ]] && summary="${summary} pruned=${PRUNE_COUNT:-0}"
            log_warn "Summary: ${summary}"
            log_warn "Changes would be made. Run ./scripts/apply.sh to apply."
            exit 1
        fi
    else
        _emit_json_plan "$has_changes"
        exit "$has_changes"
    fi
}

# Emit the JSON plan report and exit
_emit_json_plan() {
    local has_changes="$1"

    # Build planned_changes JSON array from NDJSON temp file
    local changes_json="[]"
    if [[ -s "$_PLAN_CHANGES_TMP" ]]; then
        changes_json="$(jq -s '.' "$_PLAN_CHANGES_TMP")"
    fi

    local timestamp
    timestamp="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

    jq -n \
        --arg timestamp "$timestamp" \
        --arg host "${HOST:-all}" \
        --arg url "${AGAMEMNON_URL:-http://localhost:8080}" \
        --argjson changes_needed "$has_changes" \
        --argjson agents_checked "$((CREATE_COUNT + UPDATE_COUNT + WAKE_COUNT + HIBERNATE_COUNT + UNCHANGED_COUNT))" \
        --argjson to_create "$CREATE_COUNT" \
        --argjson to_update "$UPDATE_COUNT" \
        --argjson to_wake "$WAKE_COUNT" \
        --argjson to_hibernate "$HIBERNATE_COUNT" \
        --argjson unchanged "$UNCHANGED_COUNT" \
        --argjson planned_changes "$changes_json" \
        '{
            timestamp:     $timestamp,
            host:          $host,
            agamemnon_url: $url,
            summary: {
                agents_checked: $agents_checked,
                changes_needed: ($changes_needed == 1),
                to_create:      $to_create,
                to_update:      $to_update,
                to_wake:        $to_wake,
                to_hibernate:   $to_hibernate,
                unchanged:      $unchanged
            },
            planned_changes: $planned_changes
        }'
}

plan_agent() {
    local yaml_file="$1"
    local agents_json="$2"

    # Parse YAML fields
    local fields
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="${value}"
    done < <(parse_agent_yaml "$yaml_file")

    local name="${fields[name]}"
    local desired_state="${fields[desiredState]:-active}"
    local label="${fields[label]:-}"
    local program="${fields[program]:-}"
    local workdir="${fields[workingDirectory]:-}"
    local args="${fields[programArgs]:-}"
    local desc="${fields[taskDescription]:-}"
    local tags="${fields[tags]:-}"
    local model="${fields[model]:-}"
    local owner="${fields[owner]:-}"
    local role="${fields[role]:-member}"
    local deploy_type="${fields[deploymentType]:-local}"

    # Look up in actual state
    local actual_json
    actual_json="$(echo "$agents_json" | jq -r --arg name "$name" \
        '.[] | select(.name == $name)')"

    if [[ -z "$actual_json" ]]; then
        if [[ "$OUTPUT_FORMAT" != "json" ]]; then
            log_info "[+] CREATE ${name} (program=${program}, deploy=${deploy_type})"
            if [[ "$desired_state" == "active" ]]; then
                log_info "    └─ WAKE after create"
            fi
        fi
        _plan_record_change "$name" "CREATE" \
            "program=${program} deploy=${deploy_type} desired_state=${desired_state}"
        ((CREATE_COUNT++))
        return 1
    fi

    local action
    action="$(compute_drift "$name" "$desired_state" "$actual_json" \
        "$label" "$program" "$workdir" "$args" "$desc" \
        "$tags" "$model" "$owner" "$role" "$deploy_type")"

    case "$action" in
        UNCHANGED)
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                log_info "[=] UNCHANGED ${name}"
            fi
            _plan_record_change "$name" "UNCHANGED" ""
            ((UNCHANGED_COUNT++))
            ;;
        WAKE)
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                log_warn "[!] WAKE ${name} (desired=active, actual=$(echo "$actual_json" | jq -r '.status'))"
            fi
            _plan_record_change "$name" "WAKE" \
                "desired=active actual=$(echo "$actual_json" | jq -r '.status')"
            ((WAKE_COUNT++))
            return 1
            ;;
        HIBERNATE)
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                log_warn "[z] HIBERNATE ${name} (desired=hibernated, actual=$(echo "$actual_json" | jq -r '.status'))"
            fi
            _plan_record_change "$name" "HIBERNATE" \
                "desired=hibernated actual=$(echo "$actual_json" | jq -r '.status')"
            ((HIBERNATE_COUNT++))
            return 1
            ;;
        UPDATE:*)
            local fields_changed="${action#UPDATE:}"
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                log_warn "[~] UPDATE ${name}: ${fields_changed} differ"
            fi
            _plan_record_change "$name" "UPDATE" "fields=${fields_changed}"
            ((UPDATE_COUNT++))
            return 1
            ;;
    esac

    return 0
}

main "$@"
