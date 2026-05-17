#!/usr/bin/env bash
# scripts/lib/report.sh — JSON reconciliation report builder
#
# Provides functions to build, accumulate, and emit a structured JSON report
# of a reconciliation run. Used by apply.sh and status.sh.
#
# Report schema:
# {
#   "timestamp":      "<ISO-8601>",
#   "host":           "<hostname or 'all'>",
#   "agamemnon_url":  "<url>",
#   "summary": {
#     "created":    N,
#     "updated":    N,
#     "woken":      N,
#     "hibernated": N,
#     "unchanged":  N,
#     "pruned":     N,
#     "errors":     N
#   },
#   "agents": [
#     {
#       "name":    "<agent>",
#       "host":    "<host>",
#       "action":  "UNCHANGED|CREATE|UPDATE|WAKE|HIBERNATE|PRUNE|ERROR",
#       "status":  { "desired": "...", "actual": "..." },
#       "drift":   [ { "field": "...", "old": "...", "new": "..." }, ... ],
#       "error":   "<message or null>"
#     }
#   ],
#   "unmanaged": [ "<name>", ... ],
#   "convergence": {
#     "checked":  N,
#     "verified": N,
#     "failed":   N,
#     "agents": [
#       {
#         "name":          "<agent>",
#         "desired":       "active|hibernated",
#         "actual_status": "<status or 'not_found'>",
#         "converged":     true|false,
#         "reason":        "<description>"
#       }
#     ]
#   }
# }

set -euo pipefail

# ---------------------------------------------------------------------------
# Internal state — accumulated as the reconciliation run proceeds.
# ---------------------------------------------------------------------------

# Temporary file that accumulates per-agent JSON objects (one per line, NDJSON).
_REPORT_AGENTS_TMP=""
_REPORT_UNMANAGED_TMP=""
_REPORT_CONVERGENCE_TMP=""

# Initialise the report state.  Call once at the start of a reconciliation run.
# Usage: report_init [host]
report_init() {
    local host="${1:-all}"
    _REPORT_HOST="$host"
    _REPORT_TIMESTAMP="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    _REPORT_AGENTS_TMP="$(mktemp)"
    _REPORT_UNMANAGED_TMP="$(mktemp)"
    _REPORT_CONVERGENCE_TMP="$(mktemp)"
}

# Append one agent entry to the in-progress report.
# Usage: report_add_agent <name> <host> <action> <desired_state> <actual_state> \
#                         <drift_json_array> <error_or_empty>
#
# drift_json_array  — a JSON array string: '[{"field":"label","old":"A","new":"B"}]'
#                     Pass '[]' when there is no drift.
report_add_agent() {
    local name="$1"
    local host="$2"
    local action="$3"
    local desired_state="$4"
    local actual_state="$5"
    local drift_json="${6:-[]}"
    local error_msg="${7:-}"

    # Validate drift_json is at least a valid JSON array; fall back to [] on failure.
    if ! echo "$drift_json" | jq -e . > /dev/null 2>&1; then
        drift_json="[]"
    fi

    local error_val
    if [[ -n "$error_msg" ]]; then
        error_val="$(jq -n --arg e "$error_msg" '$e')"
    else
        error_val="null"
    fi

    jq -n \
        --arg name "$name" \
        --arg host "$host" \
        --arg action "$action" \
        --arg desired "$desired_state" \
        --arg actual "$actual_state" \
        --argjson drift "$drift_json" \
        --argjson error "$error_val" \
        '{name: $name, host: $host, action: $action,
          status: {desired: $desired, actual: $actual},
          drift: $drift, error: $error}' >> "$_REPORT_AGENTS_TMP"
}

# Record an unmanaged agent (present in Agamemnon but not in YAML).
# Usage: report_add_unmanaged <name>
report_add_unmanaged() {
    local name="$1"
    echo "$name" >> "$_REPORT_UNMANAGED_TMP"
}

