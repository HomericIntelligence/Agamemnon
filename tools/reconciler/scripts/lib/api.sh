#!/usr/bin/env bash
# scripts/lib/api.sh — ProjectAgamemnon API client
#
# Thin wrapper around curl calls to the ProjectAgamemnon REST API.
# All functions print raw JSON to stdout. Callers parse with jq.
#
# Usage:
#   source scripts/lib/api.sh
#   agamemnon_list_agents | jq '.[].name'
#
# TLS environment variables:
#   AGAMEMNON_CA_CERT     Path to custom CA certificate bundle (PEM)
#   AGAMEMNON_CLIENT_CERT Path to client certificate for mutual TLS (PEM)
#   AGAMEMNON_CLIENT_KEY  Path to client private key for mutual TLS (PEM)
#   AGAMEMNON_TLS_VERIFY  Set to "false" or "0" to disable TLS verification
#                         (insecure — only for development; emits a loud warning)

set -euo pipefail

# Default AGAMEMNON_URL only when truly unset. Issue #513: an explicitly empty
# exported value must reach _agamemnon_validate_url unchanged so it can be
# rejected by the empty-string guard, rather than being silently coerced to
# the localhost default by `:-`.
if [[ -z "${AGAMEMNON_URL+set}" ]]; then
    AGAMEMNON_URL="http://localhost:8080"
fi
_aim_had_xtrace=0
if [[ "$-" == *x* ]]; then _aim_had_xtrace=1; fi
{ set +x; } 2>/dev/null
AGAMEMNON_API_KEY="${AGAMEMNON_API_KEY:-}"
if [[ $_aim_had_xtrace -eq 1 ]]; then set -x; fi
unset _aim_had_xtrace

