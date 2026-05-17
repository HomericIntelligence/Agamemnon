#!/usr/bin/env bash
# scripts/lib/reconcile.sh — diff and reconciliation logic
#
# Provides functions used by apply.sh and plan.sh.
# Parses YAML agent definitions, compares with actual Agamemnon state,
# and produces a list of actions to take.
#
# Requires: yq (YAML parser), jq, source of api.sh

set -euo pipefail

# Check required tools
check_deps() {
    local missing=()
    for cmd in yq jq curl; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required tools: ${missing[*]}"
        log_error "  Install yq: https://github.com/mikefarah/yq"
        log_error "  Install jq: apt install jq / brew install jq"
        return 1
    fi
}

# Parse a single agent YAML file. Outputs key=value lines for each field.
# Usage: parse_agent_yaml /path/to/agent.yaml
parse_agent_yaml() {
    local file="$1"

    yq eval '{
        "name": .metadata.name,
        "host": .metadata.host,
        "label": .spec.label,
        "program": .spec.program,
        "model": (.spec.model // ""),
        "workingDirectory": .spec.workingDirectory,
        "programArgs": (.spec.programArgs // ""),
        "taskDescription": (.spec.taskDescription // ""),
        "tags": (.spec.tags // [] | join(",")),
        "owner": (.spec.owner // ""),
        "role": (.spec.role // "member"),
        "deploymentType": (.spec.deployment.type // "local"),
        "dockerImage": (.spec.deployment.docker.image // ""),
        "dockerCpus": (.spec.deployment.docker.cpus // ""),
        "dockerMemory": (.spec.deployment.docker.memory // ""),
        "desiredState": (.spec.desiredState // "active")
    } | to_entries[] | .key + "=" + (.value | tostring)' "$file"
}

# Validate a host parameter: alphanumeric, hyphens, and underscores only.
# Rejects path separators and dot sequences that could enable path traversal.
# Usage: validate_host <host>
validate_host() {
    local host="$1"
    if [[ -z "$host" ]]; then
        return 0  # Empty host is valid (means "all hosts")
    fi
    if [[ ! "$host" =~ ^[a-zA-Z0-9_-]+$ ]]; then
        echo "ERROR: Invalid host '${host}': must contain only alphanumeric characters, hyphens, or underscores." >&2
        return 1
    fi
}

# Get all agent YAML files for a given host and/or fleet.
# When fleet_name is provided, only the agents in that fleet are returned.
# When fleet_name is empty, all agents/ files plus all fleet members are returned.
# Deduplicates output so a given agent path is only returned once.
# Note: FLEET_TMPDIR for inline agents is set inside a process-substitution
#       subshell when called via mapfile < <(...). Temp-file cleanup is
#       best-effort via cleanup_fleet_tmpdir in the caller's EXIT trap.
# Usage: get_agent_files [host] [fleet_name]
get_agent_files() {
    local host="${1:-}"
    local fleet_name="${2:-}"
    local repo_root
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

    validate_host "$host" || return 1

    # If scoped to a single fleet, resolve only that fleet's agents
    if [[ -n "$fleet_name" ]]; then
        local fleet_file
        fleet_file="$(find_fleet_file "$fleet_name")" || return 1
        resolve_fleet_files "$fleet_file"
        return
    fi

    # General case: collect agents/ files and all fleet members, deduplicating.
    # We output paths as we find them and track seen ones via an associative array.
    declare -A _gaf_seen

    # 1. Gather direct Agent YAML files from agents/
    # `find` returns rc>0 when the search root does not exist (e.g. a host
    # directory is removed). That is a legitimate "no files" outcome here, so
    # we guard on existence and treat a missing root as an empty result set.
    local raw_files=()
    local search_root
    if [[ -n "$host" ]]; then
        search_root="${repo_root}/agents/${host}"
    else
        search_root="${repo_root}/agents"
    fi
    if [[ -d "$search_root" ]]; then
        mapfile -t raw_files < <(
            find "$search_root" -name "*.yaml" \
                ! -path "*/_templates/*"
        )
    fi

    for f in "${raw_files[@]+"${raw_files[@]}"}"; do
        local kind
        kind="$(yq eval '.kind // ""' "$f" 2>/dev/null)"
        # Skip any Fleet files that may live under agents/; only emit Agent files
        if [[ "$kind" != "Fleet" ]]; then
            if [[ -z "${_gaf_seen[$f]+x}" ]]; then
                _gaf_seen[$f]=1
                echo "$f"
            fi
        fi
    done

    # 2. Discover Fleet YAML files in fleets/ and resolve their members
    if [[ -d "${repo_root}/fleets" ]]; then
        local fleet_files=()
        mapfile -t fleet_files < <(find "${repo_root}/fleets" -name "*.yaml" 2>/dev/null | sort)
        for fleet_file in "${fleet_files[@]+"${fleet_files[@]}"}"; do
            local fleet_kind
            fleet_kind="$(yq eval '.kind // ""' "$fleet_file" 2>/dev/null)"
            if [[ "$fleet_kind" != "Fleet" ]]; then
                continue
            fi

            # If host filter is set, only process fleets whose metadata.host matches
            if [[ -n "$host" ]]; then
                local fleet_host
                fleet_host="$(yq eval '.metadata.host // ""' "$fleet_file" 2>/dev/null)"
                if [[ "$fleet_host" != "$host" ]]; then
                    continue
                fi
            fi

            while IFS= read -r agent_file; do
                if [[ -z "${_gaf_seen[$agent_file]+x}" ]]; then
                    _gaf_seen[$agent_file]=1
                    echo "$agent_file"
                fi
            done < <(resolve_fleet_files "$fleet_file")
        done
    fi
}

# Find a fleet YAML file by name.
# Searches the fleets/ directory for a file where metadata.name matches.
# Outputs the absolute path or exits with error if not found.
# Usage: find_fleet_file <fleet-name>
find_fleet_file() {
    local fleet_name="$1"
    local repo_root
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

    local fleet_file=""
    while IFS= read -r -d '' f; do
        local name
        name="$(yq eval '.metadata.name // ""' "$f" 2>/dev/null)"
        if [[ "$name" == "$fleet_name" ]]; then
            fleet_file="$f"
            break
        fi
    done < <(find "${repo_root}/fleets" -name "*.yaml" -print0 2>/dev/null)

    if [[ -z "$fleet_file" ]]; then
        echo "ERROR: Fleet '${fleet_name}' not found in fleets/" >&2
        return 1
    fi
    echo "$fleet_file"
}

# Resolve a fleet YAML into a list of agent YAML file paths.
# Refs (ref: host/agent-name) are resolved to existing agent files.
# Inline agents are written to temp files (caller must clean up FLEET_TMPDIR).
# Sets global FLEET_TMPDIR if inline agents are created.
# Usage: resolve_fleet_files <fleet-yaml-path> [host-filter]
# When host-filter is non-empty, only refs whose host portion matches are emitted.
# Outputs one file path per line.
resolve_fleet_files() {
    local fleet_file="$1"
    local host_filter="${2:-}"
    local repo_root
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

    # Trap to clean up any FLEET_TMPDIR we create on error exit (#158)
    _resolve_fleet_cleanup() {
        if [[ -n "${FLEET_TMPDIR:-}" && -d "${FLEET_TMPDIR}" ]]; then
            rm -rf "${FLEET_TMPDIR}"
            FLEET_TMPDIR=""
        fi
    }
    trap '_resolve_fleet_cleanup' ERR

    local fleet_host
    fleet_host="$(yq eval '.metadata.host // ""' "$fleet_file")"

    local agent_count
    agent_count="$(yq eval '.spec.agents | length' "$fleet_file")"

    for (( i=0; i<agent_count; i++ )); do
        local ref
        ref="$(yq eval ".spec.agents[${i}].ref // \"\"" "$fleet_file")"

        if [[ -n "$ref" ]]; then
            # Resolve ref: host/agent-name -> agents/<host>/<name>.yaml
            local ref_host ref_name
            ref_host="${ref%%/*}"
            ref_name="${ref#*/}"

            # Apply host filter: skip refs that don't match the requested host
            if [[ -n "$host_filter" && "$ref_host" != "$host_filter" ]]; then
                continue
            fi

            local agent_file="${repo_root}/agents/${ref_host}/${ref_name}.yaml"
            if [[ ! -f "$agent_file" ]]; then
                echo "ERROR: Fleet ref '${ref}' not found — expected file at ${agent_file}" >&2
                return 1
            fi
            echo "$agent_file"
        else
            # Inline agent definition — extract and write to a temp file
            # Apply host filter: inline agents inherit the fleet's host
            if [[ -n "$host_filter" && "$fleet_host" != "$host_filter" ]]; then
                continue
            fi

            if [[ -z "${FLEET_TMPDIR:-}" ]]; then
                FLEET_TMPDIR="$(mktemp -d)"
            fi

            local inline_name
            inline_name="$(yq eval ".spec.agents[${i}].name // \"\"" "$fleet_file")"
            if [[ -z "$inline_name" ]]; then
                echo "ERROR: Inline agent at index ${i} in fleet has no name" >&2
                return 1
            fi

            # Use the fleet's host for the inline agent metadata
            local tmp_file="${FLEET_TMPDIR}/${inline_name}.yaml"
            yq eval ".spec.agents[${i}] | {
                \"apiVersion\": \"myrmidons/v1\",
                \"kind\": \"Agent\",
                \"metadata\": {
                    \"name\": .name,
                    \"host\": \"${fleet_host}\"
                },
                \"spec\": {
                    \"label\": (.label // .name),
                    \"program\": (.program // \"claude-code\"),
                    \"model\": (.model // null),
                    \"workingDirectory\": .workingDirectory,
                    \"programArgs\": (.programArgs // \"\"),
                    \"taskDescription\": (.taskDescription // \"\"),
                    \"tags\": (.tags // []),
                    \"owner\": (.owner // \"\"),
                    \"role\": (.role // \"member\"),
                    \"deployment\": (.deployment // {\"type\": \"local\"}),
                    \"desiredState\": (.desiredState // \"active\")
                }
            }" "$fleet_file" > "$tmp_file"
            echo "$tmp_file"
        fi
    done
}

# Clean up temp files created by resolve_fleet_files.
# Usage: cleanup_fleet_tmpdir
cleanup_fleet_tmpdir() {
    if [[ -n "${FLEET_TMPDIR:-}" && -d "${FLEET_TMPDIR}" ]]; then
        rm -rf "${FLEET_TMPDIR}"
        FLEET_TMPDIR=""
    fi
}

# Build a JSON create body from parsed YAML fields.
# Usage: build_create_json name label program workingDirectory programArgs taskDescription tags owner role model deploymentType
build_create_json() {
    local name="$1" label="$2" program="$3" workdir="$4"
    local args="$5" desc="$6" tags_csv="$7" owner="$8" role="$9"
    local model="${10:-}" deploy_type="${11:-local}"

    # Convert comma-separated tags to JSON array
    local tags_json
    if [[ -z "$tags_csv" ]]; then
        tags_json="[]"
    else
        tags_json="$(echo "$tags_csv" | jq -Rc 'split(",")')"
    fi

    # model may be null/empty; represent as JSON null when absent
    local model_json
    if [[ -z "$model" ]]; then
        model_json="null"
    else
        model_json="$(jq -n --arg m "$model" '$m')"
    fi

    # Note: $label is a reserved keyword in jq 1.6 (label-break syntax).
    # Use $lbl as the variable name to avoid the parser conflict.
    jq -n \
        --arg name "$name" \
        --arg lbl "$label" \
        --arg program "$program" \
        --arg workingDirectory "$workdir" \
        --arg programArgs "$args" \
        --arg taskDescription "$desc" \
        --argjson tags "$tags_json" \
        --arg owner "$owner" \
        --arg role "$role" \
        --argjson model "$model_json" \
        --arg deploymentType "$deploy_type" \
        '{
            name: $name,
            label: $lbl,
            program: $program,
            workingDirectory: $workingDirectory,
            programArgs: $programArgs,
            taskDescription: $taskDescription,
            tags: $tags,
            owner: $owner,
            role: $role,
            model: $model,
            deployment: {type: $deploymentType}
        }'
}

# Compare desired agent state (YAML fields) with actual state (JSON from API).
# Outputs: "UNCHANGED", "CREATE", "UPDATE:<field1>,<field2>...", "WAKE", "HIBERNATE"
#
# Positional parameters:
#   $1  name              — agent name (for context)
#   $2  desired_state     — "active" | "hibernated"
#   $3  actual_json       — full agent JSON from API
#   $4  desired_label
#   $5  desired_program
#   $6  desired_workdir
#   $7  desired_args
#   $8  desired_desc
#   $9  desired_tags_csv
#   $10 desired_model
#   $11 desired_owner
#   $12 desired_role
#   $13 desired_deploy_type
compute_drift() {
    if [[ $# -ne 13 ]]; then
        echo "compute_drift requires exactly 13 arguments, got $#" >&2
        return 1
    fi
    local name="$1"
    local desired_state="$2"    # "active" | "hibernated"
    local actual_json="$3"      # Full agent JSON from API

    local actual_status
    actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

    # Check if wake/hibernate action is needed
    if [[ "$desired_state" == "active" && "$actual_status" == "offline" ]]; then
        echo "WAKE"
        return
    fi
    if [[ "$desired_state" == "hibernated" ]] && \
       [[ "$actual_status" == "active" || "$actual_status" == "online" ]]; then
        echo "HIBERNATE"
        return
    fi

    # Check field-level drift (simplified: check key fields)
    local drifted_fields=()

    local actual_label actual_program actual_workdir actual_args actual_desc actual_tags_sorted
    local actual_model actual_owner actual_role actual_deploy_type
    actual_label="$(echo "$actual_json" | jq -r '.label // ""')"
    actual_program="$(echo "$actual_json" | jq -r '.program // ""')"
    actual_workdir="$(echo "$actual_json" | jq -r '.workingDirectory // ""')"
    actual_args="$(echo "$actual_json" | jq -r '.programArgs // ""')"
    actual_desc="$(echo "$actual_json" | jq -r '.taskDescription // ""')"
    # Tags: sorted comma-joined for stable comparison
    actual_tags_sorted="$(echo "$actual_json" | jq -r '.tags // [] | sort | join(",")')"
    # Normalize null model to empty string to avoid false positives
    actual_model="$(echo "$actual_json" | jq -r '.model // ""')"
    actual_owner="$(echo "$actual_json" | jq -r '.owner // ""')"
    actual_role="$(echo "$actual_json" | jq -r '.role // ""')"
    actual_deploy_type="$(echo "$actual_json" | jq -r '.deployment.type // "local"')"

    # These are passed as positional args from the caller
    local desired_label="$4"
    local desired_program="$5"
    local desired_workdir="$6"
    local desired_args="$7"
    local desired_desc="$8"
    local desired_tags_csv="${9:-}"
    local desired_model="${10:-}"
    local desired_owner="${11:-}"
    local desired_role="${12:-}"
    local desired_deploy_type="${13:-local}"

    # Normalize tilde paths before comparison
    actual_workdir="$(normalize_path "$actual_workdir")"
    desired_workdir="$(normalize_path "$desired_workdir")"

    # Sort desired tags for stable comparison
    local desired_tags_sorted=""
    if [[ -n "$desired_tags_csv" ]]; then
        desired_tags_sorted="$(echo "$desired_tags_csv" | tr ',' '\n' | sort | tr '\n' ',' | sed 's/,$//')"
    fi

    [[ "$actual_label" != "$desired_label" ]] && drifted_fields+=("label")
    [[ "$actual_program" != "$desired_program" ]] && drifted_fields+=("program")
    [[ "$actual_workdir" != "$desired_workdir" ]] && drifted_fields+=("workingDirectory")
    [[ "$actual_args" != "$desired_args" ]] && drifted_fields+=("programArgs")
    [[ "$actual_desc" != "$desired_desc" ]] && drifted_fields+=("taskDescription")
    [[ "$actual_tags_sorted" != "$desired_tags_sorted" ]] && drifted_fields+=("tags")
    [[ "$actual_model" != "$desired_model" ]] && drifted_fields+=("model")
    [[ "$actual_owner" != "$desired_owner" ]] && drifted_fields+=("owner")
    [[ "$actual_role" != "$desired_role" ]] && drifted_fields+=("role")
    [[ "$actual_deploy_type" != "$desired_deploy_type" ]] && drifted_fields+=("deploymentType")

    if [[ ${#drifted_fields[@]} -gt 0 ]]; then
        local joined
        joined="$(IFS=','; echo "${drifted_fields[*]}")"
        echo "UPDATE:${joined}"
    else
        echo "UNCHANGED"
    fi
}

# Expand ~ to $HOME so path comparisons are stable regardless of how the
# path was entered (e.g. "~/foo" vs "/home/mvillmow/foo").
normalize_path() {
    local p="$1"
    echo "${p/#\~/$HOME}"
}

# Report agents that exist in Agamemnon but are not managed by any YAML file.
# Prints a log_warn line for each unmanaged agent.
# Usage: report_unmanaged <agents_json> <yaml_file>...
report_unmanaged() {
    local agents_json="$1"
    # Guard: nothing to check if Agamemnon returned no agents
    [[ -z "$agents_json" || "$agents_json" == '[]' ]] && return 0
    shift
    # Guard: nothing to check if no YAML files are managed (#130)
    [[ $# -eq 0 ]] && return 0
    while IFS= read -r actual_name; do
        log_warn "[-] UNMANAGED ${actual_name} (in Agamemnon but not in desired state — use --prune to remove)"
    done < <(get_unmanaged_names "$agents_json" "$@")
}

# Find agent names that exist in Agamemnon but are not managed by any YAML file.
# Outputs one unmanaged agent name per line.
# Usage: get_unmanaged_names <agents_json> <yaml_file>...
get_unmanaged_names() {
    local agents_json="$1"
    # Guard: nothing to check if Agamemnon returned no agents
    [[ -z "$agents_json" || "$agents_json" == '[]' ]] && return 0
    shift
    local yaml_files=("$@")

    # Collect managed names from YAML files; if no files provided, all agents are unmanaged.
    local managed_names=()
    if [[ ${#yaml_files[@]} -gt 0 ]]; then
        while IFS= read -r n; do
            [[ -n "$n" ]] && managed_names+=("$n")
        done < <(yq eval '.metadata.name' "${yaml_files[@]}")
    fi

    # Emit names present in Agamemnon but absent from managed list
    while IFS= read -r actual_name; do
        local is_managed=0
        for mn in "${managed_names[@]}"; do
            [[ "$mn" == "$actual_name" ]] && is_managed=1 && break
        done
        if [[ $is_managed -eq 0 ]]; then
            echo "$actual_name"
        fi
    done < <(echo "$agents_json" | jq -r '.[].name')
}