# Append one convergence result entry for an agent.
# Usage: report_add_convergence <name> <desired> <actual_status> <converged_bool> <reason>
#   converged_bool — 0 for false, 1 for true
report_add_convergence() {
    local name="$1"
    local desired="$2"
    local actual_status="$3"
    local converged_bool="$4"
    local reason="$5"

    local converged_json
    if [[ "$converged_bool" == "1" ]]; then
        converged_json="true"
    else
        converged_json="false"
    fi

    jq -n \
        --arg name "$name" \
        --arg desired "$desired" \
        --arg actual_status "$actual_status" \
        --argjson converged "$converged_json" \
        --arg reason "$reason" \
        '{name: $name, desired: $desired, actual_status: $actual_status,
          converged: $converged, reason: $reason}' >> "$_REPORT_CONVERGENCE_TMP"
}

# Assemble and print the final JSON report to stdout.
# Usage: report_emit <created> <updated> <woken> <hibernated> <unchanged> <pruned> <errors>
report_emit() {
    local created="${1:-0}"
    local updated="${2:-0}"
    local woken="${3:-0}"
    local hibernated="${4:-0}"
    local unchanged="${5:-0}"
    local pruned="${6:-0}"
    local errors="${7:-0}"

    # Build agents JSON array from NDJSON temp file.
    local agents_json="[]"
    if [[ -s "$_REPORT_AGENTS_TMP" ]]; then
        agents_json="$(jq -s '.' "$_REPORT_AGENTS_TMP")"
    fi

    # Build unmanaged JSON array from temp file.
    local unmanaged_json="[]"
    if [[ -s "$_REPORT_UNMANAGED_TMP" ]]; then
        unmanaged_json="$(jq -Rn '[inputs]' < "$_REPORT_UNMANAGED_TMP")"
    fi

    # Build convergence JSON object from NDJSON temp file.
    local convergence_agents_json="[]"
    if [[ -s "$_REPORT_CONVERGENCE_TMP" ]]; then
        convergence_agents_json="$(jq -s '.' "$_REPORT_CONVERGENCE_TMP")"
    fi
    local conv_checked conv_verified conv_failed
    conv_checked="$(echo "$convergence_agents_json" | jq 'length')"
    conv_verified="$(echo "$convergence_agents_json" | jq '[.[] | select(.converged == true)] | length')"
    conv_failed="$(echo "$convergence_agents_json" | jq '[.[] | select(.converged == false)] | length')"

    jq -n \
        --arg timestamp "$_REPORT_TIMESTAMP" \
        --arg host "${_REPORT_HOST:-all}" \
        --arg url "${AGAMEMNON_URL:-http://localhost:8080}" \
        --argjson created "$created" \
        --argjson updated "$updated" \
        --argjson woken "$woken" \
        --argjson hibernated "$hibernated" \
        --argjson unchanged "$unchanged" \
        --argjson pruned "$pruned" \
        --argjson errors "$errors" \
        --argjson agents "$agents_json" \
        --argjson unmanaged "$unmanaged_json" \
        --argjson conv_checked "$conv_checked" \
        --argjson conv_verified "$conv_verified" \
        --argjson conv_failed "$conv_failed" \
        --argjson conv_agents "$convergence_agents_json" \
        '{
            timestamp:     $timestamp,
            host:          $host,
            agamemnon_url: $url,
            summary: {
                created:    $created,
                updated:    $updated,
                woken:      $woken,
                hibernated: $hibernated,
                unchanged:  $unchanged,
                pruned:     $pruned,
                errors:     $errors
            },
            agents:    $agents,
            unmanaged: $unmanaged,
            convergence: {
                checked:  $conv_checked,
                verified: $conv_verified,
                failed:   $conv_failed,
                agents:   $conv_agents
            }
        }'
}

# Write the report to the canonical last-reconciliation file and a timestamped copy.
# Usage: report_save <json_string> [report_dir]
#
# Writes two files:
#   reports/last-reconciliation.json         — always the most recent snapshot
#   reports/reconciliation-<timestamp>.json  — timestamped copy for history retention
report_save() {
    local json="$1"
    local report_dir="${2:-}"

    if [[ -z "$report_dir" ]]; then
        local repo_root
        repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
        report_dir="${repo_root}/reports"
    fi

    mkdir -p "$report_dir"

    local timestamp
    timestamp="$(date +%Y%m%d-%H%M%S)"
    local timestamped_file="${report_dir}/reconciliation-${timestamp}.json"

    # Write timestamped history copy
    printf '%s\n' "$json" > "$timestamped_file"

    # Write (or overwrite) the canonical latest snapshot
    printf '%s\n' "$json" > "${report_dir}/last-reconciliation.json"
}

