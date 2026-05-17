#!/usr/bin/env bash
# scripts/lib/log.sh — Structured logging library for Myrmidons scripts
#
# Provides log_debug, log_info, log_warn, log_error functions with:
#   - Log level filtering (LOG_LEVEL env var)
#   - Text or JSON output (LOG_FORMAT env var)
#   - Timestamps, level, script name, and message in each line
#   - Color output for TTY (auto-detected)
#   - ERROR goes to stderr; DEBUG/INFO/WARN go to stdout
#
# Usage:
#   source scripts/lib/log.sh
#   log_info "Starting reconciliation"
#   log_debug "Parsed agent: name=${name}"
#   log_warn "Agent not found: ${name}"
#   log_error "Failed to connect to Agamemnon"
#
# Environment variables:
#   LOG_LEVEL   - Minimum level to emit: DEBUG, INFO, WARN, ERROR (default: INFO)
#   LOG_FORMAT  - Output format: text, json (default: text)

# Numeric level mapping
_LOG_LEVEL_DEBUG=0
_LOG_LEVEL_INFO=1
_LOG_LEVEL_WARN=2
_LOG_LEVEL_ERROR=3

# Resolve configured minimum level to its numeric value
_log_min_level() {
    case "${LOG_LEVEL:-INFO}" in
        DEBUG) echo $_LOG_LEVEL_DEBUG ;;
        INFO)  echo $_LOG_LEVEL_INFO  ;;
        WARN)  echo $_LOG_LEVEL_WARN  ;;
        ERROR) echo $_LOG_LEVEL_ERROR ;;
        *)     echo $_LOG_LEVEL_INFO  ;;
    esac
}

# Auto-detect color support: enabled when stdout is a TTY and LOG_FORMAT != json
_log_use_color() {
    if [[ "${LOG_FORMAT:-text}" == "json" ]]; then
        echo 0; return
    fi
    if [[ -t 1 ]]; then
        echo 1
    else
        echo 0
    fi
}

# ANSI color codes
_LOG_COLOR_DEBUG='\033[0;36m'   # cyan
_LOG_COLOR_INFO='\033[0;32m'    # green
_LOG_COLOR_WARN='\033[0;33m'    # yellow
_LOG_COLOR_ERROR='\033[0;31m'   # red
_LOG_COLOR_RESET='\033[0m'

# Derive the calling script's basename for inclusion in log lines.
# Falls back to the sourcing script name or "myrmidons".
_log_script_name() {
    local name
    # BASH_SOURCE[2]: 0=log.sh, 1=caller of log_*, 2=top-level script
    name="$(basename "${BASH_SOURCE[2]:-${BASH_SOURCE[1]:-myrmidons}}" .sh)"
    echo "$name"
}

# Core emit function — not called directly by user code.
# Usage: _log_emit LEVEL numeric_level message
_log_emit() {
    local level="$1"          # DEBUG|INFO|WARN|ERROR
    local level_num="$2"      # numeric value
    local message="$3"

    local min_level
    min_level="$(_log_min_level)"
    if [[ "$level_num" -lt "$min_level" ]]; then
        return 0
    fi

    local ts
    ts="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

    local script
    script="$(_log_script_name)"

    local format="${LOG_FORMAT:-text}"

    if [[ "$format" == "json" ]]; then
        # Emit a single JSON object on one line; escape message for JSON safety
        local msg_escaped
        msg_escaped="$(printf '%s' "$message" | jq -Rr '.')"
        local line="{\"timestamp\":\"${ts}\",\"level\":\"${level}\",\"script\":\"${script}\",\"message\":${msg_escaped}}"
        if [[ "$level" == "ERROR" ]]; then
            echo "$line" >&2
        else
            echo "$line"
        fi
    else
        # Text format
        local use_color
        use_color="$(_log_use_color)"
        local color=""
        local reset=""

        if [[ "$use_color" -eq 1 ]]; then
            reset="$_LOG_COLOR_RESET"
            case "$level" in
                DEBUG) color="$_LOG_COLOR_DEBUG" ;;
                INFO)  color="$_LOG_COLOR_INFO"  ;;
                WARN)  color="$_LOG_COLOR_WARN"  ;;
                ERROR) color="$_LOG_COLOR_ERROR" ;;
            esac
        fi

        local line="${ts} ${color}${level}${reset} [${script}] ${message}"

        if [[ "$level" == "ERROR" ]]; then
            printf '%b\n' "$line" >&2
        else
            printf '%b\n' "$line"
        fi
    fi
}

log_debug() { _log_emit "DEBUG" "$_LOG_LEVEL_DEBUG" "$*"; }
log_info()  { _log_emit "INFO"  "$_LOG_LEVEL_INFO"  "$*"; }
log_warn()  { _log_emit "WARN"  "$_LOG_LEVEL_WARN"  "$*"; }
log_error() { _log_emit "ERROR" "$_LOG_LEVEL_ERROR" "$*"; }
