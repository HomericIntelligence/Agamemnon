#!/usr/bin/env bash
# scripts/diff.sh — Field-level diff: YAML desired state vs Agamemnon actual state
#
# Shows a detailed, field-by-field comparison for each drifted agent.
# Output is similar to `terraform plan`: old → new values side-by-side.
# Agents with no drift produce no output (zero output = clean).
#
# Usage:
#   ./scripts/diff.sh                      # Diff all agents on all hosts
#   ./scripts/diff.sh hermes               # Diff agents for a specific host
#   ./scripts/diff.sh --host hermes        # Same as above
#   ./scripts/diff.sh --agent my-agent     # Diff a specific agent
#   ./scripts/diff.sh hermes --agent foo   # Combine host + agent filters
#
# Exit codes:
#   0 = no drift (all agents match)
#   1 = drift detected (or error)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=scripts/lib/api.sh
source "${SCRIPT_DIR}/lib/api.sh"
# shellcheck source=scripts/lib/reconcile.sh
source "${SCRIPT_DIR}/lib/reconcile.sh"

HOST=""
AGENT_FILTER=""

# ---------------------------------------------------------------------------
# Color support
# ---------------------------------------------------------------------------

_color_enabled() {
    [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]
}

RED=""
GREEN=""
YELLOW=""
BOLD=""
RESET=""

if _color_enabled; then
    RED="\033[0;31m"
    GREEN="\033[0;32m"
    YELLOW="\033[1;33m"
    BOLD="\033[1m"
    RESET="\033[0m"
fi

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --host)    HOST="$2"; shift 2 ;;
            --agent)   AGENT_FILTER="$2"; shift 2 ;;
            -h|--help) usage; exit 0 ;;
            *)         HOST="$1"; shift ;;
        esac
    done
}

usage() {
    echo "Usage: $0 [host] [--host <host>] [--agent <name>]"
    echo ""
    echo "Shows field-level differences between desired state (YAML) and"
    echo "actual state (Agamemnon API) for each drifted agent."
    echo ""
    echo "Options:"
    echo "  host             Filter by host (positional or --host)"
    echo "  --host <host>    Filter by host"
    echo "  --agent <name>   Show diff for a single agent only"
    echo "  --help           Show this help"
    echo ""
    echo "Output:"
    echo "  Zero output when no drift exists."
    echo "  Field changes shown as:  field: \"old\" → \"new\""
    echo ""
    echo "Exit codes:"
    echo "  0 = no drift"
    echo "  1 = drift detected"
    echo ""
    echo "Examples:"
    echo "  $0                     # All hosts"
    echo "  $0 hermes              # Only hermes"
    echo "  $0 --agent my-agent    # One agent across all hosts"
    echo "  NO_COLOR=1 $0          # Disable color output"
}

# ---------------------------------------------------------------------------
# Tag diff helpers
# ---------------------------------------------------------------------------

# Print added/removed items in a tag comparison.
# Usage: diff_tags <desired_csv> <actual_csv>
diff_tags() {
    local desired_csv="$1"
    local actual_csv="$2"

    # Build sorted arrays
    local desired_tags=()
    local actual_tags=()

    if [[ -n "$desired_csv" ]]; then
        mapfile -t desired_tags < <(echo "$desired_csv" | tr ',' '\n' | sort)
    fi
    if [[ -n "$actual_csv" ]]; then
        mapfile -t actual_tags < <(echo "$actual_csv" | tr ',' '\n' | sort)
    fi

    # Added (in desired, not in actual)
    for tag in "${desired_tags[@]+"${desired_tags[@]}"}"; do
        local found=0
        for at in "${actual_tags[@]+"${actual_tags[@]}"}"; do
            [[ "$tag" == "$at" ]] && found=1 && break
        done
        if [[ $found -eq 0 ]]; then
            printf "        ${GREEN}+ %s${RESET}\n" "$tag"
        fi
    done

    # Removed (in actual, not in desired)
    for tag in "${actual_tags[@]+"${actual_tags[@]}"}"; do
        local found=0
        for dt in "${desired_tags[@]+"${desired_tags[@]}"}"; do
            [[ "$tag" == "$dt" ]] && found=1 && break
        done
        if [[ $found -eq 0 ]]; then
            printf "        ${RED}- %s${RESET}\n" "$tag"
        fi
    done
}

# ---------------------------------------------------------------------------
# Per-agent diff
# ---------------------------------------------------------------------------