# POST the report JSON to the Agamemnon webhook endpoint.
# Outputs a webhook_delivery JSON object to stdout indicating success/failure.
# Usage: report_webhook <json_string> <webhook_url>
# Output (stdout): JSON object — {"status":"success"|"failure","http_code":<N>}
report_webhook() {
    local json="$1"
    local webhook_url="$2"

    local http_code
    local _had_xtrace=0
    if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
    { set +x; } 2>/dev/null
    http_code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
        -X POST "$webhook_url" \
        -H 'Content-Type: application/json' \
        -d "$json" 2>/dev/null)" || http_code="0"
    if [[ $_had_xtrace -eq 1 ]]; then set -x; fi

    local status
    if [[ "$http_code" =~ ^2 ]]; then
        status="success"
        echo "Webhook delivered to ${webhook_url} (HTTP ${http_code})" >&2
    else
        status="failure"
        echo "WARNING: Webhook delivery failed to ${webhook_url} (HTTP ${http_code})" >&2
    fi

    jq -n --arg status "$status" --argjson code "${http_code:-0}" \
        '{status: $status, http_code: $code}'
}

# Clean up temp files.  Call after report_emit (or on EXIT).
report_cleanup() {
    [[ -n "${_REPORT_AGENTS_TMP:-}" && -f "$_REPORT_AGENTS_TMP" ]] && rm -f "$_REPORT_AGENTS_TMP"
    [[ -n "${_REPORT_UNMANAGED_TMP:-}" && -f "$_REPORT_UNMANAGED_TMP" ]] && rm -f "$_REPORT_UNMANAGED_TMP"
    [[ -n "${_REPORT_CONVERGENCE_TMP:-}" && -f "$_REPORT_CONVERGENCE_TMP" ]] && rm -f "$_REPORT_CONVERGENCE_TMP"
}

# ---------------------------------------------------------------------------
# Snapshot functions — capture agent state before apply or rollback (#228, #225)
# ---------------------------------------------------------------------------

# Write a snapshot of current agent state to the snapshot directory.
# Adds context fields: user, git_branch, host, timestamp.
#
# Usage: snapshot_write <agents_json> <snapshot_dir> <host> [suffix]
#   agents_json   — JSON array string of current agent objects from Agamemnon
#   snapshot_dir  — directory to write snapshot files into
#   host          — host argument passed to apply/rollback (or "all")
#   suffix        — optional filename suffix before .json (e.g. "pre-rollback")
#
# Outputs the path of the written snapshot file.
snapshot_write() {
    local agents_json="$1"
    local snapshot_dir="$2"
    local host="${3:-all}"
    local suffix="${4:-}"

    mkdir -p "$snapshot_dir"

    local timestamp git_branch run_user
    timestamp="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    git_branch="$(git -C "$(dirname "${BASH_SOURCE[0]}")/../.." rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")"
    run_user="${USER:-unknown}"

    local filename="${timestamp}"
    [[ -n "$suffix" ]] && filename="${timestamp}.${suffix}"
    local snapshot_file="${snapshot_dir}/${filename}.json"

    jq -n \
        --arg timestamp "$timestamp" \
        --arg git_branch "$git_branch" \
        --arg host "$host" \
        --arg user "$run_user" \
        --argjson agents "$agents_json" \
        '{
            context: {
                timestamp:  $timestamp,
                user:       $user,
                git_branch: $git_branch,
                host:       $host
            },
            agents: $agents
        }' > "$snapshot_file"

    echo "$snapshot_file"
}

