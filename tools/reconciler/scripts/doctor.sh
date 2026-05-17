#!/usr/bin/env bash
# scripts/doctor.sh — Environment health check for Myrmidons
#
# Validates the local setup: required tools, Agamemnon connectivity,
# YAML schema validity, git hooks, and pixi environment.
#
# Usage:
#   ./scripts/doctor.sh
#   just doctor
#
# Exit codes:
#   0 = all checks passed
#   1 = one or more checks failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=scripts/lib/config.sh
source "${SCRIPT_DIR}/lib/config.sh"
load_config

# shellcheck source=scripts/lib/api.sh
source "${SCRIPT_DIR}/lib/api.sh"

# MYRM_AIM_HOST is populated by load_config; fall back to env/default for safety.
AGAMEMNON_URL="${AGAMEMNON_URL:-${MYRM_AIM_HOST:-http://localhost:8080}}"

# Flags
SKIP_CONNECTIVITY=false
SKIP_HOOKS=false

# Parse command-line flags
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-connectivity) SKIP_CONNECTIVITY=true; shift ;;
        --skip-hooks) SKIP_HOOKS=true; shift ;;
        *) shift ;;
    esac
done

# Counters
PASS=0
FAIL=0

# =============================================================================
# Output helpers
# =============================================================================

# Only use ANSI colors when stdout is a TTY (not piped)
if [[ -t 1 ]]; then
    green()  { printf '\033[0;32m%s\033[0m' "$*"; }
    red()    { printf '\033[0;31m%s\033[0m' "$*"; }
    yellow() { printf '\033[1;33m%s\033[0m' "$*"; }
else
    green()  { printf '%s' "$*"; }  # allow-duplicate-function: if/else TTY vs plain-text branch
    red()    { printf '%s' "$*"; }  # allow-duplicate-function: if/else TTY vs plain-text branch
    yellow() { printf '%s' "$*"; }  # allow-duplicate-function: if/else TTY vs plain-text branch
fi

pass() {
    local label="$1"
    local detail="${2:-}"
    printf '  %s %s' "$(green "[PASS]")" "$label"
    if [[ -n "$detail" ]]; then
        printf ' (%s)' "$detail"
    fi
    printf '\n'
    PASS=$((PASS + 1))
}

fail() {
    local label="$1"
    local hint="${2:-}"
    printf '  %s %s\n' "$(red "[FAIL]")" "$label"
    if [[ -n "$hint" ]]; then
        printf '         %s\n' "$(yellow "→ ${hint}")"
    fi
    FAIL=$((FAIL + 1))
}

warn() {
    local label="$1"
    local hint="${2:-}"
    printf '  %s %s\n' "$(yellow "[WARN]")" "$label"
    if [[ -n "$hint" ]]; then
        printf '         %s\n' "$(yellow "→ ${hint}")"
    fi
}

section() {
    printf '\n%s\n' "$*"
}

# =============================================================================
# Check 1: Required tools
# =============================================================================

check_tools() {
    section "Check 1: Required tools"

    local tools=(yq jq curl)
    local install_hints=(
        "https://github.com/mikefarah/yq"
        "apt install jq  /  brew install jq"
        "apt install curl  /  brew install curl"
    )

    for i in "${!tools[@]}"; do
        local cmd="${tools[$i]}"
        local hint="${install_hints[$i]}"
        if command -v "$cmd" &>/dev/null; then
            local version
            version="$("$cmd" --version 2>&1 | head -1)" || version="(version unknown)"
            pass "$cmd" "$version"
        else
            fail "$cmd not found" "Install: ${hint}"
        fi
    done
}

# =============================================================================
# Check 2: Agamemnon connectivity
# =============================================================================

