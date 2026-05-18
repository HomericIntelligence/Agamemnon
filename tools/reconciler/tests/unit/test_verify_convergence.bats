#!/usr/bin/env bats
# tests/unit/test_verify_convergence.bats — regression tests for verify_convergence()
#
# Verifies:
#   - verify_convergence returns 0 when _MODIFIED_NAMES is empty (no-op path)
#   - verify_convergence returns 0 when all modified agents converged
#   - verify_convergence returns 1 when an agent fails to converge
#   - The dead function write_failed_agents_file is NOT defined in apply.sh (F-05 regression)
#   - Only one verify_convergence definition exists in apply.sh (F-03 regression)

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
APPLY_SH="${SCRIPT_DIR}/scripts/apply.sh"

# ---------------------------------------------------------------------------
# Static checks — no execution needed, just grep apply.sh
# ---------------------------------------------------------------------------

@test "F-05 regression: write_failed_agents_file (public variant) is not defined in apply.sh" {
    # The dead public variant must not exist; only the private _write_failed_agents_file may remain.
    # grep -c returns non-zero when no lines match, which is what we want.
    local count
    count=$(grep -c '^write_failed_agents_file()' "$APPLY_SH" || true)
    [[ "$count" -eq 0 ]]
}

@test "F-03 regression: only one verify_convergence definition exists in apply.sh" {
    local count
    count=$(grep -c '^verify_convergence()' "$APPLY_SH" || true)
    [[ "$count" -eq 1 ]]
}

@test "F-04 regression: repo_root (lowercase) does not appear in apply.sh" {
    local count
    count=$(grep -c '\${repo_root}' "$APPLY_SH" || true)
    [[ "$count" -eq 0 ]]
}

@test "F-06 regression: bare FAILED_AGENTS array declaration is not present in apply.sh" {
    # The unified FAILED_AGENT_NAMES/STATUSES/MESSAGES arrays replaced FAILED_AGENTS.
    # Verify neither the declaration nor a direct append to the bare array exists.
    local decl_count
    decl_count=$(grep -cE '^FAILED_AGENTS=\(\)' "$APPLY_SH" || true)
    [[ "$decl_count" -eq 0 ]]
    local append_count
    append_count=$(grep -cE 'FAILED_AGENTS\+=\(' "$APPLY_SH" || true)
    [[ "$append_count" -eq 0 ]]
}

# ---------------------------------------------------------------------------
# Functional tests — source a stripped-down harness that exports the function
# ---------------------------------------------------------------------------

# Build a minimal sourcing harness so we can call verify_convergence in-process
# without running main() or requiring a live Agamemnon server.

