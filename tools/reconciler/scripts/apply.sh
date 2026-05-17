#!/usr/bin/env bash
# scripts/apply.sh — Reconcile desired state → actual via Agamemnon API
#
# The core GitOps reconciliation loop. Reads agent YAML files and ensures
# Agamemnon matches the desired state. All changes go through the REST API.
#
# Usage:
#   ./scripts/apply.sh                         # Apply all agents on all hosts
#   ./scripts/apply.sh hermes                  # Apply agents for a specific host
#   ./scripts/apply.sh --fleet dev-mesh
#   ./scripts/apply.sh --prune                 # Also hibernate+delete unmanaged agents
#   ./scripts/apply.sh --dry-run               # Same as plan.sh
#   ./scripts/apply.sh --yes                   # Skip interactive confirmation prompt
#   ./scripts/apply.sh --output json           # Emit JSON reconciliation report to stdout
#   ./scripts/apply.sh --webhook <url>         # POST report to webhook URL after apply
#   ./scripts/apply.sh --fail-fast             # Stop on first error
#   ./scripts/apply.sh --retry                 # Re-apply only agents from failed-agents.txt
#
# Safety:
#   - Never auto-deletes agents without --prune flag
#   - Always hibernates before deleting
#   - Prints a summary of what was done
#   - Acquires a lock file to prevent concurrent runs (AIM_LOCK_FILE env var)
#   - Prompts for confirmation before destructive changes unless --yes is passed
#   - Verifies convergence after apply and reports any agents that didn't converge
#
# Exit codes:
#   0 = all agents applied successfully
#   1 = partial failure (some agents failed)
#   2 = total failure (all processed agents failed)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=scripts/lib/config.sh
source "${SCRIPT_DIR}/lib/config.sh"
# shellcheck source=scripts/lib/api.sh
source "${SCRIPT_DIR}/lib/api.sh"
# shellcheck source=scripts/lib/reconcile.sh
source "${SCRIPT_DIR}/lib/reconcile.sh"
# shellcheck source=scripts/lib/report.sh
source "${SCRIPT_DIR}/lib/report.sh"
# shellcheck source=scripts/lib/prompt.sh
source "${SCRIPT_DIR}/lib/prompt.sh"

load_config

HOST=""
FLEET_NAME=""
PRUNE=0
DRY_RUN=0
FAIL_FAST=0
RETRY=0
RETRY_FILE=""
YES=0
OUTPUT_FORMAT="text"   # "text" | "json"
WEBHOOK_URL=""
AIM_LOCK_FILE="${AIM_LOCK_FILE:-.myrmidons.lock}"
AIM_LOCK_TIMEOUT="${AIM_LOCK_TIMEOUT:-60}"
# Seconds to wait after hibernating an unmanaged agent before issuing DELETE.
# Allows the Agamemnon API to record the state transition before DELETE arrives.
# Override via env var: HIBERNATE_SETTLE_SECONDS=0 ./scripts/apply.sh --prune
HIBERNATE_SETTLE_SECONDS="${HIBERNATE_SETTLE_SECONDS:-2}"
SNAPSHOT_DIR=""
SNAPSHOT_KEEP="${SNAPSHOT_KEEP:-10}"
# File descriptor used for flock (9)
_LOCK_FD=9

CREATED=0
UPDATED=0
WOKEN=0
HIBERNATED=0
UNCHANGED=0
PRUNED=0
ERRORS=0
_PRUNED_NAMES=()   # names of agents pruned during this run (for convergence check)

# Per-agent error tracking: structured entries "name\x01http_status\x01message"
# Uses ASCII unit separator (0x01) as delimiter — safe against agent names, HTTP codes, and error messages.
FAILED_AGENTS_INFO=()
FAILED_AGENT_NAMES=()  # agent names (metadata.name) that failed; written to RETRY_FILE by _write_failed_agents_file

# Directory for state files
MYRMIDONS_STATE_DIR="${REPO_ROOT}/.myrmidons"
FAILED_AGENTS_FILE="${MYRMIDONS_STATE_DIR}/failed-agents.txt"

# Arrays tracking modified agents for convergence verification (#41)
_MODIFIED_NAMES=()
_MODIFIED_DESIRED=()

# Set by apply_agent on successful CREATE; cleared at the top of each apply_agent call.
_LAST_CREATED_AGENT_JSON=""

# Counts of destructive operations pending — used by confirmation prompt (#48)
_DESTRUCTIVE_COUNT=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --prune)            PRUNE=1; shift ;;
            --dry-run)          DRY_RUN=1; shift ;;
            --fail-fast)        FAIL_FAST=1; shift ;;
            --retry)            RETRY=1; shift ;;
            --retry-file)       RETRY_FILE="$2"; shift 2 ;;
            --fleet)            FLEET_NAME="$2"; shift 2 ;;
            --failed-agents-file) FAILED_AGENTS_FILE="$2"; shift 2 ;;
            --yes|-y)           YES=1; shift ;;
            --lock-timeout)     AIM_LOCK_TIMEOUT="$2"; shift 2 ;;
            --output)           OUTPUT_FORMAT="$2"; shift 2 ;;
            --webhook)          WEBHOOK_URL="$2"; shift 2 ;;
            --snapshot-dir)     SNAPSHOT_DIR="$2"; shift 2 ;;
            --force)            shift ;;  # Consume --force (applies during actual apply, not dry-run)
            -h|--help)          usage; exit 0 ;;
            *) HOST="$1"; shift ;;
        esac
    done
}