# Prune old snapshots in a directory, keeping at most SNAPSHOT_KEEP files.
# Skips files that contain a suffix (e.g. pre-rollback) from pruning count
# to avoid deleting safety nets.
#
# Usage: snapshot_prune <snapshot_dir> [keep_count]
snapshot_prune() {
    local snapshot_dir="$1"
    local keep="${2:-${SNAPSHOT_KEEP:-10}}"

    [[ ! -d "$snapshot_dir" ]] && return 0

    # Only count/prune regular (non-suffixed) snapshots:
    # Suffixed snapshots (*.pre-rollback.json etc.) are retained separately.
    # The first pass keeps stems with no dots; we use awk for the inverted
    # match so an empty result returns rc=0 (grep -v returns 1 in that case,
    # which would fail under `set -o pipefail`).
    local regular_snapshots=()
    mapfile -t regular_snapshots < <(
        find "$snapshot_dir" -maxdepth 1 -name "*.json" \
            | awk -F/ '{stem=$NF; sub(/\.json$/,"",stem); if (index(stem,".")==0) print}' \
            | sort
        # fallback: match ISO timestamp pattern only (no dots in stem)
        find "$snapshot_dir" -maxdepth 1 -name "*T*Z.json" \
            | awk '/\/[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\.json$/' \
            | sort
    )

    # Deduplicate
    local -A seen=()
    local deduped=()
    for f in "${regular_snapshots[@]}"; do
        [[ -z "${seen[$f]:-}" ]] && deduped+=("$f") && seen["$f"]=1
    done

    local count=${#deduped[@]}
    if [[ $count -gt $keep ]]; then
        local excess=$(( count - keep ))
        # Remove oldest first (sort ascending, take head)
        printf '%s\n' "${deduped[@]}" | sort | head -n "$excess" | xargs rm -f
    fi
}

# Build a drift JSON array from an UPDATE:<fields> action string and the actual/desired values.
# Usage: build_drift_json <action_string> <actual_json> \
#                         <desired_label> <desired_program> <desired_workdir> \
#                         <desired_args> <desired_desc> <desired_tags_csv> \
#                         <desired_owner> <desired_role>
# Outputs a JSON array like: [{"field":"label","old":"X","new":"Y"}, ...]
build_drift_json() {
    local action="$1"
    local actual_json="$2"
    local desired_label="$3"
    local desired_program="$4"
    local desired_workdir="$5"
    local desired_args="$6"
    local desired_desc="$7"
    local desired_tags_csv="${8:-}"
    local desired_owner="${9:-}"
    local desired_role="${10:-}"

    if [[ "$action" != UPDATE:* ]]; then
        echo "[]"
        return
    fi

    local changed_fields="${action#UPDATE:}"
    IFS=',' read -ra fields <<< "$changed_fields"

    # Map desired values by field name
    declare -A desired_vals
    desired_vals["label"]="$desired_label"
    desired_vals["program"]="$desired_program"
    desired_vals["workingDirectory"]="$(normalize_path "$desired_workdir")"
    desired_vals["programArgs"]="$desired_args"
    desired_vals["taskDescription"]="$desired_desc"
    desired_vals["tags"]="$desired_tags_csv"
    desired_vals["owner"]="$desired_owner"
    desired_vals["role"]="$desired_role"

    # Build JSON array using jq
    local entries="[]"
    for field in "${fields[@]}"; do
        local old_val new_val

        case "$field" in
            label)            old_val="$(echo "$actual_json" | jq -r '.label // ""')" ;;
            program)          old_val="$(echo "$actual_json" | jq -r '.program // ""')" ;;
            workingDirectory) old_val="$(normalize_path "$(echo "$actual_json" | jq -r '.workingDirectory // ""')")" ;;
            programArgs)      old_val="$(echo "$actual_json" | jq -r '.programArgs // ""')" ;;
            taskDescription)  old_val="$(echo "$actual_json" | jq -r '.taskDescription // ""')" ;;
            tags)             old_val="$(echo "$actual_json" | jq -r '.tags // [] | sort | join(",")')" ;;
            owner)            old_val="$(echo "$actual_json" | jq -r '.owner // ""')" ;;
            role)             old_val="$(echo "$actual_json" | jq -r '.role // ""')" ;;
            *)                old_val="" ;;
        esac

        new_val="${desired_vals[$field]:-}"

        entries="$(jq -n \
            --argjson arr "$entries" \
            --arg field "$field" \
            --arg old "$old_val" \
            --arg new "$new_val" \
            '$arr + [{"field": $field, "old": $old, "new": $new}]')"
    done

    echo "$entries"
}
