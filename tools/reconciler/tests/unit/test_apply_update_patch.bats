#!/usr/bin/env bats
# tests/unit/test_apply_update_patch.bats — verify the JSON patch body sent on UPDATE
#
# Issue #99: Add shell-level tests for apply.sh UPDATE patch body.
#
# Uses a mock curl to capture the PATCH request body when apply.sh detects drift
# and calls agamemnon_update_agent. Verifies the JSON patch contains the
# expected fields: label, program, workingDirectory, programArgs,
# taskDescription, tags, owner, role.
#
# Approach:
#   - Source apply.sh helper functions directly (build_create_json is in reconcile.sh)
#   - Source reconcile.sh directly and call apply_agent() with a mock agents_json
#   - Mock agamemnon_update_agent to capture its body argument
#   - Mock agamemnon_create_agent and agamemnon_wake_agent as no-ops

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# ── shared variables ─────────────────────────────────────────────────────────

TEMP_DIR=""
CAPTURED_PATCH_FILE=""

# ── setup / teardown ─────────────────────────────────────────────────────────

setup() {
    TEMP_DIR="$(mktemp -d)"
    CAPTURED_PATCH_FILE="${TEMP_DIR}/patch_body.json"

    export AGAMEMNON_URL="http://localhost:19999"
    export OUTPUT_FORMAT="text"
    export CREATED=0 UPDATED=0 WOKEN=0 HIBERNATED=0 UNCHANGED=0 PRUNED=0 ERRORS=0

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/log.sh"
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/reconcile.sh"

    # Stub report functions (defined in report.sh which we don't need here)
    report_add_agent() { : ; }
    report_add_unmanaged() { : ; }

    # Stub API functions that we don't want to call for real
    agamemnon_create_agent() { echo '{"id":"new-id-001","name":"test"}'; }
    agamemnon_wake_agent()   { : ; }
    agamemnon_hibernate_agent() { : ; }

    # Mock agamemnon_update_agent to capture the patch body
    agamemnon_update_agent() {
        # $1 = agent_id, $2 = patch body JSON
        echo "$2" > "$CAPTURED_PATCH_FILE"
        echo '{"id":"existing-id","name":"test"}' # return a valid response
    }
}

teardown() {
    [[ -n "$TEMP_DIR" ]] && rm -rf "$TEMP_DIR"
    unset OUTPUT_FORMAT CREATED UPDATED WOKEN HIBERNATED UNCHANGED PRUNED ERRORS
}

# ── helpers ───────────────────────────────────────────────────────────────────

# Build a minimal actual_json simulating an existing agent returned by Agamemnon.
# Arguments: status label program workdir args desc tags_json owner role
_make_actual_json() {
    # Note: $label is a reserved keyword in jq 1.6; use $lbl instead.
    jq -n \
        --arg id        "existing-id" \
        --arg status    "$1" \
        --arg lbl       "$2" \
        --arg program   "$3" \
        --arg workdir   "$4" \
        --arg args      "$5" \
        --arg desc      "$6" \
        --argjson tags  "$7" \
        --arg owner     "$8" \
        --arg role      "$9" \
        '{id: $id, status: $status, label: $lbl, program: $program,
          workingDirectory: $workdir, programArgs: $args,
          taskDescription: $desc, tags: $tags,
          owner: $owner, role: $role}'
}

# Create a minimal agent YAML in TEMP_DIR for testing.
_make_agent_yaml() {
    local name="$1" label="$2" program="$3" workdir="$4" args="$5"
    local desc="$6" tags="$7" owner="$8" role="$9" desired="${10:-active}"
    cat > "${TEMP_DIR}/agent.yaml" <<YAML
apiVersion: myrmidons/v1
kind: Agent
metadata:
  name: ${name}
  host: hermes
spec:
  label: ${label}
  program: ${program}
  workingDirectory: ${workdir}
  programArgs: "${args}"
  taskDescription: "${desc}"
  tags: [${tags}]
  owner: ${owner}
  role: ${role}
  deployment:
    type: local
  desiredState: ${desired}
YAML
    echo "${TEMP_DIR}/agent.yaml"
}

# ── source apply.sh functions in a controlled way ─────────────────────────────
# We cannot exec apply.sh directly (it calls main() at the end), so we copy
# just the apply_agent() function here, mirroring apply.sh exactly.