# Validate that AGAMEMNON_URL is a safe http/https URL.
#
# Rejects:
#   - Non-http/https schemes (ftp://, file://, javascript:, etc.)
#   - URLs with embedded credentials (user:password@)
#   - URLs with fragment (#) or query (?) components
#   - Non-numeric port numbers
#   - IPv6 bracket notation in the hostname field
#   - Empty or clearly non-URL strings
#   - Strings with embedded newlines (header injection)
#
# Usage:
#   _agamemnon_validate_url "$AGAMEMNON_URL" || exit 1
_agamemnon_validate_url() {
    local url="$1"

    # Reject empty
    if [[ -z "$url" ]]; then
        echo "ERROR: AGAMEMNON_URL is empty." >&2
        return 1
    fi

    # Reject embedded newlines (header injection)
    if [[ "$url" == *$'\n'* ]]; then
        echo "ERROR: AGAMEMNON_URL contains a newline character." >&2
        return 1
    fi

    # Must start with http:// or https://
    if [[ "$url" != http://* && "$url" != https://* ]]; then
        echo "ERROR: AGAMEMNON_URL must use http or https scheme: ${url}" >&2
        return 1
    fi

    # Strip scheme
    local rest="${url#http://}"
    rest="${rest#https://}"

    # Reject embedded credentials (user:pass@)
    if [[ "$rest" == *@* ]]; then
        echo "ERROR: AGAMEMNON_URL must not contain credentials: ${url}" >&2
        return 1
    fi

    # Reject fragment (#) — fragments are client-side and indicate a bad URL
    if [[ "$url" == *'#'* ]]; then
        echo "ERROR: AGAMEMNON_URL must not contain a fragment (#): ${url}" >&2
        return 1
    fi

    # Reject query string (?) — the API path is fixed; a query in the base URL is suspicious
    if [[ "$url" == *'?'* ]]; then
        echo "ERROR: AGAMEMNON_URL must not contain a query string (?): ${url}" >&2
        return 1
    fi

    # Reject IPv6 bracket notation in the host field (not supported)
    if [[ "$rest" == '['* ]]; then
        echo "ERROR: AGAMEMNON_URL IPv6 bracket notation is not supported: ${url}" >&2
        return 1
    fi

    # Extract host[:port][/path] — split off optional path
    local hostport="${rest%%/*}"

    # If there is a port, validate it is numeric
    if [[ "$hostport" == *:* ]]; then
        local port="${hostport##*:}"
        if [[ -z "$port" || "$port" =~ [^0-9] ]]; then
            echo "ERROR: AGAMEMNON_URL port must be numeric: ${url}" >&2
            return 1
        fi
    fi

    # Host must be non-empty
    local host="${hostport%%:*}"
    if [[ -z "$host" ]]; then
        echo "ERROR: AGAMEMNON_URL host is empty: ${url}" >&2
        return 1
    fi

    return 0
}


# Build the TLS flags array for curl based on environment variables.
# Populates the global _AGAMEMNON_TLS_FLAGS array; call once at source time.
_agamemnon_build_tls_flags() {
    _AGAMEMNON_TLS_FLAGS=()

    # Disable TLS verification escape hatch — warn loudly.
    local tls_verify="${AGAMEMNON_TLS_VERIFY:-true}"
    if [[ "$tls_verify" == "false" || "$tls_verify" == "0" ]]; then
        echo "WARNING: TLS verification is DISABLED (AGAMEMNON_TLS_VERIFY=${tls_verify})." >&2
        echo "  This is insecure and must not be used in production." >&2
        _AGAMEMNON_TLS_FLAGS+=(--insecure)
    fi

    # Custom CA certificate bundle.
    # Issue #167: validate path at source time.
    if [[ -n "${AGAMEMNON_CA_CERT:-}" ]]; then
        if [[ ! -r "${AGAMEMNON_CA_CERT}" ]]; then
            echo "WARN: AGAMEMNON_CA_CERT is set but path is not readable: ${AGAMEMNON_CA_CERT}" >&2
        fi
        _AGAMEMNON_TLS_FLAGS+=(--cacert "${AGAMEMNON_CA_CERT}")
    fi

    # Issue #168: warn when only one of CLIENT_CERT / CLIENT_KEY is specified.
    local have_cert=0 have_key=0
    [[ -n "${AGAMEMNON_CLIENT_CERT:-}" ]] && have_cert=1
    [[ -n "${AGAMEMNON_CLIENT_KEY:-}"  ]] && have_key=1
    if [[ $have_cert -ne $have_key ]]; then
        echo "WARN: mTLS misconfiguration — AGAMEMNON_CLIENT_CERT and AGAMEMNON_CLIENT_KEY must both be set or both be unset." >&2
        if [[ $have_cert -eq 0 ]]; then
            echo "  AGAMEMNON_CLIENT_CERT is unset; AGAMEMNON_CLIENT_KEY is set." >&2
        else
            echo "  AGAMEMNON_CLIENT_CERT is set; AGAMEMNON_CLIENT_KEY is unset." >&2
        fi
    fi

    # Mutual TLS: client certificate + key.
    # Issue #167: validate paths at source time.
    if [[ -n "${AGAMEMNON_CLIENT_CERT:-}" ]]; then
        if [[ ! -r "${AGAMEMNON_CLIENT_CERT}" ]]; then
            echo "WARN: AGAMEMNON_CLIENT_CERT is set but path is not readable: ${AGAMEMNON_CLIENT_CERT}" >&2
        fi
        _AGAMEMNON_TLS_FLAGS+=(--cert "${AGAMEMNON_CLIENT_CERT}")
    fi
    if [[ -n "${AGAMEMNON_CLIENT_KEY:-}" ]]; then
        if [[ ! -r "${AGAMEMNON_CLIENT_KEY}" ]]; then
            echo "WARN: AGAMEMNON_CLIENT_KEY is set but path is not readable: ${AGAMEMNON_CLIENT_KEY}" >&2
        fi
        _AGAMEMNON_TLS_FLAGS+=(--key "${AGAMEMNON_CLIENT_KEY}")
    fi
}

# Initialise TLS flags when this library is sourced.
_agamemnon_build_tls_flags

# Build auth headers array for curl. Populates _AUTH_HEADERS global array.
# Prefers Authorization: Bearer when AGAMEMNON_API_KEY is set.
# Falls back to no auth (backward compatible).
# set +x guard prevents the token value from appearing in bash -x / xtrace output.
_agamemnon_auth_headers() {
    _AUTH_HEADERS=()
    local _had_xtrace=0
    if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
    { set +x; } 2>/dev/null
    if [[ -n "${AGAMEMNON_API_KEY}" ]]; then
        _AUTH_HEADERS+=(-H "Authorization: Bearer ${AGAMEMNON_API_KEY}")
        _AUTH_HEADERS+=(-H "X-API-Key: ${AGAMEMNON_API_KEY}")
    fi
    if [[ $_had_xtrace -eq 1 ]]; then set -x; fi
}

# Validate that AGAMEMNON_URL is set and has a recognised scheme (http/https).
# Call this early in any entry-point script before issuing API calls.
validate_agamemnon_url() {
    local url="${AGAMEMNON_URL:-}"
    if [[ -z "$url" ]]; then
        echo "ERROR: AGAMEMNON_URL is not set." >&2
        echo "  Export AGAMEMNON_URL before running this script." >&2
        echo "  Example: export AGAMEMNON_URL=http://localhost:8080" >&2
        return 1
    fi
    case "$url" in
        http://*|https://*)
            ;;
        *)
            echo "ERROR: AGAMEMNON_URL has an unrecognised scheme: ${url}" >&2
            echo "  Expected a URL beginning with http:// or https://" >&2
            return 1
            ;;
    esac
}

# Check that Agamemnon is reachable before making calls.
# Also validates that AGAMEMNON_URL is a safe http/https URL (issue #120).
# Issue #116: a RETURN trap ensures any curl temp state (e.g. response output
# files written to disk on unexpected exit) is cleaned up on every exit path.
agamemnon_check_connection() {
    # Validate the URL before making any network calls.
    if ! _agamemnon_validate_url "${AGAMEMNON_URL}"; then
        echo "ERROR: Refusing to connect — AGAMEMNON_URL failed security validation." >&2
        return 1
    fi
    local _check_tmpfile
    _check_tmpfile="$(mktemp)"
    # Trap fires on RETURN (normal exit, error, or early return) to remove any
    # temp file left behind if curl exits unexpectedly mid-transfer.
    # shellcheck disable=SC2064
    trap "rm -f '${_check_tmpfile}'" RETURN

    _agamemnon_auth_headers
    local _had_xtrace=0
    if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
    { set +x; } 2>/dev/null
    local _conn_rc=0
    curl -sf --max-time 5 -o "${_check_tmpfile}" \
            "${_AGAMEMNON_TLS_FLAGS[@]+"${_AGAMEMNON_TLS_FLAGS[@]}"}" \
            "${_AUTH_HEADERS[@]+"${_AUTH_HEADERS[@]}"}" \
            "${AGAMEMNON_URL}/v1/health" 2>&1 || _conn_rc=$?
    if [[ $_had_xtrace -eq 1 ]]; then set -x; fi
    if [[ $_conn_rc -ne 0 ]]; then
        echo "ERROR: Cannot reach Agamemnon at ${AGAMEMNON_URL}" >&2
        echo "  Is Agamemnon running? Check your ProjectAgamemnon deployment." >&2
        return 1
    fi
}

# Internal helper: curl with retry on transient errors (connection refused, timeout, 5xx).
# Injects auth headers automatically when AGAMEMNON_API_KEY is set.
# Usage: _agamemnon_curl_retry [-X METHOD] URL [-H header] [-d body]
_agamemnon_curl_retry() {
    local max_attempts=3
    local delay=1
    local attempt=1
    local http_code response tmpfile curl_exit

    _agamemnon_auth_headers

    while [[ $attempt -le $max_attempts ]]; do
        tmpfile="$(mktemp)"
        local _had_xtrace=0
        if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
        { set +x; } 2>/dev/null
        http_code="$(curl -s --max-time "${AGAMEMNON_TIMEOUT:-10}" -w "%{http_code}" -o "$tmpfile" \
            "${_AGAMEMNON_TLS_FLAGS[@]+"${_AGAMEMNON_TLS_FLAGS[@]}"}" "${_AUTH_HEADERS[@]+"${_AUTH_HEADERS[@]}"}" "$@" 2>/dev/null)"
        curl_exit=$?
        if [[ $_had_xtrace -eq 1 ]]; then set -x; fi
        response="$(cat "$tmpfile")"
        rm -f "$tmpfile"

        # Success
        if [[ $curl_exit -eq 0 && "${http_code:0:1}" == "2" ]]; then
            echo "$response"
            return 0
        fi

        # Classify failure: transient = retry; permanent = fail immediately
        local is_transient=0
        if [[ $curl_exit -eq 7 || $curl_exit -eq 28 ]]; then
            is_transient=1
        elif [[ $curl_exit -ne 0 ]]; then
            is_transient=0
        elif [[ "${http_code:0:1}" == "5" ]]; then
            is_transient=1
        fi

        if [[ $is_transient -eq 0 ]]; then
            if [[ $curl_exit -ne 0 ]]; then
                echo "ERROR: curl failed (exit ${curl_exit}) for: $*" >&2
            else
                if [[ "$http_code" == "401" || "$http_code" == "403" ]]; then
                    echo "ERROR: Authentication failed (HTTP ${http_code}) — check AGAMEMNON_API_KEY" >&2
                else
                    echo "ERROR: HTTP ${http_code} from Agamemnon" >&2
                    echo "  URL: $*" >&2
                    if [[ -n "$response" ]]; then
                        echo "  Body: ${response}" >&2
                    fi
                fi
            fi
            return 1
        fi

        if [[ $attempt -lt $max_attempts ]]; then
            echo "WARN: Retry ${attempt}/${max_attempts} in ${delay}s (curl_exit=${curl_exit} http=${http_code}): $*" >&2
            sleep "$delay"
            delay=$((delay * 2))
        fi

        attempt=$((attempt + 1))
    done

    if [[ $curl_exit -ne 0 ]]; then
        echo "ERROR: curl failed after ${max_attempts} attempts (exit ${curl_exit}) for: $*" >&2
    else
        echo "ERROR: HTTP ${http_code} from Agamemnon after ${max_attempts} attempts" >&2
        echo "  URL: $*" >&2
        if [[ -n "$response" ]]; then
            echo "  Body: ${response}" >&2
        fi
    fi
    return 1
}

# Internal helper: single curl call with auth headers.
# All API functions delegate here. Uses retry logic from _agamemnon_curl_retry.
# Usage: _agamemnon_curl [-X METHOD] URL [-H header] [-d body]
_agamemnon_curl() {
    _agamemnon_curl_retry "$@"
}

# List all agents registered on this host.
agamemnon_list_agents() {
    _agamemnon_curl "${AGAMEMNON_URL}/v1/agents"
}

# Get a single agent by ID.
agamemnon_get_agent() {
    local agent_id="$1"
    _agamemnon_curl "${AGAMEMNON_URL}/v1/agents/${agent_id}"
}

# Get a single agent by name (rich resolution).
agamemnon_by_name() {
    local name="$1"
    _agamemnon_curl "${AGAMEMNON_URL}/v1/agents/by-name/${name}"
}

# Create a new agent. $1 = JSON body.
# Required fields: name, program, workingDirectory
agamemnon_create_agent() {
    local body="$1"
    _agamemnon_curl -X POST \
        "${AGAMEMNON_URL}/v1/agents" \
        -H 'Content-Type: application/json' \
        -d "${body}"
}

# Partially update an agent. $1 = agent ID, $2 = JSON patch body.
agamemnon_update_agent() {
    local agent_id="$1"
    local body="$2"
    _agamemnon_curl -X PATCH \
        "${AGAMEMNON_URL}/v1/agents/${agent_id}" \
        -H 'Content-Type: application/json' \
        -d "${body}"
}

# Delete an agent (hard delete creates a backup).
# Always stop first for graceful shutdown.
agamemnon_delete_agent() {
    local agent_id="$1"
    _agamemnon_curl -X DELETE "${AGAMEMNON_URL}/v1/agents/${agent_id}?hard=true"
}

# Start an agent (starts tmux session + AI program).
agamemnon_wake_agent() {
    local agent_id="$1"
    _agamemnon_curl -X POST \
        "${AGAMEMNON_URL}/v1/agents/${agent_id}/start" \
        -H 'Content-Type: application/json' \
        -d '{}'
}

# Stop an agent (graceful stop: Ctrl-C, exit, kill tmux).
agamemnon_hibernate_agent() {
    local agent_id="$1"
    _agamemnon_curl -X POST \
        "${AGAMEMNON_URL}/v1/agents/${agent_id}/stop" \
        -H 'Content-Type: application/json' \
        -d '{}'
}

# Create a Docker-deployed agent.
agamemnon_docker_create() {
    local body="$1"
    _agamemnon_curl -X POST \
        "${AGAMEMNON_URL}/v1/agents/docker" \
        -H 'Content-Type: application/json' \
        -d "${body}"
}

# Helper: get agent ID by name. Returns empty string if not found.
agamemnon_id_by_name() {
    local name="$1"
    agamemnon_list_agents | jq -r --arg name "$name" \
        '.[] | select(.name == $name) | .id // empty'
}

# Helper: get agent status by name. Returns "unknown" if not found.
agamemnon_status_by_name() {
    local name="$1"
    local status
    status="$(agamemnon_list_agents | jq -r --arg name "$name" \
        'first(.[] | select(.name == $name) | .status) // "unknown"')"
    echo "${status:-unknown}"
}