usage() {
    cat <<EOF
Usage: $0 [host] [--fleet <name>] [--prune] [--dry-run] [--yes] [--force] [--fail-fast] [--retry] [--retry-file FILE] [--lock-timeout SECONDS] [--output json] [--webhook <url>]

Reconciles agent YAML definitions against Agamemnon's actual state.

Options:
  host                       Only apply agents for this host (default: all)
  --fleet NAME               Only apply agents belonging to the named fleet
  --prune                    Hibernate and delete unmanaged agents (agents in Agamemnon
                             but not in YAML). DEFAULT: warn only.
  --dry-run                  Show what would happen, make no changes (same as plan.sh)
  --fail-fast                Stop on first error (default: continue processing all agents)
  --retry                    Re-apply only agents listed in .myrmidons/failed-agents.txt.
                             File is self-managing: updated on partial success, cleared on
                             full success. Operators need not manually manage this file.
  --retry-file FILE          Path to the retry file (default: .myrmidons/failed-agents.txt).
  --failed-agents-file PATH  Path to the failed-agents file (default: .myrmidons/failed-agents.txt).
  --yes, -y                  Skip the interactive confirmation prompt for destructive
                             changes (WAKE, HIBERNATE, DELETE/PRUNE). Required in
                             non-interactive (CI) environments unless already non-tty.
  --force                    Force apply even if lock acquisition times out.
  --lock-timeout SECS        Set lock acquisition timeout in seconds (default: 60).
                             Also configurable via AIM_LOCK_TIMEOUT env var.
  --output json              Emit a JSON reconciliation report to stdout instead of
                             human-readable text. Also saves to reports/last-reconciliation.json.
  --webhook URL              POST the JSON report to URL after reconciliation completes.
  --snapshot-dir DIR         Directory for pre-apply snapshots (default: .myrmidons/snapshots).
                             Also configurable via SNAPSHOT_DIR env var.
  -h, --help                 Show this help

Environment variables:
  AIM_LOCK_FILE              Path to the apply lock file (default: .myrmidons.lock).
                             Set to a workspace-scoped path in parallel CI environments.
  AIM_LOCK_TIMEOUT           Lock acquisition timeout in seconds (default: 60).
  HIBERNATE_SETTLE_SECONDS   Seconds to wait after hibernating an unmanaged agent before
                             issuing DELETE (default: 2). Set to 0 to skip the wait in CI:
                             HIBERNATE_SETTLE_SECONDS=0 ./scripts/apply.sh --prune

Examples:
  $0                              # Reconcile everything
  $0 hermes                       # Reconcile hermes only
  $0 --fleet dev-mesh             # Reconcile agents in the dev-mesh fleet
  $0 --prune                      # Reconcile + remove unmanaged agents
  $0 --yes                        # Skip confirmation prompt (e.g. in CI)
  $0 --yes --prune                # Reconcile + prune without confirmation prompt
  $0 --dry-run --force            # Dry-run (force is handled correctly)
  $0 --fail-fast                  # Stop on first error
  $0 --retry                      # Re-attempt agents from last failure
  $0 --retry --retry-file failed-agents.txt  # Retry only previously failed agents
  $0 --lock-timeout 120           # Reconcile with 120s lock timeout
  $0 --output json | jq .         # Machine-readable report
  $0 --webhook http://host/hook   # Post report to webhook
EOF
}

