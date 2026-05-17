#!/usr/bin/env bash
# scripts/lib/config.sh — Project-level configuration loader
#
# Loads configuration from (in order of precedence, lowest to highest):
#   1. Built-in defaults
#   2. .myrmidons.yaml  (project config, committed to git)
#   3. .myrmidons.local.yaml  (local overrides, gitignored)
#   4. Environment variables
#
# After sourcing this file, use MYRM_* variables for effective config.
#
# Usage:
#   source scripts/lib/config.sh
#   load_config              # populate MYRM_* from all sources
#   show_config              # print effective config table

set -euo pipefail

# =============================================================================
# Defaults
# =============================================================================

_MYRM_DEFAULT_HOST="hermes"
_MYRM_DEFAULT_AIM_HOST="http://localhost:8080"
_MYRM_DEFAULT_LOG_LEVEL="info"
_MYRM_DEFAULT_PRUNE_POLICY="manual"
_MYRM_DEFAULT_SNAPSHOT_RETENTION="7"

# =============================================================================
# Internal helpers
# =============================================================================

# Read a scalar field from a YAML file using yq.
# Returns the default value if the field is absent or null.
# Usage: _cfg_read_yaml FILE FIELD DEFAULT
_cfg_read_yaml() {
    local file="$1"
    local field="$2"
    local default="$3"

    if [[ ! -f "$file" ]]; then
        echo "$default"
        return
    fi

    local value
    value="$(yq eval ".${field} // \"\"" "$file" 2>/dev/null || echo "")"
    if [[ -z "$value" || "$value" == "null" ]]; then
        echo "$default"
    else
        echo "$value"
    fi
}

# Find the repository root (directory containing justfile or .myrmidons.yaml).
# Falls back to the current working directory.
_cfg_repo_root() {
    local dir
    dir="$(pwd)"
    while [[ "$dir" != "/" ]]; do
        if [[ -f "$dir/justfile" || -f "$dir/.myrmidons.yaml" ]]; then
            echo "$dir"
            return
        fi
        dir="$(dirname "$dir")"
    done
    echo "$(pwd)"
}

# =============================================================================
# Public API
# =============================================================================

# load_config — populate MYRM_* variables from all configuration sources.
#
# Precedence (highest wins):
#   env vars > .myrmidons.local.yaml > .myrmidons.yaml > built-in defaults
#
# Exported variables:
#   MYRM_DEFAULT_HOST       — default agent host (used by scripts as HOST default)
#   MYRM_AIM_HOST           — ProjectAgamemnon base URL (alias: AGAMEMNON_URL)
#   MYRM_LOG_LEVEL          — verbosity: debug | info | warn | error
#   MYRM_PRUNE_POLICY       — when to prune: manual | auto
#   MYRM_SNAPSHOT_RETENTION — days to keep snapshots (integer)
load_config() {
    local root
    root="$(_cfg_repo_root)"

    local project_cfg="${root}/.myrmidons.yaml"
    local local_cfg="${root}/.myrmidons.local.yaml"

    # --- Step 1: built-in defaults ---
    local d_host="$_MYRM_DEFAULT_HOST"
    local d_aim_host="$_MYRM_DEFAULT_AIM_HOST"
    local d_log_level="$_MYRM_DEFAULT_LOG_LEVEL"
    local d_prune_policy="$_MYRM_DEFAULT_PRUNE_POLICY"
    local d_snapshot_retention="$_MYRM_DEFAULT_SNAPSHOT_RETENTION"

    # --- Step 2: project config file (.myrmidons.yaml) ---
    d_host="$(_cfg_read_yaml "$project_cfg" "defaultHost" "$d_host")"
    d_aim_host="$(_cfg_read_yaml "$project_cfg" "aimHost" "$d_aim_host")"
    d_log_level="$(_cfg_read_yaml "$project_cfg" "logLevel" "$d_log_level")"
    d_prune_policy="$(_cfg_read_yaml "$project_cfg" "prunePolicy" "$d_prune_policy")"
    d_snapshot_retention="$(_cfg_read_yaml "$project_cfg" "snapshotRetention" "$d_snapshot_retention")"

    # --- Step 3: local override file (.myrmidons.local.yaml) ---
    d_host="$(_cfg_read_yaml "$local_cfg" "defaultHost" "$d_host")"
    d_aim_host="$(_cfg_read_yaml "$local_cfg" "aimHost" "$d_aim_host")"
    d_log_level="$(_cfg_read_yaml "$local_cfg" "logLevel" "$d_log_level")"
    d_prune_policy="$(_cfg_read_yaml "$local_cfg" "prunePolicy" "$d_prune_policy")"
    d_snapshot_retention="$(_cfg_read_yaml "$local_cfg" "snapshotRetention" "$d_snapshot_retention")"

    # --- Step 4: environment variables (highest precedence) ---
    MYRM_DEFAULT_HOST="${HOST:-${d_host}}"
    MYRM_AIM_HOST="${AGAMEMNON_URL:-${d_aim_host}}"
    MYRM_LOG_LEVEL="${MYRM_LOG_LEVEL:-${d_log_level}}"
    MYRM_PRUNE_POLICY="${MYRM_PRUNE_POLICY:-${d_prune_policy}}"
    MYRM_SNAPSHOT_RETENTION="${MYRM_SNAPSHOT_RETENTION:-${d_snapshot_retention}}"

    export MYRM_DEFAULT_HOST MYRM_AIM_HOST MYRM_LOG_LEVEL \
           MYRM_PRUNE_POLICY MYRM_SNAPSHOT_RETENTION
}

