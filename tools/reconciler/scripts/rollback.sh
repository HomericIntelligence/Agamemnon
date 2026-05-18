#!/usr/bin/env bash
# scripts/rollback.sh — Restore the most recent pre-apply snapshot
#
# Reads the latest snapshot from .myrmidons/snapshots/ and restores all agents
# to their captured state via the Agamemnon API. Works even if YAML files have
# been modified since the apply that created the snapshot.
#
# Usage:
#   ./scripts/rollback.sh                      # Restore most recent snapshot
#   ./scripts/rollback.sh --list               # List available snapshots
#   ./scripts/rollback.sh --snapshot FILE      # Restore a specific snapshot
#   ./scripts/rollback.sh --snapshot-dir DIR   # Override snapshot directory
#
# Safety:
#   - Dry-run with --dry-run to preview what would change
#   - Validates snapshot JSON before applying
#   - Hibernates running agents before reconfiguring them

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=scripts/lib/api.sh
source "${SCRIPT_DIR}/lib/api.sh"
# shellcheck source=scripts/lib/report.sh
source "${SCRIPT_DIR}/lib/report.sh"

SNAPSHOT_DIR="${REPO_ROOT}/.myrmidons/snapshots"
SNAPSHOT_FILE=""
LIST_ONLY=0
DRY_RUN=0

RESTORED=0
CREATED=0
ERRORS=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --list)            LIST_ONLY=1; shift ;;
            --dry-run)         DRY_RUN=1; shift ;;
            --snapshot)        SNAPSHOT_FILE="$2"; shift 2 ;;
            --snapshot-dir)    SNAPSHOT_DIR="$2"; shift 2 ;;
            -h|--help)         usage; exit 0 ;;
            *) echo "ERROR: Unknown argument: $1" >&2; usage; exit 1 ;;
        esac
    done
}

usage() {
    cat <<EOF
Usage: $0 [--snapshot FILE] [--snapshot-dir DIR] [--list] [--dry-run]

Restores agents to their state captured in the most recent pre-apply snapshot.

Options:
  --list              List available snapshots and exit
  --snapshot FILE     Restore from a specific snapshot file (default: most recent)
  --snapshot-dir DIR  Directory where snapshots are stored (default: .myrmidons/snapshots)
  --dry-run           Show what would be restored without making changes
  -h, --help          Show this help

Examples:
  $0                                          # Restore most recent snapshot
  $0 --list                                   # List snapshots
  $0 --snapshot .myrmidons/snapshots/2024-01-15T10:30:00.json
EOF
}