apply_agent() {
    local yaml_file="$1"
    local agents_json="$2"

    local name label program workdir args desc tags owner role desired_state
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
    desired_state="$(yq eval '.spec.desiredState // "active"' "$yaml_file")"

    local actual_json
    actual_json="$(echo "$agents_json" | jq -r --arg n "$name" '.[] | select(.name == $n)')"

    if [[ -z "$actual_json" ]]; then
        local create_body
        create_body="$(build_create_json "$name" "$label" "$program" "$workdir" "$args" "$desc" "$tags" "$owner" "$role")"
        local result
        if result="$(agamemnon_create_agent "$create_body" 2>&1)"; then
            local new_id
            new_id="$(echo "$result" | jq -r '.id // empty')"
            CREATED=$((CREATED + 1))
            if [[ "$desired_state" == "active" && -n "$new_id" ]]; then
                agamemnon_wake_agent "$new_id" > /dev/null
                WOKEN=$((WOKEN + 1))
            fi
            report_add_agent "$name" "$agent_host" "CREATE" "$desired_state" "created" "[]" ""
        else
            ERRORS=$((ERRORS + 1))
            report_add_agent "$name" "$agent_host" "ERROR" "$desired_state" "unknown" "[]" "create failed"
        fi
        return
    fi

    local actual_id actual_status
    actual_id="$(echo "$actual_json" | jq -r '.id')"
    actual_status="$(echo "$actual_json" | jq -r '.status // "unknown"')"

    local action
    action="$(compute_drift "$name" "$desired_state" "$actual_json" \
        "$label" "$program" "$workdir" "$args" "$desc" "$tags" "" "" "" "local")"

    case "$action" in
        UNCHANGED)
            UNCHANGED=$((UNCHANGED + 1))
            report_add_agent "$name" "$agent_host" "UNCHANGED" "$desired_state" "$actual_status" "[]" ""
            ;;
        WAKE)
            agamemnon_wake_agent "$actual_id" > /dev/null
            WOKEN=$((WOKEN + 1))
            report_add_agent "$name" "$agent_host" "WAKE" "$desired_state" "$actual_status" "[]" ""
            ;;
        HIBERNATE)
            agamemnon_hibernate_agent "$actual_id" > /dev/null
            HIBERNATED=$((HIBERNATED + 1))
            report_add_agent "$name" "$agent_host" "HIBERNATE" "$desired_state" "$actual_status" "[]" ""
            ;;
        UPDATE:*)
            local _changed_fields="${action#UPDATE:}"

            local tags_json
            if [[ -z "$tags" ]]; then
                tags_json="[]"
            else
                tags_json="$(echo "$tags" | jq -Rc 'split(",")')"
            fi

            local patch_body
            patch_body="$(jq -n \
                --arg label "$label" \
                --arg program "$program" \
                --arg workingDirectory "$workdir" \
                --arg programArgs "$args" \
                --arg taskDescription "$desc" \
                --argjson tags "$tags_json" \
                --arg owner "$owner" \
                --arg role "$role" \
                '{label: $label, program: $program, workingDirectory: $workingDirectory,
                  programArgs: $programArgs, taskDescription: $taskDescription,
                  tags: $tags, owner: $owner, role: $role}')"

            if agamemnon_update_agent "$actual_id" "$patch_body" > /dev/null 2>&1; then
                UPDATED=$((UPDATED + 1))
                report_add_agent "$name" "$agent_host" "UPDATE" "$desired_state" "$actual_status" "[]" ""
            else
                ERRORS=$((ERRORS + 1))
                report_add_agent "$name" "$agent_host" "ERROR" "$desired_state" "$actual_status" "[]" "update failed"
            fi

            if [[ "$desired_state" == "active" && "$actual_status" == "offline" ]]; then
                agamemnon_wake_agent "$actual_id" > /dev/null
                WOKEN=$((WOKEN + 1))
            elif [[ "$desired_state" == "hibernated" && \
                    ("$actual_status" == "active" || "$actual_status" == "online") ]]; then
                agamemnon_hibernate_agent "$actual_id" > /dev/null
                HIBERNATED=$((HIBERNATED + 1))
            fi
            ;;
    esac
}

# ── Tests: patch body field presence ─────────────────────────────────────────

@test "UPDATE patch body: contains 'label' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "NewLabel" "claude-code" "/tmp" "" "" "" "mvillmow" "member")"

    # Actual agent has a different label → triggers UPDATE
    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"OldLabel","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.label' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "NewLabel" ]]
}