# Guard against writing snapshots to a dangerous or out-of-tree path (#391).
# Args: <dir> <"set"|""> — second arg non-empty means SNAPSHOT_DIR was explicitly set.
_guard_snapshot_dir() {
    local dir="$1"
    local explicitly_set="${2:-}"

    # Always block the known dangerous fallback regardless of how it was derived.
    if [[ "$dir" == "/.myrmidons/snapshots" ]]; then
        echo "ERROR: snapshot dir resolved to '/.myrmidons/snapshots'." >&2
        echo "  This usually means REPO_ROOT is empty or unset." >&2
        echo "  Set SNAPSHOT_DIR explicitly or ensure REPO_ROOT is defined." >&2
        exit 1
    fi

    # Block derived out-of-repo paths (only when SNAPSHOT_DIR was not explicitly set).
    # Strip any trailing slash from REPO_ROOT (#487) so the glob below does not silently
    # fail when REPO_ROOT is set to something like "/home/user/repo/" — the resulting
    # pattern "/home/user/repo//*" would not match the legitimate default
    # "/home/user/repo/.myrmidons/snapshots".
    # NOTE: variable name avoids the lowercase "repo_root" substring (issue #370 invariant
    # enforced by tests/unit/test_apply_sc2154.bats).
    local REPO_ROOT_NOSLASH="${REPO_ROOT%/}"
    if [[ -z "$explicitly_set" && "$dir" != "${REPO_ROOT_NOSLASH}"/* ]]; then
        echo "ERROR: snapshot dir '${dir}' is outside the repo tree '${REPO_ROOT}'." >&2
        echo "  REPO_ROOT may be empty or unset. Set SNAPSHOT_DIR explicitly to override." >&2
        exit 1
    fi
}

# Record a per-agent failure and increment error counter.
# Usage: record_failure <agent_name> <http_status> <error_message>
record_failure() {
    local agent_name="$1"
    local http_status="$2"
    local error_message="$3"

    local _sep=$'\x01'
    ERRORS=$((ERRORS + 1))
    FAILED_AGENTS_INFO+=("${agent_name}${_sep}${http_status}${_sep}${error_message}")
}

# Print a detailed per-agent error summary.
print_error_summary() {
    echo ""
    echo "================================================"
    echo "FAILED AGENTS (${ERRORS}):"
    echo ""
    local entry agent_name http_status error_msg
    for entry in "${FAILED_AGENTS_INFO[@]}"; do
        IFS=$'\x01' read -r agent_name http_status error_msg <<< "$entry"
        echo "  [FAIL] ${agent_name}"
        if [[ -n "$http_status" ]]; then
            echo "         HTTP status: ${http_status}"
        fi
        if [[ -n "$error_msg" ]]; then
            echo "         Error: ${error_msg}"
        fi
    done
    echo ""
    echo "Failed agents written to: ${FAILED_AGENTS_FILE}"
    echo "Run 'just retry' to re-apply only failed agents."
}

# ---------------------------------------------------------------------------
# #54 — Lock file: prevent concurrent apply runs
# ---------------------------------------------------------------------------

# acquire_lock — open AIM_LOCK_FILE on fd 9 and obtain an exclusive flock.
# Waits up to AIM_LOCK_TIMEOUT seconds. Exits with error if it times out.
# Release happens automatically when the fd is closed (i.e. process exits).
acquire_lock() {
    # Open (or create) the lock file on the dedicated fd.
    # eval is the portable way to open a file on a dynamic fd number in bash.
    eval "exec ${_LOCK_FD}>> \"\${AIM_LOCK_FILE}\""

    if ! flock -w "${AIM_LOCK_TIMEOUT}" "${_LOCK_FD}"; then
        echo "ERROR: Could not acquire apply lock '${AIM_LOCK_FILE}' within ${AIM_LOCK_TIMEOUT}s." >&2
        echo "  Another apply.sh may be running. If not, remove the lock file and retry." >&2
        echo "  Lock file: $(pwd)/${AIM_LOCK_FILE}" >&2
        exit 1
    fi
}

# release_lock — explicitly release the flock and close the fd.
# Called from the EXIT trap so it runs even on error exit.
release_lock() {
    # Close the fd — flock is released automatically.
    # Failure to close the fd is non-fatal (the EXIT trap will be called once
    # only, so a missing fd is safe) but should be surfaced for debugging.
    if ! eval "exec ${_LOCK_FD}>&-" 2>/dev/null; then
        echo "warn: failed to close lock fd ${_LOCK_FD} (already closed?)" >&2
    fi
}

# ---------------------------------------------------------------------------
# #48 — Confirmation prompt: count pending destructive ops and ask user
# ---------------------------------------------------------------------------

# count_destructive_ops — scan YAML files for agents that will need WAKE,
# HIBERNATE, DELETE (PRUNE), or CREATE against current Agamemnon state.
# Sets global _DESTRUCTIVE_COUNT.
count_destructive_ops() {
    local agents_json="$1"
    shift
    local yaml_files=("$@")
    _DESTRUCTIVE_COUNT=0

    for yaml_file in "${yaml_files[@]}"; do
        local name desired_state actual_json actual_status
        name="$(yq eval '.metadata.name' "$yaml_file")"
        desired_state="$(yq eval '.spec.desiredState // "active"' "$yaml_file")"
        actual_json="$(echo "$agents_json" | jq -r --arg n "$name" '.[] | select(.name == $n)')"

        if [[ -z "$actual_json" ]]; then
            # CREATE is destructive (new agent + potentially WAKE)
            _DESTRUCTIVE_COUNT=$((_DESTRUCTIVE_COUNT + 1))
            continue
        fi

        actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

        if [[ "$desired_state" == "active" && "$actual_status" == "offline" ]]; then
            _DESTRUCTIVE_COUNT=$((_DESTRUCTIVE_COUNT + 1))
        elif [[ "$desired_state" == "hibernated" ]] && \
             [[ "$actual_status" == "active" || "$actual_status" == "online" ]]; then
            _DESTRUCTIVE_COUNT=$((_DESTRUCTIVE_COUNT + 1))
        fi
    done

    # PRUNE operations are also destructive
    if [[ $PRUNE -eq 1 ]]; then
        local managed_names=()
        for yaml_file in "${yaml_files[@]}"; do
            managed_names+=("$(yq eval '.metadata.name' "$yaml_file")")
        done
        while IFS= read -r actual_name; do
            local is_managed=0
            for mn in "${managed_names[@]}"; do
                [[ "$mn" == "$actual_name" ]] && is_managed=1 && break
            done
            [[ $is_managed -eq 0 ]] && _DESTRUCTIVE_COUNT=$((_DESTRUCTIVE_COUNT + 1))
        done < <(echo "$agents_json" | jq -r '.[].name')
    fi
}

# confirm_destructive — prompt the user if there are destructive ops pending
# and --yes was not passed. Returns 0 if proceed, exits 0 if user declines.
confirm_destructive() {
    local total_changes="$1"

    # Non-interactive or --yes: skip prompt
    if [[ $YES -eq 1 ]] || [[ "${MYRMIDONS_YES:-}" == "true" ]] || [[ ! -t 0 ]]; then
        return 0
    fi

    if [[ $_DESTRUCTIVE_COUNT -eq 0 ]]; then
        return 0
    fi

    echo ""
    echo "Pending changes: ${total_changes} total, ${_DESTRUCTIVE_COUNT} destructive (WAKE/HIBERNATE/CREATE/PRUNE)"
    if ! confirm_with_timeout "Apply ${total_changes} change(s) to ${AGAMEMNON_URL}? [y/N]"; then
        echo "Aborted."
        exit 0
    fi
}

# ---------------------------------------------------------------------------
# #41 — Convergence verification: re-check state after apply loop
# ---------------------------------------------------------------------------

# verify_convergence — re-fetch agent states for all modified agents and
# confirm desired == actual. Reports any that did not converge.
# Returns 1 if any agent failed to converge (caller should increment ERRORS).
verify_convergence() {
    if [[ ${#_MODIFIED_NAMES[@]} -eq 0 && ${#_PRUNED_NAMES[@]} -eq 0 ]]; then
        return 0
    fi

    local failed=0
    local verified=0
    local pruned_verified=0

    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        echo ""
        echo "Verifying convergence for ${#_MODIFIED_NAMES[@]} modified agent(s)..."
        if [[ ${#_PRUNED_NAMES[@]} -gt 0 ]]; then
            echo "Verifying ${#_PRUNED_NAMES[@]} pruned agent(s) are absent..."
        fi
    fi

    # Refresh agent list once for all convergence checks
    local agents_json_fresh
    agents_json_fresh="$(agamemnon_list_agents)"

    for i in "${!_MODIFIED_NAMES[@]}"; do
        local agent_name="${_MODIFIED_NAMES[$i]}"
        local desired="${_MODIFIED_DESIRED[$i]}"

        local actual_json actual_status
        actual_json="$(echo "$agents_json_fresh" | jq -r --arg n "$agent_name" '.[] | select(.name == $n)')"

        if [[ -z "$actual_json" ]]; then
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "  [!] ${agent_name}: NOT FOUND in Agamemnon after apply (convergence failed)"
            fi
            report_add_convergence "$agent_name" "$desired" "not_found" 0 \
                "not found in Agamemnon after apply"
            failed=$((failed + 1))
            continue
        fi

        actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

        # Map desired state to expected runtime status
        local converged=0
        local reason=""
        if [[ "$desired" == "active" ]]; then
            # After a WAKE/CREATE, status should be active or online (not offline)
            if [[ "$actual_status" == "active" || "$actual_status" == "online" || \
                  "$actual_status" == "starting" ]]; then
                converged=1
                reason="status=${actual_status}"
            else
                reason="desired=active but actual_status=${actual_status}"
            fi
        elif [[ "$desired" == "hibernated" ]]; then
            # After HIBERNATE, status should be offline or hibernated
            if [[ "$actual_status" == "offline" || "$actual_status" == "hibernated" ]]; then
                converged=1
                reason="status=${actual_status}"
            else
                reason="desired=hibernated but actual_status=${actual_status}"
            fi
        else
            # For UPDATE actions (field changes only), treat as converged if agent exists
            converged=1
            reason="update applied; status=${actual_status}"
        fi

        if [[ $converged -eq 1 ]]; then
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "  [ok] ${agent_name}: converged (status=${actual_status})"
            fi
            report_add_convergence "$agent_name" "$desired" "$actual_status" 1 "$reason"
            verified=$((verified + 1))
        else
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "  [!] ${agent_name}: NOT converged (desired=${desired}, actual_status=${actual_status})"
            fi
            report_add_convergence "$agent_name" "$desired" "$actual_status" 0 "$reason"
            failed=$((failed + 1))
        fi
    done

    # Verify pruned agents are gone from the API
    if [[ ${#_PRUNED_NAMES[@]} -gt 0 ]]; then
        for pruned_name in "${_PRUNED_NAMES[@]}"; do
            local still_exists
            still_exists="$(echo "$agents_json_fresh" | jq -r --arg n "$pruned_name" '.[] | select(.name == $n) | .name')"
            if [[ -n "$still_exists" ]]; then
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "  [!] ${pruned_name}: pruned but still present in API (convergence failed)"
                fi
                report_add_convergence "$pruned_name" "pruned" "present" 0 \
                    "pruned but still present in Agamemnon API"
                failed=$((failed + 1))
            else
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "  [ok] ${pruned_name}: confirmed absent (pruned)"
                fi
                pruned_verified=$((pruned_verified + 1))
                report_add_convergence "$pruned_name" "pruned" "absent" 1 \
                    "pruned and confirmed absent from Agamemnon API"
            fi
        done
    fi

    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        if [[ ${#_PRUNED_NAMES[@]} -gt 0 ]]; then
            echo "Convergence: ${verified} converged, ${pruned_verified} pruned, ${failed} failed."
        else
            echo "Convergence: ${verified} converged, ${failed} failed."
        fi
    fi

    if [[ $failed -gt 0 ]]; then
        return 1
    fi
    return 0
}

main() {
    # Save original args before parse_args consumes them for dry-run filtering
    local -a orig_args=("$@")

    parse_args "$@"

    # Validate AGAMEMNON_URL format early (#118)
    validate_agamemnon_url

    # Validate HOST argument against known agents/ subdirectories (#149)
    if [[ -n "$HOST" && ! -d "${REPO_ROOT}/agents/${HOST}" ]]; then
        echo "ERROR: Host '${HOST}' not found — no agents/${HOST}/ directory exists." >&2
        echo "  Known hosts: $(find "${REPO_ROOT}/agents" -mindepth 1 -maxdepth 1 -type d \
            ! -name '_templates' -printf '%f ' 2>/dev/null || echo '(none)')" >&2
        exit 1
    fi

    # Export AIM_LOCK_TIMEOUT for use by child processes (e.g. api.sh)
    export AIM_LOCK_TIMEOUT

    if [[ $DRY_RUN -eq 1 ]]; then
        # Strip --force, --dry-run, --lock-timeout, --snapshot-dir, --yes, --fail-fast,
        # --retry, and --retry-file before forwarding to plan.sh (plan.sh doesn't understand these).
        # --prune IS forwarded so plan.sh can show which unmanaged agents would
        # be removed, preserving the prune intent in dry-run output. (#69, #71)
        # --output and --webhook ARE also forwarded so plan.sh can emit JSON.
        local -a clean_args=()
        local skip_next=0
        for arg in "${orig_args[@]}"; do
            if [[ $skip_next -eq 1 ]]; then
                skip_next=0
                continue
            fi
            case "$arg" in
                --force | --dry-run | --fail-fast | --retry | --yes | -y)
                    continue
                    ;;
                --lock-timeout | --snapshot-dir | --retry-file)
                    skip_next=1
                    continue
                    ;;
                *)
                    clean_args+=("$arg")
                    ;;
            esac
        done

        exec "${SCRIPT_DIR}/plan.sh" "${clean_args[@]}"
    fi

    check_deps
    agamemnon_check_connection

    # #54 — Acquire apply lock to prevent concurrent runs.
    acquire_lock
    trap 'release_lock; report_cleanup; cleanup_fleet_tmpdir' EXIT

    # Check for agents/ and fleets/ directories
    local has_agents=false
    local has_fleets=false
    [[ -d "${REPO_ROOT}/agents/" ]] && has_agents=true
    [[ -d "${REPO_ROOT}/fleets/" ]] && has_fleets=true

    if [[ "$has_agents" == "false" && "$has_fleets" == "false" ]]; then
        log_error "Neither agents/ nor fleets/ directory found — nothing to reconcile"
        exit 1
    elif [[ "$has_agents" == "false" ]]; then
        log_warn "agents/ directory not found — reconciling fleets only"
    elif [[ "$has_fleets" == "false" ]]; then
        log_warn "fleets/ directory not found — reconciling agents only"
    fi

    # Confirmation prompt when --prune is set (unless --yes or MYRMIDONS_YES=true)
    if [[ $PRUNE -eq 1 && $YES -eq 0 && "${MYRMIDONS_YES:-}" != "true" ]]; then
        echo "WARNING: --prune will hibernate and delete agents not in YAML."
        if ! confirm_with_timeout "Continue? [y/N]"; then
            echo "Aborted."
            exit 0
        fi
    fi

    # Initialise report accumulator
    report_init "${HOST:-all}"

    local agents_json
    agents_json="$(agamemnon_list_agents)"

    # Capture pre-apply snapshot (#228: includes context fields user/branch/host/timestamp)
    local effective_snapshot_dir="${SNAPSHOT_DIR:-${REPO_ROOT}/.myrmidons/snapshots}"
    # Guard: abort if snapshot dir is dangerous or outside repo tree (#391)
    _guard_snapshot_dir "$effective_snapshot_dir" "${SNAPSHOT_DIR:+set}"
    local snap_file
    snap_file="$(snapshot_write "$agents_json" "$effective_snapshot_dir" "${HOST:-all}")"
    snapshot_prune "$effective_snapshot_dir" "$SNAPSHOT_KEEP"
    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        echo "Snapshot saved: ${snap_file}"
    fi

    local yaml_files
    if [[ $RETRY -eq 1 ]]; then
        # Retry mode: read yaml_file paths directly from failed-agents.txt
        if [[ ! -f "${FAILED_AGENTS_FILE}" ]] || [[ ! -s "${FAILED_AGENTS_FILE}" ]]; then
            echo "No failed agents to retry (${FAILED_AGENTS_FILE} is empty or missing)."
            exit 0
        fi
        mapfile -t yaml_files < "$FAILED_AGENTS_FILE"
        echo "Retrying ${#yaml_files[@]} previously failed agent(s):"
        for fn in "${yaml_files[@]}"; do echo "  - ${fn}"; done
        echo ""
    else
        mapfile -t yaml_files < <(get_agent_files "$HOST" "$FLEET_NAME")
    fi

    # If --retry, filter yaml_files to only the agents listed in RETRY_FILE
    if [[ $RETRY -eq 1 ]]; then
        local effective_retry_file="${RETRY_FILE:-failed-agents.txt}"
        if [[ -f "$effective_retry_file" ]]; then
            local -a retry_files=()
            while IFS= read -r agent_name; do
                [[ -z "$agent_name" ]] && continue
                for yaml_file in "${yaml_files[@]}"; do
                    local fname_name
                    fname_name="$(yq eval '.metadata.name' "$yaml_file")"
                    if [[ "$fname_name" == "$agent_name" ]]; then
                        retry_files+=("$yaml_file")
                        break
                    fi
                done
            done < "$effective_retry_file"
            yaml_files=("${retry_files[@]}")
        else
            log_warn "--retry specified but retry file '${effective_retry_file}' not found; applying all agents"
        fi
    fi

    if [[ ${#yaml_files[@]} -eq 0 ]]; then
        if [[ "$OUTPUT_FORMAT" == "json" ]]; then
            report_emit 0 0 0 0 0 0 0
        else
            echo "No agent YAML files found."
        fi
        exit 0
    fi

    # #48 — Count destructive ops and prompt for confirmation if needed.
    count_destructive_ops "$agents_json" "${yaml_files[@]}"
    local total_changes=${#yaml_files[@]}
    confirm_destructive "$total_changes"

    if [[ "$OUTPUT_FORMAT" != "json" ]]; then
        echo "Applying desired state to ${AGAMEMNON_URL}"
        echo "================================================"
        echo ""
    fi

    # Truncate failed-agents file before this run (stores yaml_file paths for --retry)
    mkdir -p "${MYRMIDONS_STATE_DIR}"
    : > "$FAILED_AGENTS_FILE"

    for yaml_file in "${yaml_files[@]}"; do
        local errors_before=$ERRORS
        apply_agent "$yaml_file" "$agents_json"

        # If apply_agent recorded a new failure, store the yaml_file path for --retry
        if [[ $ERRORS -gt $errors_before ]]; then
            # Store the yaml_file path (not the agent name) so --retry can find it
            echo "$yaml_file" >> "$FAILED_AGENTS_FILE"
        fi

        if [[ $FAIL_FAST -eq 1 && $ERRORS -gt 0 ]]; then
            echo ""
            echo "ERROR: --fail-fast enabled, stopping after first failure." >&2
            break
        fi

        # Refresh actual state after each change
        agents_json="$(agamemnon_list_agents)"
    done

    # Handle unmanaged agents.
    # When --fleet is active, scope the check to only agents that belong to the
    # fleet (i.e. agents whose names are in yaml_files), so agents managed by
    # other fleets or outside this fleet are not flagged as unmanaged.
    local scoped_agents_json
    if [[ -n "$FLEET_NAME" ]]; then
        local fleet_names_json
        fleet_names_json="$(for f in "${yaml_files[@]}"; do yq eval '.metadata.name' "$f"; done | jq -Rsc 'split("\n") | map(select(length > 0))')"
        scoped_agents_json="$(echo "$agents_json" | jq --argjson names "$fleet_names_json" '[.[] | select(.name as $n | $names | index($n) != null)]')"
    else
        scoped_agents_json="$agents_json"
    fi
    handle_unmanaged "$scoped_agents_json" "${yaml_files[@]}"

    # #41 — Convergence verification: re-check all modified agents.
    if ! verify_convergence; then
        ERRORS=$((ERRORS + 1))
    fi

    # Write failed agents file when there were errors (and not a retry run)
    if [[ $ERRORS -gt 0 && $RETRY -eq 0 ]]; then
        _write_failed_agents_file
    fi

    # Emit output
    if [[ "$OUTPUT_FORMAT" == "json" ]]; then
        local report_json
        report_json="$(report_emit "$CREATED" "$UPDATED" "$WOKEN" "$HIBERNATED" \
                                   "$UNCHANGED" "$PRUNED" "$ERRORS")"
        if [[ -n "$WEBHOOK_URL" ]]; then
            local delivery_json
            delivery_json="$(report_webhook "$report_json" "$WEBHOOK_URL")"
            report_json="$(echo "$report_json" | jq --argjson d "$delivery_json" \
                '. + {webhook_delivery: $d}')"
        fi
        report_save "$report_json"
        echo "$report_json"
    else
        echo ""
        echo "================================================"
        echo "Summary: created=${CREATED} updated=${UPDATED} woken=${WOKEN} hibernated=${HIBERNATED} unchanged=${UNCHANGED} pruned=${PRUNED} errors=${ERRORS}"

        # Always save a report file (silently) so report_save is useful even in text mode
        local report_json
        report_json="$(report_emit "$CREATED" "$UPDATED" "$WOKEN" "$HIBERNATED" \
                                   "$UNCHANGED" "$PRUNED" "$ERRORS")"
        if [[ -n "$WEBHOOK_URL" ]]; then
            local delivery_json
            delivery_json="$(report_webhook "$report_json" "$WEBHOOK_URL")"
            report_json="$(echo "$report_json" | jq --argjson d "$delivery_json" \
                '. + {webhook_delivery: $d}')"
        fi
        report_save "$report_json"
    fi

    if [[ $ERRORS -gt 0 ]]; then
        print_error_summary

        local total_processed=$(( CREATED + UPDATED + WOKEN + HIBERNATED + UNCHANGED + ERRORS ))
        # Total failure: every processed agent either failed or none succeeded
        if [[ $(( total_processed - ERRORS )) -eq 0 ]]; then
            exit 2
        fi
        exit 1
    fi

    # Clear failed-agents file on full success (issue #269).
    # This completes the self-managing lifecycle:
    # - Populated during the apply loop with yaml_file paths of failed agents
    # - Cleared here on full success (all agents passed in this run, $ERRORS == 0)
    # This ensures the file always contains only agents currently failing, with no manual management needed.
    if [[ -f "${FAILED_AGENTS_FILE}" ]]; then
        : > "${FAILED_AGENTS_FILE}"
    fi
}

# Write FAILED_AGENT_NAMES array to failed-agents.txt (one name per line).
# FAILED_AGENT_NAMES holds metadata.name strings (e.g. "odyssey-mainline-analysis"), NOT
# yaml_file paths. Do not confuse with FAILED_AGENT_STATUSES/FAILED_AGENT_MESSAGES (also
# metadata.name strings, but used only for error-summary display in print_error_summary).
# The FAILED_AGENTS_FILE written inline at line 583 uses yaml_file paths for --retry
# resolution; this function writes to RETRY_FILE (default: failed-agents.txt) for a
# different consumer. See #371.
_write_failed_agents_file() {
    local failed_file="${RETRY_FILE:-failed-agents.txt}"
    if [[ ${#FAILED_AGENT_NAMES[@]} -gt 0 ]]; then
        : > "$failed_file"
        for agent_name in "${FAILED_AGENT_NAMES[@]}"; do
            echo "$agent_name" >> "$failed_file"
        done
        log_warn "Wrote ${#FAILED_AGENT_NAMES[@]} failed agent(s) to ${failed_file}"
    fi
}

apply_agent() {
    local yaml_file="$1"
    local agents_json="$2"

    _LAST_CREATED_AGENT_JSON=""

    # Parse YAML fields into local variables
    local name label program workdir args desc tags owner role model deploy_type desired_state
    name="$(yq eval '.metadata.name' "$yaml_file")"
    local agent_host
    agent_host="$(yq eval '.metadata.host // "hermes"' "$yaml_file")"
    label="$(yq eval '.spec.label // ""' "$yaml_file")"
    program="$(yq eval '.spec.program // "claude-code"' "$yaml_file")"
    workdir="$(yq eval '.spec.workingDirectory // ""' "$yaml_file")"
    args="$(yq eval '.spec.programArgs // ""' "$yaml_file")"
    desc="$(yq eval '.spec.taskDescription // ""' "$yaml_file")"
    tags="$(yq eval '.spec.tags // [] | join(",")' "$yaml_file")"
    owner="$(yq eval '.spec.owner // ""' "$yaml_file")"
    role="$(yq eval '.spec.role // "member"' "$yaml_file")"
    model="$(yq eval '.spec.model // ""' "$yaml_file")"
    deploy_type="$(yq eval '.spec.deployment.type // "local"' "$yaml_file")"
    desired_state="$(yq eval '.spec.desiredState // "active"' "$yaml_file")"

    # Look up actual agent
    local actual_json
    actual_json="$(echo "$agents_json" | jq -r --arg n "$name" '.[] | select(.name == $n)')"

    if [[ -z "$actual_json" ]]; then
        # CREATE
        if [[ "$OUTPUT_FORMAT" != "json" ]]; then
            echo "[+] Creating ${name}..."
        fi
        local create_body
        create_body="$(build_create_json "$name" "$label" "$program" "$workdir" "$args" "$desc" "$tags" "$owner" "$role" "$model" "$deploy_type")"

        local result
        if result="$(agamemnon_create_agent "$create_body" 2>&1)"; then
            local new_id
            new_id="$(echo "$result" | jq -r '.id // empty')"
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "    Created: id=${new_id}"
            fi
            CREATED=$((CREATED + 1))
            _LAST_CREATED_AGENT_JSON="$result"

            local woke_status="created"
            # Wake if desired
            if [[ "$desired_state" == "active" && -n "$new_id" ]]; then
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Starting ${name}..."
                fi
                agamemnon_wake_agent "$new_id" > /dev/null
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Started."
                fi
                WOKEN=$((WOKEN + 1))
                woke_status="active"
            fi

            # Track for convergence verification (#41)
            _MODIFIED_NAMES+=("$name")
            _MODIFIED_DESIRED+=("$desired_state")

            report_add_agent "$name" "$agent_host" "CREATE" "$desired_state" "$woke_status" "[]" ""
        else
            # Try to extract HTTP status from error output
            local http_status
            http_status="$(echo "$result" | grep -oE 'HTTP [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"
            local err_msg
            err_msg="$(echo "$result" | tail -1)"
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "    ERROR creating ${name}: ${err_msg}" >&2
            fi
            record_failure "$name" "$http_status" "$err_msg"
            report_add_agent "$name" "$agent_host" "ERROR" "$desired_state" "unknown" "[]" "create failed: ${err_msg}"
        fi
        return
    fi

    # Agent exists — check what needs to change
    local actual_id actual_status
    actual_id="$(echo "$actual_json" | jq -r '.id')"
    actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

    local action
    action="$(compute_drift "$name" "$desired_state" "$actual_json" \
        "$label" "$program" "$workdir" "$args" "$desc" "$tags" "$model" "$owner" "$role" "$deploy_type")"

    case "$action" in
        UNCHANGED)
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "[=] Unchanged: ${name}"
            fi
            UNCHANGED=$((UNCHANGED + 1))
            report_add_agent "$name" "$agent_host" "UNCHANGED" "$desired_state" "$actual_status" "[]" ""
            ;;
        WAKE)
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "[!] Starting ${name} (status=${actual_status}, desired=active)..."
            fi
            local wake_out
            if wake_out="$(agamemnon_wake_agent "$actual_id" 2>&1)"; then
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Started."
                fi
                WOKEN=$((WOKEN + 1))
                # Track for convergence verification (#41)
                _MODIFIED_NAMES+=("$name")
                _MODIFIED_DESIRED+=("$desired_state")
                report_add_agent "$name" "$agent_host" "WAKE" "$desired_state" "$actual_status" "[]" ""
            else
                local http_status err_msg
                http_status="$(echo "$wake_out" | grep -oE 'HTTP [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"
                err_msg="$(echo "$wake_out" | tail -1)"
                echo "    ERROR starting ${name}: ${err_msg}" >&2
                record_failure "$name" "$http_status" "Failed to wake agent: ${err_msg}"
                report_add_agent "$name" "$agent_host" "ERROR" "$desired_state" "$actual_status" "[]" "wake failed: ${err_msg}"
            fi
            ;;
        HIBERNATE)
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "[z] Stopping ${name} (status=${actual_status}, desired=hibernated)..."
            fi
            local hib_out
            if hib_out="$(agamemnon_hibernate_agent "$actual_id" 2>&1)"; then
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Hibernated."
                fi
                HIBERNATED=$((HIBERNATED + 1))
                # Track for convergence verification (#41)
                _MODIFIED_NAMES+=("$name")
                _MODIFIED_DESIRED+=("$desired_state")
                report_add_agent "$name" "$agent_host" "HIBERNATE" "$desired_state" "$actual_status" "[]" ""
            else
                local http_status err_msg
                http_status="$(echo "$hib_out" | grep -oE 'HTTP [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"
                err_msg="$(echo "$hib_out" | tail -1)"
                echo "    ERROR stopping ${name}: ${err_msg}" >&2
                record_failure "$name" "$http_status" "Failed to hibernate agent: ${err_msg}"
                report_add_agent "$name" "$agent_host" "ERROR" "$desired_state" "$actual_status" "[]" "hibernate failed: ${err_msg}"
            fi
            ;;
        UPDATE:*)
            local changed_fields="${action#UPDATE:}"
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "[~] Updating ${name} (fields: ${changed_fields})..."
            fi

            local drift_json
            drift_json="$(build_drift_json "$action" "$actual_json" \
                "$label" "$program" "$workdir" "$args" "$desc" "$tags" "$owner" "$role")"

            local tags_json
            if [[ -z "$tags" ]]; then
                tags_json="[]"
            else
                tags_json="$(echo "$tags" | jq -Rc 'split(",")')"
            fi

            local patch_body
            # Note: $label is a reserved keyword in jq 1.6 (label-break syntax).
            # Use $lbl as the variable name to avoid the parser conflict.
            patch_body="$(jq -n \
                --arg lbl "$label" \
                --arg program "$program" \
                --arg workingDirectory "$workdir" \
                --arg programArgs "$args" \
                --arg taskDescription "$desc" \
                --argjson tags "$tags_json" \
                --arg owner "$owner" \
                --arg role "$role" \
                '{label: $lbl, program: $program, workingDirectory: $workingDirectory,
                  programArgs: $programArgs, taskDescription: $taskDescription,
                  tags: $tags, owner: $owner, role: $role}')"

            local update_out
            if update_out="$(agamemnon_update_agent "$actual_id" "$patch_body" 2>&1)"; then
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Updated."
                fi
                UPDATED=$((UPDATED + 1))
                # Track for convergence verification (#41)
                _MODIFIED_NAMES+=("$name")
                _MODIFIED_DESIRED+=("$desired_state")
                report_add_agent "$name" "$agent_host" "UPDATE" "$desired_state" "$actual_status" "$drift_json" ""

                # Also start/stop if state needs to change
                if [[ "$desired_state" == "active" && "$actual_status" == "offline" ]]; then
                    agamemnon_wake_agent "$actual_id" > /dev/null
                    WOKEN=$((WOKEN + 1))
                elif [[ "$desired_state" == "hibernated" && \
                        ("$actual_status" == "active" || "$actual_status" == "online") ]]; then
                    agamemnon_hibernate_agent "$actual_id" > /dev/null
                    HIBERNATED=$((HIBERNATED + 1))
                fi
            else
                local http_status
                http_status="$(echo "$update_out" | grep -oE 'HTTP [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"
                local err_msg
                err_msg="$(echo "$update_out" | tail -1)"
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    ERROR updating ${name}: ${err_msg}" >&2
                fi
                record_failure "$name" "$http_status" "Failed to update fields [${changed_fields}]: ${err_msg}"
                report_add_agent "$name" "$agent_host" "ERROR" "$desired_state" "$actual_status" "$drift_json" "update failed: ${err_msg}"
            fi
            ;;
    esac
}

handle_unmanaged() {
    local agents_json="$1"
    shift
    local yaml_files=("$@")

    # Collect managed names
    local managed_names=()
    for yaml_file in "${yaml_files[@]}"; do
        local n
        n="$(yq eval '.metadata.name' "$yaml_file")"
        managed_names+=("$n")
    done

    # Find unmanaged
    while IFS= read -r actual_name; do
        local is_managed=0
        for mn in "${managed_names[@]}"; do
            [[ "$mn" == "$actual_name" ]] && is_managed=1 && break
        done

        if [[ $is_managed -eq 0 ]]; then
            if [[ $PRUNE -eq 1 ]]; then
                local agent_id
                agent_id="$(echo "$agents_json" | jq -r --arg n "$actual_name" \
                    '.[] | select(.name == $n) | .id')"
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "[-] Pruning unmanaged: ${actual_name}"
                    echo "    Hibernating first..."
                fi
                # Hibernate is best-effort: deleting an already-offline agent
                # is fine. Surface a warning so transient API errors are
                # visible in the apply log even though prune continues.
                if ! agamemnon_hibernate_agent "$agent_id" > /dev/null; then
                    echo "    warn: pre-delete hibernate failed for ${actual_name} (continuing to delete)" >&2
                fi
                sleep "$HIBERNATE_SETTLE_SECONDS"
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Deleting..."
                fi
                agamemnon_delete_agent "$agent_id" > /dev/null
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "    Deleted (backup created)."
                fi
                PRUNED=$((PRUNED + 1))
                # Track pruned names for convergence verification
                _PRUNED_NAMES+=("$actual_name")
                report_add_agent "$actual_name" "-" "PRUNE" "-" "pruned" "[]" ""
            else
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "[-] UNMANAGED: ${actual_name} (in Agamemnon but not in YAML — use --prune to remove)"
                fi
                report_add_unmanaged "$actual_name"
            fi
        fi
    done < <(echo "$agents_json" | jq -r '.[].name')
}

main "$@"