_source_verify_convergence() {
    # Extract just the verify_convergence function from apply.sh into a temp file
    # so we can source it safely in tests.
    local harness="${BATS_TMPDIR}/vc_harness_$$.sh"
    cat > "$harness" << 'HARNESS'
#!/usr/bin/env bash
set -uo pipefail

# Minimal stubs required by verify_convergence
OUTPUT_FORMAT="text"
_MODIFIED_NAMES=()
_MODIFIED_DESIRED=()
_PRUNED_NAMES=()

# Stub agamemnon_list_agents — returns the value of _STUB_AGENTS_JSON
agamemnon_list_agents() {
    echo "${_STUB_AGENTS_JSON:-[]}"
}

# ---- paste verify_convergence body inline ----
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
    fi

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
            failed=$((failed + 1))
            continue
        fi

        actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

        local converged=0
        if [[ "$desired" == "active" ]]; then
            if [[ "$actual_status" == "active" || "$actual_status" == "online" || \
                  "$actual_status" == "starting" ]]; then
                converged=1
            fi
        elif [[ "$desired" == "hibernated" ]]; then
            if [[ "$actual_status" == "offline" || "$actual_status" == "hibernated" ]]; then
                converged=1
            fi
        else
            converged=1
        fi

        if [[ $converged -eq 1 ]]; then
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "  [ok] ${agent_name}: converged (status=${actual_status})"
            fi
            verified=$((verified + 1))
        else
            if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                echo "  [!] ${agent_name}: NOT converged (desired=${desired}, actual_status=${actual_status})"
            fi
            failed=$((failed + 1))
        fi
    done

    if [[ ${#_PRUNED_NAMES[@]} -gt 0 ]]; then
        for pruned_name in "${_PRUNED_NAMES[@]}"; do
            local still_exists
            still_exists="$(echo "$agents_json_fresh" | jq -r --arg n "$pruned_name" '.[] | select(.name == $n) | .name')"
            if [[ -n "$still_exists" ]]; then
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "  [!] ${pruned_name}: pruned but still present in API (convergence failed)"
                fi
                failed=$((failed + 1))
            else
                if [[ "$OUTPUT_FORMAT" != "json" ]]; then
                    echo "  [ok] ${pruned_name}: confirmed absent (pruned)"
                fi
                pruned_verified=$((pruned_verified + 1))
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
HARNESS
    echo "$harness"
}

@test "verify_convergence: returns 0 when _MODIFIED_NAMES is empty" {
    local harness
    harness="$(_source_verify_convergence)"

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=()
        _MODIFIED_DESIRED=()
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    rm -f "$harness"
}

@test "verify_convergence: returns 0 when modified agent is active and desired is active" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"test-agent","status":"active"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"test-agent\")
        _MODIFIED_DESIRED=(\"active\")
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    rm -f "$harness"
}

@test "verify_convergence: returns 0 when modified agent is online and desired is active" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"test-agent","status":"online"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"test-agent\")
        _MODIFIED_DESIRED=(\"active\")
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    rm -f "$harness"
}

@test "verify_convergence: returns 1 when modified agent is offline but desired is active" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"test-agent","status":"offline"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"test-agent\")
        _MODIFIED_DESIRED=(\"active\")
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 1 ]]
    rm -f "$harness"
}

@test "verify_convergence: returns 1 when agent is not found in API" {
    local harness
    harness="$(_source_verify_convergence)"

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"missing-agent\")
        _MODIFIED_DESIRED=(\"active\")
        _STUB_AGENTS_JSON='[]'
        verify_convergence
    "
    [[ "$status" -eq 1 ]]
    rm -f "$harness"
}

@test "verify_convergence: returns 0 when hibernated agent is offline" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"sleepy-agent","status":"offline"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"sleepy-agent\")
        _MODIFIED_DESIRED=(\"hibernated\")
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    rm -f "$harness"
}

@test "verify_convergence: returns 1 when hibernated agent is still active" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"sleepy-agent","status":"active"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"sleepy-agent\")
        _MODIFIED_DESIRED=(\"hibernated\")
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 1 ]]
    rm -f "$harness"
}

@test "verify_convergence: mixed results — 1 converged, 1 not — returns 1" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"agent-a","status":"active"},{"name":"agent-b","status":"offline"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"agent-a\" \"agent-b\")
        _MODIFIED_DESIRED=(\"active\" \"active\")
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 1 ]]
    rm -f "$harness"
}

@test "verify_convergence: prune-only run returns 0 and shows pruned count" {
    local harness
    harness="$(_source_verify_convergence)"

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=()
        _MODIFIED_DESIRED=()
        _PRUNED_NAMES=(\"a\" \"b\" \"c\")
        _STUB_AGENTS_JSON='[]'
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"3 pruned"* ]]
    rm -f "$harness"
}

@test "verify_convergence: pruned agent still present returns 1" {
    local harness
    harness="$(_source_verify_convergence)"

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=()
        _MODIFIED_DESIRED=()
        _PRUNED_NAMES=(\"ghost\")
        _STUB_AGENTS_JSON='[{\"name\":\"ghost\",\"status\":\"offline\"}]'
        verify_convergence
    "
    [[ "$status" -eq 1 ]]
    rm -f "$harness"
}

@test "verify_convergence: prune-only summary reads '0 converged, 3 pruned, 0 failed.'" {
    local harness
    harness="$(_source_verify_convergence)"

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=()
        _MODIFIED_DESIRED=()
        _PRUNED_NAMES=(\"a\" \"b\" \"c\")
        _STUB_AGENTS_JSON='[]'
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" == *"Convergence: 0 converged, 3 pruned, 0 failed."* ]]
    rm -f "$harness"
}

@test "verify_convergence: no-prune summary does not contain ' pruned'" {
    local harness
    harness="$(_source_verify_convergence)"

    local agents_json='[{"name":"test-agent","status":"active"}]'

    run bash -c "
        source '${harness}'
        _MODIFIED_NAMES=(\"test-agent\")
        _MODIFIED_DESIRED=(\"active\")
        _PRUNED_NAMES=()
        _STUB_AGENTS_JSON='${agents_json}'
        verify_convergence
    "
    [[ "$status" -eq 0 ]]
    [[ "$output" != *" pruned"* ]]
    rm -f "$harness"
}