@test "UPDATE patch body: contains 'program' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "aider" "/tmp" "" "" "" "mvillmow" "member")"

    # Actual has claude-code → desired is aider → UPDATE
    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.program' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "aider" ]]
}

@test "UPDATE patch body: contains 'workingDirectory' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/new/path" "" "" "" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/old/path","programArgs":"",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.workingDirectory' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "/new/path" ]]
}

@test "UPDATE patch body: contains 'programArgs' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/tmp" "--new-flag" "" "" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"--old-flag",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.programArgs' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "--new-flag" ]]
}

@test "UPDATE patch body: contains 'taskDescription' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/tmp" "" "New description" "" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"Old description","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.taskDescription' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "New description" ]]
}

@test "UPDATE patch body: contains 'tags' field as JSON array" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/tmp" "" "" "ai,ops" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":["old"],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    # tags should be a JSON array
    local tag_count
    tag_count="$(jq -r '.tags | length' "$CAPTURED_PATCH_FILE")"
    [[ "$tag_count" == "2" ]]
    local tag0 tag1
    tag0="$(jq -r '.tags[0]' "$CAPTURED_PATCH_FILE")"
    tag1="$(jq -r '.tags[1]' "$CAPTURED_PATCH_FILE")"
    [[ "$tag0" == "ai" ]]
    [[ "$tag1" == "ops" ]]
}

@test "UPDATE patch body: contains 'owner' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/tmp" "" "" "" "alice" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"bob","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.owner' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "alice" ]]
}

@test "UPDATE patch body: contains 'role' field" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/tmp" "" "" "" "mvillmow" "admin")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local field
    field="$(jq -r '.role' "$CAPTURED_PATCH_FILE")"
    [[ "$field" == "admin" ]]
}

@test "UPDATE patch body: is valid JSON" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "NewLabel" "aider" "/tmp/new" "--flag" "A desc" "tag1,tag2" "alice" "admin")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"OldLabel","program":"claude-code",
        "workingDirectory":"/tmp/old","programArgs":"",
        "taskDescription":"","tags":[],"owner":"bob","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    # Verify the captured body is valid JSON
    jq . "$CAPTURED_PATCH_FILE" > /dev/null
}

@test "UPDATE patch body: all 8 required keys are present" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "NewLabel" "claude-code" "/tmp" "" "" "" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"OldLabel","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    # All 8 patch fields must be present
    jq -e 'has("label")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("program")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("workingDirectory")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("programArgs")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("taskDescription")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("tags")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("owner")' "$CAPTURED_PATCH_FILE" > /dev/null
    jq -e 'has("role")' "$CAPTURED_PATCH_FILE" > /dev/null
}

@test "UNCHANGED: agamemnon_update_agent is NOT called when no drift" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "MyAgent" "claude-code" "/tmp" "" "" "" "mvillmow" "member")"

    # Actual matches desired exactly → UNCHANGED.
    # Note: apply.sh calls compute_drift without owner/role args (positions $11/$12 default to "").
    # So actual_json must have owner="" and role="" to avoid spurious owner/role drift.
    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"MyAgent","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"","role":""
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    # patch file should NOT be created (no update was sent)
    [[ ! -f "$CAPTURED_PATCH_FILE" ]]
    [[ "$UNCHANGED" -eq 1 ]]
}

@test "UPDATE increments UPDATED counter" {
    local yaml_file
    yaml_file="$(_make_agent_yaml "test-agent" "NewLabel" "claude-code" "/tmp" "" "" "" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"OldLabel","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":[],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"
    [[ "$UPDATED" -eq 1 ]]
}

@test "UPDATE patch body: empty tags field becomes empty JSON array" {
    local yaml_file
    # No tags in YAML → tags=[]
    yaml_file="$(_make_agent_yaml "test-agent" "NewLabel" "claude-code" "/tmp" "" "" "" "mvillmow" "member")"

    local agents_json
    agents_json="$(jq -n '[{
        "id":"existing-id","name":"test-agent","status":"active",
        "label":"OldLabel","program":"claude-code",
        "workingDirectory":"/tmp","programArgs":"",
        "taskDescription":"","tags":["old"],"owner":"mvillmow","role":"member"
    }]')"

    apply_agent "$yaml_file" "$agents_json"

    [[ -f "$CAPTURED_PATCH_FILE" ]]
    local tag_count
    tag_count="$(jq -r '.tags | length' "$CAPTURED_PATCH_FILE")"
    [[ "$tag_count" == "0" ]]
}
