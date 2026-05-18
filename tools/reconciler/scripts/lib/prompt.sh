#!/usr/bin/env bash
# scripts/lib/prompt.sh — Interactive confirmation helper for Myrmidons scripts
#
# Provides confirm_with_timeout() so all yes/no prompts share the same
# TTY-detection, timeout, and non-TTY fallback logic. This prevents bare
# `read -r` calls (no timeout) from hanging in CI or pipeline contexts.
#
# Usage:
#   source scripts/lib/prompt.sh
#   if confirm_with_timeout "Apply changes? [y/N]"; then
#       do_the_thing
#   fi
#
# Non-TTY behavior (CI / piped stdin):
#   Reads one line with a 1-second timeout. If nothing arrives, falls back to
#   the DEFAULT argument. The caller controls the default; scripts that must
#   be safe-by-default should pass "n" (the built-in default).

# confirm_with_timeout PROMPT [TIMEOUT [DEFAULT]]
#
# Writes PROMPT to stderr, waits up to TIMEOUT seconds for a y/n reply.
# Returns 0 for yes (y/Y/yes/YES), 1 for anything else.
#
#   PROMPT   — text shown to the user (default: "Continue? [y/N]")
#   TIMEOUT  — seconds to wait on a TTY (default: 30)
#   DEFAULT  — reply assumed on timeout or non-TTY with no piped input (default: "n")
confirm_with_timeout() {
    local prompt="${1:-Continue? [y/N]}"
    local timeout="${2:-30}"
    local default="${3:-n}"
    local reply

    if [[ -t 0 ]]; then
        printf '%s ' "$prompt" >&2
        if ! read -r -t "$timeout" reply; then
            printf '\n(Timed out after %ss — defaulting to %s)\n' "$timeout" "$default" >&2
            reply="$default"
        fi
    else
        # Non-TTY (CI, pipe): read one line if available within 1s, else use default
        if ! IFS= read -r -t 1 reply 2>/dev/null; then
            reply="$default"
        fi
    fi

    [[ "${reply,,}" == y* ]]
}