# show_config — print the effective configuration and its source for each field.
show_config() {
    local root
    root="$(_cfg_repo_root)"

    local project_cfg="${root}/.myrmidons.yaml"
    local local_cfg="${root}/.myrmidons.local.yaml"

    # Capture which env vars were set BEFORE load_config exports the MYRM_* names.
    local env_host="${HOST:-}"
    local env_aim_host="${AGAMEMNON_URL:-}"
    local env_log_level="${MYRM_LOG_LEVEL:-}"
    local env_prune_policy="${MYRM_PRUNE_POLICY:-}"
    local env_snapshot_retention="${MYRM_SNAPSHOT_RETENTION:-}"

    load_config

    echo "Effective Myrmidons configuration"
    echo "=================================="
    echo ""
    echo "Sources (lowest → highest precedence):"
    echo "  [defaults]  built-in defaults"
    if [[ -f "$project_cfg" ]]; then
        echo "  [project]   ${project_cfg}"
    else
        echo "  [project]   ${project_cfg} (not found — using defaults)"
    fi
    if [[ -f "$local_cfg" ]]; then
        echo "  [local]     ${local_cfg}"
    else
        echo "  [local]     ${local_cfg} (not found)"
    fi
    echo "  [env]       environment variables"
    echo ""
    printf "%-24s  %-40s  %s\n" "FIELD" "VALUE" "SOURCE"
    printf "%-24s  %-40s  %s\n" "------------------------" "----------------------------------------" "------"

    _cfg_show_field "defaultHost"       "$MYRM_DEFAULT_HOST"       "$project_cfg" "$local_cfg" "$env_host"
    _cfg_show_field "aimHost"           "$MYRM_AIM_HOST"           "$project_cfg" "$local_cfg" "$env_aim_host"
    _cfg_show_field "logLevel"          "$MYRM_LOG_LEVEL"          "$project_cfg" "$local_cfg" "$env_log_level"
    _cfg_show_field "prunePolicy"       "$MYRM_PRUNE_POLICY"       "$project_cfg" "$local_cfg" "$env_prune_policy"
    _cfg_show_field "snapshotRetention" "$MYRM_SNAPSHOT_RETENTION" "$project_cfg" "$local_cfg" "$env_snapshot_retention"
}

# _cfg_show_field — helper for show_config to resolve and print a single field's source.
# $1=field  $2=effective-value  $3=project-cfg  $4=local-cfg  $5=pre-captured-env-value
_cfg_show_field() {
    local field="$1"
    local effective="$2"
    local project_cfg="$3"
    local local_cfg="$4"
    local env_value="$5"

    local source="defaults"

    local from_project
    from_project="$(_cfg_read_yaml "$project_cfg" "$field" "")"
    if [[ -n "$from_project" && "$from_project" != "null" ]]; then
        source="project"
    fi

    local from_local
    from_local="$(_cfg_read_yaml "$local_cfg" "$field" "")"
    if [[ -n "$from_local" && "$from_local" != "null" ]]; then
        source="local"
    fi

    if [[ -n "$env_value" ]]; then
        source="env"
    fi

    printf "%-24s  %-40s  %s\n" "$field" "$effective" "$source"
}
