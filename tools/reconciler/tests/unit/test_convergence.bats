#!/usr/bin/env bats
# tests/unit/test_convergence.bats — regression guard for issue #369
#
# Issue #369: dual verify_convergence definition — second definition shadowed
# the first, and the call at (former) line 602 passed no args, causing the
# second definition to iterate zero files and silently report success.
#
# These tests verify:
#   - Single definition: verify_convergence takes no positional args
#   - Non-converged agent returns exit 1
#   - All-converged returns exit 0
#   - _PRUNED_NAMES check is executed (pruned agent still present → exit 1)
#   - _PRUNED_NAMES check passes when pruned agent is absent
#   - Empty _MODIFIED_NAMES with empty _PRUNED_NAMES returns exit 0 immediately

# ---------------------------------------------------------------------------
# Helper: build a minimal verify_convergence harness that extracts the logic
# from apply.sh without requiring a live Agamemnon server.
# ---------------------------------------------------------------------------

_make_verify_script() {
    local script="$1"
    local agents_json="$2"       # JSON array returned by the stub API
    local modified_names="$3"    # space-separated agent names
    local modified_desired="$4"  # space-separated desired states (parallel to names)
    local pruned_names="$5"      # space-separated pruned agent names

    cat > "$script" << SCRIPT
#!/usr/bin/env bash
set -euo pipefail

OUTPUT_FORMAT="text"

# Stub agamemnon_list_agents returns fixed JSON
agamemnon_list_agents() {
    cat <<'JSON'
${agents_json}
JSON
}

# Populate global arrays from args
_MODIFIED_NAMES=()
_MODIFIED_DESIRED=()
_PRUNED_NAMES=()

IFS=' ' read -r -a _MODIFIED_NAMES <<< "${modified_names}" 2>/dev/null || true
IFS=' ' read -r -a _MODIFIED_DESIRED <<< "${modified_desired}" 2>/dev/null || true
IFS=' ' read -r -a _PRUNED_NAMES <<< "${pruned_names}" 2>/dev/null || true

$(declare -f verify_convergence_impl)

verify_convergence_impl
SCRIPT
    chmod +x "$script"
}