check_connectivity() {
    section "Check 2: Agamemnon connectivity"

    printf '  URL: %s\n' "$AGAMEMNON_URL"

    # Validate URL format (#118)
    if [[ -z "$AGAMEMNON_URL" ]]; then
        fail "AGAMEMNON_URL is not set" \
            "Export AGAMEMNON_URL before running (e.g. export AGAMEMNON_URL=http://localhost:8080)"
        return
    fi
    case "$AGAMEMNON_URL" in
        http://*|https://*) ;;
        *)
            fail "AGAMEMNON_URL has an unrecognised scheme: ${AGAMEMNON_URL}" \
                "Expected a URL beginning with http:// or https://"
            return
            ;;
    esac

    if [[ "$SKIP_CONNECTIVITY" == "true" ]]; then
        warn "Agamemnon connectivity (skipped via --skip-connectivity)"
        return 0
    fi

    # Health endpoint — guard xtrace so AGAMEMNON_API_KEY does not leak if set
    _agamemnon_auth_headers
    local http_code
    local _had_xtrace=0
    if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
    { set +x; } 2>/dev/null
    http_code="$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
        "${_AGAMEMNON_TLS_FLAGS[@]+"${_AGAMEMNON_TLS_FLAGS[@]}"}" \
        "${_AUTH_HEADERS[@]+"${_AUTH_HEADERS[@]}"}" \
        "${AGAMEMNON_URL}/v1/health" 2>/dev/null)" || http_code="000"
    if [[ $_had_xtrace -eq 1 ]]; then set -x; fi

    if [[ "$http_code" == "200" ]]; then
        pass "Agamemnon reachable at ${AGAMEMNON_URL}" "HTTP ${http_code}"
    elif [[ "$http_code" == "000" ]]; then
        fail "Agamemnon unreachable at ${AGAMEMNON_URL}" \
            "Is ProjectAgamemnon running? Check AGAMEMNON_URL env var."
        return
    else
        fail "Agamemnon health check failed" \
            "HTTP ${http_code} — check ProjectAgamemnon logs"
        return
    fi

    # List agents endpoint (API functional check) — same xtrace guard
    local agents_code
    _had_xtrace=0
    if [[ "$-" == *x* ]]; then _had_xtrace=1; fi
    { set +x; } 2>/dev/null
    agents_code="$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
        "${_AGAMEMNON_TLS_FLAGS[@]+"${_AGAMEMNON_TLS_FLAGS[@]}"}" \
        "${_AUTH_HEADERS[@]+"${_AUTH_HEADERS[@]}"}" \
        "${AGAMEMNON_URL}/v1/agents" 2>/dev/null)" || agents_code="000"
    if [[ $_had_xtrace -eq 1 ]]; then set -x; fi

    if [[ "${agents_code:0:1}" == "2" ]]; then
        pass "Agamemnon API responding" "GET /v1/agents → HTTP ${agents_code}"
    else
        fail "Agamemnon API not responding correctly" \
            "GET /v1/agents returned HTTP ${agents_code}"
    fi
}

# =============================================================================
# Check 3: YAML validation
# =============================================================================