list_snapshots() {
    if [[ ! -d "$SNAPSHOT_DIR" ]]; then
        echo "No snapshots directory found at: ${SNAPSHOT_DIR}"
        return 0
    fi

    local snapshots=()
    mapfile -t snapshots < <(find "$SNAPSHOT_DIR" -name "*.json" | sort -r)

    if [[ ${#snapshots[@]} -eq 0 ]]; then
        echo "No snapshots found in: ${SNAPSHOT_DIR}"
        return 0
    fi

    echo "Available snapshots (newest first):"
    echo ""
    local i=0
    for snap in "${snapshots[@]}"; do
        local basename
        basename="$(basename "$snap")"
        local agent_count
        # Support both new format {context:{}, agents:[]} and legacy plain array
        agent_count="$(jq 'if type == "object" and has("agents") then .agents | length elif type == "array" then length else 0 end' "$snap" 2>/dev/null || echo "?")"
        local marker=""
        [[ $i -eq 0 ]] && marker=" ← most recent"
        printf "  %s  (%s agents)%s\n" "$basename" "$agent_count" "$marker"
        i=$((i + 1))
    done
}

find_latest_snapshot() {
    if [[ ! -d "$SNAPSHOT_DIR" ]]; then
        echo "ERROR: No snapshots directory found at: ${SNAPSHOT_DIR}" >&2
        echo "  Run 'just apply' first to create a snapshot." >&2
        return 1
    fi

    local latest
    latest="$(find "$SNAPSHOT_DIR" -name "*.json" | sort -r | head -1)"

    if [[ -z "$latest" ]]; then
        echo "ERROR: No snapshots found in: ${SNAPSHOT_DIR}" >&2
        echo "  Run 'just apply' first to create a snapshot." >&2
        return 1
    fi

    echo "$latest"
}

validate_snapshot() {
    local snapshot_file="$1"

    if [[ ! -f "$snapshot_file" ]]; then
        echo "ERROR: Snapshot file not found: ${snapshot_file}" >&2
        return 1
    fi

    # Accept both new format {context:{},agents:[]} and legacy plain array
    if ! jq -e '(type == "array") or (type == "object" and has("agents") and (.agents | type == "array"))' \
            "$snapshot_file" > /dev/null 2>&1; then
        echo "ERROR: Snapshot is not a valid JSON array: ${snapshot_file}" >&2
        return 1
    fi
}

# Extract the agents array from a snapshot file.
# Handles both new {context,agents} format and legacy plain-array format.
# Usage: extract_snapshot_agents <snapshot_file>  → JSON array on stdout
extract_snapshot_agents() {
    local snapshot_file="$1"
    jq 'if type == "object" and has("agents") then .agents else . end' "$snapshot_file"
}

restore_agent() {
    local agent_json="$1"
    local current_agents_json="$2"

    local name label program workdir args desc status
    name="$(echo "$agent_json" | jq -r '.name // empty')"
    label="$(echo "$agent_json" | jq -r '.label // ""')"
    program="$(echo "$agent_json" | jq -r '.program // "claude-code"')"
    workdir="$(echo "$agent_json" | jq -r '.workingDirectory // ""')"
    args="$(echo "$agent_json" | jq -r '.programArgs // ""')"
    desc="$(echo "$agent_json" | jq -r '.taskDescription // ""')"
    status="$(echo "$agent_json" | jq -r '.status // "offline"')"

    if [[ -z "$name" ]]; then
        echo "  WARNING: Skipping agent with no name" >&2
        return 0
    fi

    local current_agent
    current_agent="$(echo "$current_agents_json" | jq -r --arg n "$name" '.[] | select(.name == $n)')"
    local current_id=""
    [[ -n "$current_agent" ]] && current_id="$(echo "$current_agent" | jq -r '.id')"

    if [[ "$DRY_RUN" -eq 1 ]]; then
        if [[ -z "$current_id" ]]; then
            echo "  [DRY-RUN] Would create: ${name}"
        else
            echo "  [DRY-RUN] Would restore: ${name} (status=${status})"
        fi
        return 0
    fi

    if [[ -z "$current_id" ]]; then
        # Agent no longer exists — recreate it
        echo "  [+] Recreating: ${name}..."
        local create_body
        create_body="$(echo "$agent_json" | jq '{
            name: .name,
            label: (.label // ""),
            program: (.program // "claude-code"),
            workingDirectory: (.workingDirectory // ""),
            programArgs: (.programArgs // ""),
            taskDescription: (.taskDescription // ""),
            tags: (.tags // []),
            owner: (.owner // ""),
            role: (.role // "member")
        }')"

        local result
        if result="$(agamemnon_create_agent "$create_body" 2>&1)"; then
            current_id="$(echo "$result" | jq -r '.id // empty')"
            echo "    Created: id=${current_id}"
            CREATED=$((CREATED + 1))
        else
            echo "    ERROR recreating ${name}: ${result}" >&2
            ERRORS=$((ERRORS + 1))
            return 0
        fi
    else
        # Agent exists — patch it back to snapshot state
        echo "  [~] Restoring: ${name}..."
        local patch_body
        patch_body="$(jq -n \
            --arg lbl "$label" \
            --arg program "$program" \
            --arg workingDirectory "$workdir" \
            --arg programArgs "$args" \
            --arg taskDescription "$desc" \
            '{label: $lbl, program: $program, workingDirectory: $workingDirectory,
              programArgs: $programArgs, taskDescription: $taskDescription}')"

        if ! agamemnon_update_agent "$current_id" "$patch_body" > /dev/null 2>&1; then
            echo "    ERROR updating ${name}" >&2
            ERRORS=$((ERRORS + 1))
            return 0
        fi
        echo "    Patched."
        RESTORED=$((RESTORED + 1))
    fi

    # Restore run state
    if [[ -n "$current_id" ]]; then
        local desired_active=0
        [[ "$status" == "active" || "$status" == "online" ]] && desired_active=1

        if [[ $desired_active -eq 1 ]]; then
            echo "    Starting ${name}..."
            if ! agamemnon_wake_agent "$current_id" > /dev/null; then
                echo "    warn: wake failed for ${name} (best-effort restore continues)" >&2
            fi
        else
            echo "    Hibernating ${name}..."
            if ! agamemnon_hibernate_agent "$current_id" > /dev/null; then
                echo "    warn: hibernate failed for ${name} (best-effort restore continues)" >&2
            fi
        fi
    fi
}

main() {
    parse_args "$@"

    if [[ $LIST_ONLY -eq 1 ]]; then
        list_snapshots
        exit 0
    fi

    # Resolve snapshot file
    if [[ -z "$SNAPSHOT_FILE" ]]; then
        SNAPSHOT_FILE="$(find_latest_snapshot)"
    fi

    validate_snapshot "$SNAPSHOT_FILE"

    local snapshot_basename
    snapshot_basename="$(basename "$SNAPSHOT_FILE")"

    echo "Rollback from snapshot: ${snapshot_basename}"
    echo "Snapshot dir: ${SNAPSHOT_DIR}"
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "(DRY RUN — no changes will be made)"
    fi
    echo "================================================"
    echo ""

    local snapshot_agents
    snapshot_agents="$(extract_snapshot_agents "$SNAPSHOT_FILE")"

    local agent_count
    agent_count="$(echo "$snapshot_agents" | jq 'length')"
    echo "Snapshot contains ${agent_count} agents."
    echo ""

    if [[ $DRY_RUN -eq 0 ]]; then
        agamemnon_check_connection
    fi

    local current_agents_json="{}"
    if [[ $DRY_RUN -eq 0 ]]; then
        current_agents_json="$(agamemnon_list_agents)"
    fi

    # (#225) Snapshot current state before restoring, to enable chained rollbacks.
    if [[ $DRY_RUN -eq 0 ]]; then
        local pre_rollback_snap
        pre_rollback_snap="$(snapshot_write "$current_agents_json" "$SNAPSHOT_DIR" "all" "pre-rollback")"
        echo "Pre-rollback snapshot saved: ${pre_rollback_snap}"
        echo ""
    fi

    # Restore each agent from the snapshot
    while IFS= read -r agent_json; do
        restore_agent "$agent_json" "$current_agents_json"
        # Refresh after each change
        if [[ $DRY_RUN -eq 0 ]]; then
            current_agents_json="$(agamemnon_list_agents)"
        fi
    done < <(echo "$snapshot_agents" | jq -c '.[]')

    echo ""
    echo "================================================"
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "Dry run complete. No changes made."
    else
        echo "Summary: restored=${RESTORED} created=${CREATED} errors=${ERRORS}"
    fi

    if [[ $ERRORS -gt 0 ]]; then
        exit 1
    fi
}

main "$@"