# Print field-level diff for one agent. Returns 1 if drift was found.
diff_agent() {
    local yaml_file="$1"
    local agents_json="$2"

    # Parse YAML fields
    declare -A fields
    while IFS='=' read -r key value; do
        fields["$key"]="${value}"
    done < <(parse_agent_yaml "$yaml_file")

    local name="${fields[name]}"
    local desired_state="${fields[desiredState]:-active}"
    local desired_label="${fields[label]:-}"
    local desired_program="${fields[program]:-}"
    local desired_workdir
    desired_workdir="$(normalize_path "${fields[workingDirectory]:-}")"
    local desired_args="${fields[programArgs]:-}"
    local desired_desc="${fields[taskDescription]:-}"
    local desired_tags_csv="${fields[tags]:-}"

    # Apply --agent filter
    if [[ -n "$AGENT_FILTER" && "$name" != "$AGENT_FILTER" ]]; then
        return 0
    fi

    # Look up actual state
    local actual_json
    actual_json="$(echo "$agents_json" | jq -r --arg name "$name" \
        '.[] | select(.name == $name)')"

    if [[ -z "$actual_json" ]]; then
        printf "${BOLD}${GREEN}[+] %s${RESET} — not in Agamemnon (would be created)\n" "$name"
        printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "label:"            "$desired_label"
        printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "program:"          "$desired_program"
        printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "workingDirectory:" "$desired_workdir"
        if [[ -n "$desired_args" ]]; then
            printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "programArgs:"  "$desired_args"
        fi
        if [[ -n "$desired_desc" ]]; then
            printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "taskDescription:" "$desired_desc"
        fi
        if [[ -n "$desired_tags_csv" ]]; then
            printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "tags:"         "$desired_tags_csv"
        fi
        printf "    %-20s ${GREEN}\"%s\"${RESET}\n" "desiredState:"     "$desired_state"
        echo ""
        return 1
    fi

    local actual_status
    actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

    # Check lifecycle drift first
    if [[ "$desired_state" == "active" && "$actual_status" == "offline" ]]; then
        printf "${BOLD}${YELLOW}[!] %s${RESET} — needs wake (desired=active, actual=offline)\n" "$name"
        return 1
    fi
    if [[ "$desired_state" == "hibernated" ]] && \
       [[ "$actual_status" == "active" || "$actual_status" == "online" ]]; then
        printf "${BOLD}${YELLOW}[z] %s${RESET} — needs hibernate (desired=hibernated, actual=%s)\n" \
            "$name" "$actual_status"
        return 1
    fi

    # Extract actual field values
    local actual_label actual_program actual_workdir actual_args actual_desc actual_tags
    actual_label="$(echo "$actual_json" | jq -r '.label // ""')"
    actual_program="$(echo "$actual_json" | jq -r '.program // ""')"
    actual_workdir="$(normalize_path "$(echo "$actual_json" | jq -r '.workingDirectory // ""')")"
    actual_args="$(echo "$actual_json" | jq -r '.programArgs // ""')"
    actual_desc="$(echo "$actual_json" | jq -r '.taskDescription // ""')"
    actual_tags="$(echo "$actual_json" | jq -r '.tags // [] | sort | join(",")')"

    # Sort desired tags for stable comparison
    local desired_tags_sorted=""
    if [[ -n "$desired_tags_csv" ]]; then
        desired_tags_sorted="$(echo "$desired_tags_csv" | tr ',' '\n' | sort | tr '\n' ',' | sed 's/,$//')"
    fi

    # Collect drifted fields
    local has_drift=0
    local drift_lines=()

    _check_field_drift() {
        local field="$1" desired="$2" actual="$3"
        if [[ "$desired" != "$actual" ]]; then
            drift_lines+=("$(printf "    %-20s ${RED}\"%s\"${RESET} → ${GREEN}\"%s\"${RESET}" \
                "${field}:" "$actual" "$desired")")
            has_drift=1
        fi
    }

    _check_field_drift "label"            "$desired_label"         "$actual_label"
    _check_field_drift "program"          "$desired_program"       "$actual_program"
    _check_field_drift "workingDirectory" "$desired_workdir"       "$actual_workdir"
    _check_field_drift "programArgs"      "$desired_args"          "$actual_args"
    _check_field_drift "taskDescription"  "$desired_desc"          "$actual_desc"

    # Tags are handled separately
    local tags_differ=0
    if [[ "$desired_tags_sorted" != "$actual_tags" ]]; then
        tags_differ=1
        has_drift=1
    fi

    if [[ $has_drift -eq 0 ]]; then
        return 0
    fi

    # Print agent header
    printf "${BOLD}[~] %s${RESET}\n" "$name"

    # Print scalar field diffs
    for line in "${drift_lines[@]+"${drift_lines[@]}"}"; do
        echo -e "$line"
    done

    # Print tag diff if needed
    if [[ $tags_differ -eq 1 ]]; then
        printf "    %-20s\n" "tags:"
        diff_tags "$desired_tags_sorted" "$actual_tags"
    fi

    echo ""
    return 1
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

main() {
    parse_args "$@"
    check_deps
    agamemnon_check_connection

    local agents_json
    agents_json="$(agamemnon_list_agents)"

    local yaml_files
    mapfile -t yaml_files < <(get_agent_files "$HOST")

    if [[ ${#yaml_files[@]} -eq 0 ]]; then
        echo "No agent YAML files found."
        exit 0
    fi

    local has_drift=0

    for yaml_file in "${yaml_files[@]}"; do
        diff_agent "$yaml_file" "$agents_json" || has_drift=1
    done

    if [[ $has_drift -eq 0 ]]; then
        echo "No drift detected. Desired state matches actual state."
        exit 0
    else
        exit 1
    fi
}

main "$@"