check_yaml() {
    section "Check 3: YAML validation"

    if ! command -v yq &>/dev/null; then
        warn "Skipping YAML check (yq not installed)"
        return
    fi

    local errors=0
    local checked=0
    local yaml_files=()

    while IFS= read -r -d '' file; do
        [[ "$file" == *"/_templates/"* ]] && continue
        yaml_files+=("$file")
    done < <(find "${REPO_ROOT}/agents" "${REPO_ROOT}/fleets" \
        -name "*.yaml" -print0 2>/dev/null)

    if [[ ${#yaml_files[@]} -eq 0 ]]; then
        warn "No agent/fleet YAML files found"
        return
    fi

    for file in "${yaml_files[@]}"; do
        checked=$((checked + 1))
        local rel="${file#"${REPO_ROOT}/"}"

        # Syntax check
        if ! yq eval '.' "$file" > /dev/null 2>&1; then
            fail "${rel}: invalid YAML syntax"
            errors=$((errors + 1))
            continue
        fi

        local api_version kind
        api_version="$(yq eval '.apiVersion // ""' "$file")"
        kind="$(yq eval '.kind // ""' "$file")"

        if [[ "$api_version" != "myrmidons/v1" ]]; then
            fail "${rel}: wrong apiVersion ('${api_version}')" \
                "Expected myrmidons/v1"
            errors=$((errors + 1))
            continue
        fi

        if [[ "$kind" != "Agent" && "$kind" != "Fleet" ]]; then
            fail "${rel}: invalid kind ('${kind}')" \
                "Expected Agent or Fleet"
            errors=$((errors + 1))
            continue
        fi

        if [[ "$kind" == "Fleet" ]]; then
            local fleet_name
            fleet_name="$(yq eval '.metadata.name // ""' "$file")"
            if [[ -z "$fleet_name" ]]; then
                fail "${rel}: metadata.name required in Fleet"
                errors=$((errors + 1))
            else
                pass "${rel}" "Fleet: ${fleet_name}"
            fi
            continue
        fi

        # Agent field validation
        local name host program workdir desired_state deploy_type
        name="$(yq eval '.metadata.name // ""' "$file")"
        host="$(yq eval '.metadata.host // ""' "$file")"
        program="$(yq eval '.spec.program // ""' "$file")"
        workdir="$(yq eval '.spec.workingDirectory // ""' "$file")"
        desired_state="$(yq eval '.spec.desiredState // ""' "$file")"
        deploy_type="$(yq eval '.spec.deployment.type // "local"' "$file")"

        local field_errors=()
        [[ -z "$name" ]] && field_errors+=("metadata.name is required")
        [[ -z "$host" ]] && field_errors+=("metadata.host is required")
        [[ -z "$program" ]] && field_errors+=("spec.program is required")
        [[ -z "$workdir" ]] && field_errors+=("spec.workingDirectory is required")
        if [[ -n "$desired_state" && "$desired_state" != "active" && "$desired_state" != "hibernated" ]]; then
            field_errors+=("spec.desiredState must be 'active' or 'hibernated'")
        fi
        if [[ "$deploy_type" != "local" && "$deploy_type" != "docker" ]]; then
            field_errors+=("spec.deployment.type must be 'local' or 'docker'")
        fi

        if [[ ${#field_errors[@]} -gt 0 ]]; then
            fail "${rel}: field validation failed"
            for err in "${field_errors[@]}"; do
                printf '           - %s\n' "$err"
            done
            errors=$((errors + 1))
        else
            pass "${rel}" "Agent: ${name}"
        fi
    done

    if [[ $errors -eq 0 ]]; then
        printf '  %s\n' "$(green "All ${checked} file(s) valid.")"
    fi
}

# =============================================================================
# Check 4: Git hooks
# =============================================================================

check_hooks() {
    section "Check 4: Git hooks"

    if [[ "$SKIP_HOOKS" == "true" ]]; then
        warn "pre-commit hook check skipped (--skip-hooks)"
        return
    fi

    local hook_src="${REPO_ROOT}/hooks/pre-commit"
    local hook_dst="${REPO_ROOT}/.git/hooks/pre-commit"

    if [[ ! -f "$hook_src" ]]; then
        warn "hooks/pre-commit source not found in repo"
        return
    fi

    if [[ ! -f "$hook_dst" ]]; then
        fail "pre-commit hook not installed" \
            "Run: just install-hooks"
    elif [[ ! -x "$hook_dst" ]]; then
        fail "pre-commit hook not executable" \
            "Run: chmod +x .git/hooks/pre-commit"
    else
        pass "pre-commit hook installed and executable"
    fi
}

# =============================================================================
# Check 5: pixi environment
# =============================================================================

check_pixi() {
    section "Check 5: pixi environment"

    if ! command -v pixi &>/dev/null; then
        warn "pixi not found — skipping environment check" \
            "Install: https://prefix.dev/docs/pixi/overview"
        return
    fi

    local version
    version="$(pixi --version 2>&1 | head -1)" || version="(unknown)"
    pass "pixi installed" "$version"

    local pixi_toml="${REPO_ROOT}/pixi.toml"
    if [[ ! -f "$pixi_toml" ]]; then
        warn "pixi.toml not found — skipping environment validation"
        return
    fi

    # Check if the pixi environment is initialised (has a lockfile)
    local lock="${REPO_ROOT}/pixi.lock"
    if [[ -f "$lock" ]]; then
        pass "pixi.lock present"
    else
        warn "pixi.lock not found" \
            "Run: pixi install"
    fi

    # Verify that core deps resolve inside the pixi shell
    if pixi run --manifest-path "$pixi_toml" yq --version &>/dev/null 2>&1; then
        pass "pixi environment active (yq accessible)"
    else
        warn "pixi environment not active or deps missing" \
            "Run: pixi install"
    fi
}

# =============================================================================
# Summary
# =============================================================================

print_summary() {
    local total=$((PASS + FAIL))
    printf '\n'
    printf '━%.0s' {1..50}
    printf '\n'
    printf 'Summary: %s passed, %s failed (of %s checks)\n' \
        "$(green "$PASS")" "$(if [[ $FAIL -gt 0 ]]; then red "$FAIL"; else echo "$FAIL"; fi)" "$total"

    if [[ $FAIL -gt 0 ]]; then
        printf '\n%s\n' "$(red "Doctor found issues. Fix the failures above before running scripts.")"
        return 1
    else
        printf '\n%s\n' "$(green "All checks passed. Environment looks healthy.")"
        return 0
    fi
}

# =============================================================================
# Main
# =============================================================================

main() {
    printf 'Myrmidons Doctor — environment health check\n'
    printf 'AGAMEMNON_URL=%s\n' "$AGAMEMNON_URL"

    check_tools
    check_connectivity
    check_yaml
    check_hooks
    check_pixi

    print_summary
}

main "$@"