# Inline the verify_convergence logic for isolated unit testing
# (avoids sourcing the entire apply.sh which requires Agamemnon deps)
verify_convergence_impl() {
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

    local agents_json_fresh
    agents_json_fresh="$(agamemnon_list_agents)"

    for i in "${!_MODIFIED_NAMES[@]}"; do
        local agent_name="${_MODIFIED_NAMES[$i]}"
        local desired="${_MODIFIED_DESIRED[$i]}"
        local actual_json actual_status

        actual_json="$(echo "$agents_json_fresh" | jq -r --arg n "$agent_name" '.[] | select(.name == $n)')"

        if [[ -z "$actual_json" ]]; then
            echo "  [!] ${agent_name}: NOT FOUND in Agamemnon after apply (convergence failed)"
            failed=$((failed + 1))
            continue
        fi

        actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

        local converged=0
        if [[ "$desired" == "active" ]]; then
            [[ "$actual_status" == "active" || "$actual_status" == "online" || \
               "$actual_status" == "starting" ]] && converged=1
        elif [[ "$desired" == "hibernated" ]]; then
            [[ "$actual_status" == "offline" || "$actual_status" == "hibernated" ]] && converged=1
        else
            converged=1
        fi

        if [[ $converged -eq 1 ]]; then
            echo "  [ok] ${agent_name}: converged (status=${actual_status})"
            verified=$((verified + 1))
        else
            echo "  [!] ${agent_name}: NOT converged (desired=${desired}, actual_status=${actual_status})"
            failed=$((failed + 1))
        fi
    done

    # Verify pruned agents are gone from the API
    if [[ ${#_PRUNED_NAMES[@]} -gt 0 ]]; then
        for pruned_name in "${_PRUNED_NAMES[@]}"; do
            local still_exists
            still_exists="$(echo "$agents_json_fresh" | jq -r --arg n "$pruned_name" '.[] | select(.name == $n) | .name')"
            if [[ -n "$still_exists" ]]; then
                echo "  [!] ${pruned_name}: pruned but still present in API (convergence failed)"
                failed=$((failed + 1))
            else
                echo "  [ok] ${pruned_name}: confirmed absent (pruned)"
                pruned_verified=$((pruned_verified + 1))
            fi
        done
    fi

    if [[ ${#_PRUNED_NAMES[@]} -gt 0 ]]; then
        echo "Convergence: ${verified} converged, ${pruned_verified} pruned, ${failed} failed."
    else
        echo "Convergence: ${verified} converged, ${failed} failed."
    fi

    if [[ $failed -gt 0 ]]; then
        return 1
    fi
    return 0
}

TEMP_TEST_DIR=""

setup() {
    TEMP_TEST_DIR="$(mktemp -d)"
    export TEMP_TEST_DIR
}

teardown() {
    [[ -n "$TEMP_TEST_DIR" ]] && rm -rf "$TEMP_TEST_DIR"
}

# ---------------------------------------------------------------------------
# #369 regression: verify_convergence accepts NO positional arguments
# The bug was a second definition taking positional args; when called with
# no args that second definition's yaml_files was empty → silent success.
# ---------------------------------------------------------------------------

@test "verify_convergence signature: called with zero args, not positional params" {
    # Confirm the canonical definition is the only one by grepping apply.sh
    SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
    count="$(grep -c '^verify_convergence()' "${SCRIPT_DIR}/scripts/apply.sh")"
    [[ "$count" -eq 1 ]]
}

@test "verify_convergence: shadowing second definition comment is gone from apply.sh" {
    SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
    # The second definition was introduced by this exact comment line.
    # Its removal confirms the shadowing definition is gone.
    ! grep -qF 'verify_convergence — after apply, confirm each agent reached its desired state.' \
        "${SCRIPT_DIR}/scripts/apply.sh"
}

# ---------------------------------------------------------------------------
# Behavioural: non-converged agent → exit 1
# ---------------------------------------------------------------------------

@test "verify_convergence: returns 1 when agent did not converge (offline, desired active)" {
    _MODIFIED_NAMES=("my-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=()

    agamemnon_list_agents() {
        echo '[{"name":"my-agent","status":"offline"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 1 ]]
    [[ "$output" == *"NOT converged"* ]]
}

@test "verify_convergence: returns 1 when agent is not found in API" {
    _MODIFIED_NAMES=("missing-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=()

    agamemnon_list_agents() {
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 1 ]]
    [[ "$output" == *"NOT FOUND"* ]]
}

# ---------------------------------------------------------------------------
# Behavioural: converged agent → exit 0
# ---------------------------------------------------------------------------

@test "verify_convergence: returns 0 when agent is active and desired active" {
    _MODIFIED_NAMES=("my-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=()

    agamemnon_list_agents() {
        echo '[{"name":"my-agent","status":"active"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"converged"* ]]
}

@test "verify_convergence: returns 0 when agent is online and desired active" {
    _MODIFIED_NAMES=("my-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=()

    agamemnon_list_agents() {
        echo '[{"name":"my-agent","status":"online"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
}

@test "verify_convergence: returns 0 when agent is offline and desired hibernated" {
    _MODIFIED_NAMES=("my-agent")
    _MODIFIED_DESIRED=("hibernated")
    _PRUNED_NAMES=()

    agamemnon_list_agents() {
        echo '[{"name":"my-agent","status":"offline"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Behavioural: _PRUNED_NAMES check
# ---------------------------------------------------------------------------

@test "verify_convergence: returns 1 when pruned agent still present in API" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=("old-agent")

    agamemnon_list_agents() {
        echo '[{"name":"old-agent","status":"offline"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 1 ]]
    [[ "$output" == *"pruned but still present"* ]]
}

@test "verify_convergence: returns 0 when pruned agent is absent from API" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=("old-agent")

    agamemnon_list_agents() {
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Behavioural: empty arrays → early return (no API call needed)
# ---------------------------------------------------------------------------

@test "verify_convergence: returns 0 immediately when both arrays empty" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=()

    agamemnon_list_calls=0
    agamemnon_list_agents() {
        agamemnon_list_calls=$((agamemnon_list_calls + 1))
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Behavioural: mixed modified + pruned in one pass
# ---------------------------------------------------------------------------

@test "verify_convergence: checks both modified agents and pruned agents in one pass" {
    _MODIFIED_NAMES=("live-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=("dead-agent")

    agamemnon_list_agents() {
        # live-agent is active (converged); dead-agent still present (prune failed)
        echo '[{"name":"live-agent","status":"active"},{"name":"dead-agent","status":"offline"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 1 ]]
    [[ "$output" == *"[ok] live-agent"* ]]
    [[ "$output" == *"pruned but still present"* ]]
}

@test "verify_convergence: all pass when modified converged and pruned agent absent" {
    _MODIFIED_NAMES=("live-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=("dead-agent")

    agamemnon_list_agents() {
        echo '[{"name":"live-agent","status":"active"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Issue #408: prune-only run prints prune-specific header
# ---------------------------------------------------------------------------

@test "verify_convergence: prune-only run prints 'Verifying N pruned agent(s) are absent' header" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=("old-agent" "gone-agent")

    agamemnon_list_agents() {
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Verifying 2 pruned agent(s) are absent"* ]]
}

# ---------------------------------------------------------------------------
# #407: pruned_verified counter and conditional summary line
# ---------------------------------------------------------------------------

@test "verify_convergence: prune-only run increments pruned_verified, returns 0" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=("agent-x" "agent-y" "agent-z")

    agamemnon_list_agents() {
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"3 pruned"* ]]
    [[ "$output" == *"0 failed"* ]]
}

@test "verify_convergence: prune-only run summary reads '0 converged, 3 pruned, 0 failed.'" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=("agent-x" "agent-y" "agent-z")

    agamemnon_list_agents() {
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Convergence: 0 converged, 3 pruned, 0 failed."* ]]
}

@test "verify_convergence: pruned agent confirmed absent emits ok line" {
    _MODIFIED_NAMES=()
    _MODIFIED_DESIRED=()
    _PRUNED_NAMES=("gone-agent")

    agamemnon_list_agents() {
        echo '[]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"[ok] gone-agent: confirmed absent (pruned)"* ]]
}

@test "verify_convergence: no-prune run summary does not contain 'pruned'" {
    _MODIFIED_NAMES=("my-agent")
    _MODIFIED_DESIRED=("active")
    _PRUNED_NAMES=()

    agamemnon_list_agents() {
        echo '[{"name":"my-agent","status":"active"}]'
    }

    run verify_convergence_impl
    [[ "$status" -eq 0 ]]
    [[ "$output" != *" pruned"* ]]
}
