#!/usr/bin/env bash
# scripts/new-agent.sh — Scaffold a new agent YAML file from a template
#
# Prompts for required fields (interactive mode) or accepts them as flags
# (--non-interactive), validates input, and writes the agent YAML to
# agents/<host>/<name>.yaml.
#
# Usage (interactive):
#   bash scripts/new-agent.sh
#
# Usage (non-interactive):
#   bash scripts/new-agent.sh \
#     --name my-agent \
#     --host hermes \
#     --program claude-code \
#     --working-directory /home/mvillmow/MyProject \
#     --task-description "What this agent does" \
#     [--label "Display Name"] \
#     [--desired-state active] \
#     [--tags "tag1,tag2"]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TEMPLATES_DIR="${REPO_ROOT}/agents/_templates"

# shellcheck source=scripts/lib/prompt.sh
source "${SCRIPT_DIR}/lib/prompt.sh"

# ---------------------------------------------------------------------------
# Colour helpers (same convention as other scripts)
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

info()    { echo -e "${CYAN}${*}${RESET}"; }
success() { echo -e "${GREEN}${*}${RESET}"; }
warn()    { echo -e "${YELLOW}${*}${RESET}"; }
error()   { echo -e "${RED}ERROR: ${*}${RESET}" >&2; }
die()     { error "${*}"; exit 1; }

# ---------------------------------------------------------------------------
# Valid enum values
# ---------------------------------------------------------------------------
VALID_PROGRAMS=("claude-code" "aider" "none")
VALID_DESIRED_STATES=("active" "hibernated")

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
NON_INTERACTIVE=false
OPT_NAME=""
OPT_HOST=""
OPT_PROGRAM=""
OPT_WORKING_DIR=""
OPT_TASK_DESC=""
OPT_LABEL=""
OPT_DESIRED_STATE=""
OPT_TAGS=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Scaffold a new agent YAML file from a template.

Options:
  --name NAME                  Agent name (lowercase, hyphens only)
  --host HOST                  Target host (must match a directory in agents/)
  --program PROGRAM            Program type: claude-code | aider | none
  --working-directory DIR      Working directory for the agent
  --task-description DESC      What this agent does
  --label LABEL                Display name in Agamemnon UI (default: NAME)
  --desired-state STATE        active | hibernated (default: active)
  --tags TAG1,TAG2,...         Comma-separated list of tags (optional)
  --non-interactive            Require all values via flags; do not prompt
  -h, --help                   Show this help

Without --non-interactive, any missing fields will be prompted interactively.

Examples:
  # Interactive
  bash scripts/new-agent.sh

  # Non-interactive
  bash scripts/new-agent.sh \\
    --name ci-agent \\
    --host hermes \\
    --program claude-code \\
    --working-directory /home/mvillmow/CIProject \\
    --task-description "Continuous integration helper"
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --name)               OPT_NAME="$2";          shift 2 ;;
        --host)               OPT_HOST="$2";          shift 2 ;;
        --program)            OPT_PROGRAM="$2";       shift 2 ;;
        --working-directory)  OPT_WORKING_DIR="$2";   shift 2 ;;
        --task-description)   OPT_TASK_DESC="$2";     shift 2 ;;
        --label)              OPT_LABEL="$2";         shift 2 ;;
        --desired-state)      OPT_DESIRED_STATE="$2"; shift 2 ;;
        --tags)               OPT_TAGS="$2";          shift 2 ;;
        --non-interactive)    NON_INTERACTIVE=true;   shift   ;;
        -h|--help)            usage; exit 0 ;;
        --) shift; break ;;
        *) die "Unknown option: $1. Use --help for usage." ;;
    esac
done

# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
validate_name() {
    local name="$1"
    if [[ -z "$name" ]]; then
        return 1
    fi
    if [[ ! "$name" =~ ^[a-z0-9][a-z0-9-]*$ ]]; then
        return 1
    fi
    return 0
}

validate_program() {
    local prog="$1"
    for valid in "${VALID_PROGRAMS[@]}"; do
        [[ "$prog" == "$valid" ]] && return 0
    done
    return 1
}

validate_desired_state() {
    local state="$1"
    for valid in "${VALID_DESIRED_STATES[@]}"; do
        [[ "$state" == "$valid" ]] && return 0
    done
    return 1
}

# ---------------------------------------------------------------------------
# Template selection
# ---------------------------------------------------------------------------
select_template() {
    local program="$1"
    case "$program" in
        claude-code) echo "${TEMPLATES_DIR}/claude-default.yaml" ;;
        aider)       echo "${TEMPLATES_DIR}/aider-default.yaml"  ;;
        none)        echo "${TEMPLATES_DIR}/worker-default.yaml" ;;
        *)           die "No template for program: ${program}" ;;
    esac
}

# ---------------------------------------------------------------------------
# Interactive prompts
# ---------------------------------------------------------------------------

# prompt_field and prompt_enum collect free-text / enum data from the user.
# They use bare `read -r -p` (no timeout) intentionally: a timeout would
# silently corrupt required field values with an empty default. These are
# data-entry reads, not yes/no confirmations. Use confirm_with_timeout() for
# any confirmation prompt that should default-deny after a timeout.
prompt_field() {
    local prompt="$1"
    local default="$2"
    local result=""
    if [[ -n "$default" ]]; then
        # data-entry read — no timeout intentional (free-text input, not y/n)
        read -r -p "$(echo -e "${BOLD}${prompt}${RESET} [${default}]: ")" result
        echo "${result:-$default}"
    else
        while [[ -z "$result" ]]; do
            # data-entry read — no timeout intentional (required field)
            read -r -p "$(echo -e "${BOLD}${prompt}${RESET}: ")" result
            if [[ -z "$result" ]]; then
                warn "  This field is required."
            fi
        done
        echo "$result"
    fi
}

prompt_enum() {
    local prompt="$1"
    local default="$2"
    shift 2
    local options=("$@")
    local joined
    joined="$(IFS="|"; echo "${options[*]}")"
    local result=""
    while true; do
        if [[ -n "$default" ]]; then
            # data-entry read — no timeout intentional (enum selection)
            read -r -p "$(echo -e "${BOLD}${prompt}${RESET} (${joined}) [${default}]: ")" result
            result="${result:-$default}"
        else
            # data-entry read — no timeout intentional (required enum)
            read -r -p "$(echo -e "${BOLD}${prompt}${RESET} (${joined}): ")" result
        fi
        for valid in "${options[@]}"; do
            [[ "$result" == "$valid" ]] && { echo "$result"; return 0; }
        done
        warn "  Must be one of: ${joined}"
    done
}

# ---------------------------------------------------------------------------
# Tags formatting: "tag1,tag2" → YAML list items
# ---------------------------------------------------------------------------
format_tags_yaml() {
    local tags_input="$1"
    if [[ -z "$tags_input" ]]; then
        echo "[]"
        return
    fi
    local yaml="["
    local first=true
    IFS=',' read -ra tag_arr <<< "$tags_input"
    for tag in "${tag_arr[@]}"; do
        tag="$(echo "$tag" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
        [[ -z "$tag" ]] && continue
        if $first; then
            yaml+="${tag}"
            first=false
        else
            yaml+=", ${tag}"
        fi
    done
    yaml+="]"
    echo "$yaml"
}

# ---------------------------------------------------------------------------
# Collect field values
# ---------------------------------------------------------------------------
collect_fields() {
    echo ""
    info "=== New Agent Scaffolding ==="
    echo ""

    # Name
    local name="${OPT_NAME}"
    while true; do
        if [[ -z "$name" ]]; then
            name="$(prompt_field "Agent name (lowercase, hyphens only, e.g. my-agent)" "")"
        fi
        if validate_name "$name"; then
            break
        fi
        error "Name must match ^[a-z0-9][a-z0-9-]*\$ (lowercase letters, digits, hyphens only)"
        name=""
    done

    # Host
    local host="${OPT_HOST}"
    while true; do
        if [[ -z "$host" ]]; then
            host="$(prompt_field "Target host" "hermes")"
        fi
        local host_dir="${REPO_ROOT}/agents/${host}"
        if [[ -d "$host_dir" ]]; then
            break
        fi
        # Offer to create the directory
        warn "Directory agents/${host}/ does not exist."
        if confirm_with_timeout "Create agents/${host}/? [y/N]"; then
            mkdir -p "$host_dir"
            info "Created agents/${host}/"
            break
        else
            warn "Host directory must exist. Choose an existing host or create the directory."
            host=""
            OPT_HOST=""  # allow re-prompt
        fi
    done

    # Program
    local program="${OPT_PROGRAM}"
    while true; do
        if [[ -z "$program" ]]; then
            program="$(prompt_enum "Program type" "claude-code" "${VALID_PROGRAMS[@]}")"
        fi
        if validate_program "$program"; then
            break
        fi
        error "Program must be one of: $(IFS=", "; echo "${VALID_PROGRAMS[*]}")"
        program=""
        OPT_PROGRAM=""
    done

    # Working directory
    local working_dir="${OPT_WORKING_DIR}"
    if [[ -z "$working_dir" ]]; then
        working_dir="$(prompt_field "Working directory" "/home/mvillmow/${name}")"
    fi
    if [[ -z "$working_dir" ]]; then
        die "Working directory is required."
    fi

    # Task description
    local task_desc="${OPT_TASK_DESC}"
    if [[ -z "$task_desc" ]]; then
        task_desc="$(prompt_field "Task description" "Describe what this agent does")"
    fi

    # Label (display name)
    local label="${OPT_LABEL}"
    if [[ -z "$label" ]]; then
        if $NON_INTERACTIVE; then
            label="$name"
        else
            label="$(prompt_field "Display label (Agamemnon UI)" "$name")"
        fi
    fi

    # Desired state
    local desired_state="${OPT_DESIRED_STATE}"
    while true; do
        if [[ -z "$desired_state" ]]; then
            desired_state="$(prompt_enum "Desired state" "active" "${VALID_DESIRED_STATES[@]}")"
        fi
        if validate_desired_state "$desired_state"; then
            break
        fi
        error "Desired state must be one of: $(IFS=", "; echo "${VALID_DESIRED_STATES[*]}")"
        desired_state=""
        OPT_DESIRED_STATE=""
    done

    # Tags
    local tags_input="${OPT_TAGS}"
    if [[ -z "$tags_input" ]] && ! $NON_INTERACTIVE; then
        # data-entry read — no timeout intentional (optional free-text field)
        read -r -p "$(echo -e "${BOLD}Tags${RESET} (comma-separated, optional): ")" tags_input
    fi

    # Export collected values
    FIELD_NAME="$name"
    FIELD_HOST="$host"
    FIELD_PROGRAM="$program"
    FIELD_WORKING_DIR="$working_dir"
    FIELD_TASK_DESC="$task_desc"
    FIELD_LABEL="$label"
    FIELD_DESIRED_STATE="$desired_state"
    FIELD_TAGS="$tags_input"
}

# ---------------------------------------------------------------------------
# Non-interactive validation: all required fields must be provided via flags
# ---------------------------------------------------------------------------
validate_non_interactive() {
    local missing=()
    [[ -z "$OPT_NAME" ]]        && missing+=("--name")
    [[ -z "$OPT_HOST" ]]        && missing+=("--host")
    [[ -z "$OPT_PROGRAM" ]]     && missing+=("--program")
    [[ -z "$OPT_WORKING_DIR" ]] && missing+=("--working-directory")
    [[ -z "$OPT_TASK_DESC" ]]   && missing+=("--task-description")
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "--non-interactive requires all fields. Missing: ${missing[*]}"
    fi

    if ! validate_name "$OPT_NAME"; then
        die "Invalid --name '${OPT_NAME}': must match ^[a-z0-9][a-z0-9-]*\$"
    fi
    if ! validate_program "$OPT_PROGRAM"; then
        die "Invalid --program '${OPT_PROGRAM}': must be one of $(IFS=", "; echo "${VALID_PROGRAMS[*]}")"
    fi
    if [[ -n "$OPT_DESIRED_STATE" ]] && ! validate_desired_state "$OPT_DESIRED_STATE"; then
        die "Invalid --desired-state '${OPT_DESIRED_STATE}': must be one of $(IFS=", "; echo "${VALID_DESIRED_STATES[*]}")"
    fi
}

# ---------------------------------------------------------------------------
# Generate the YAML file
# ---------------------------------------------------------------------------
generate_agent_yaml() {
    local template
    template="$(select_template "${FIELD_PROGRAM}")"

    local output_file="${REPO_ROOT}/agents/${FIELD_HOST}/${FIELD_NAME}.yaml"

    if [[ -f "$output_file" ]]; then
        die "File already exists: agents/${FIELD_HOST}/${FIELD_NAME}.yaml\nDelete it first or choose a different name."
    fi

    local tags_yaml
    tags_yaml="$(format_tags_yaml "${FIELD_TAGS}")"

    # Escape special characters for sed replacement strings
    escape_sed() { printf '%s\n' "$1" | sed 's/[\/&]/\\&/g'; }

    local esc_name esc_host esc_label esc_working_dir esc_task_desc esc_desired_state esc_tags
    esc_name="$(escape_sed "${FIELD_NAME}")"
    esc_host="$(escape_sed "${FIELD_HOST}")"
    esc_label="$(escape_sed "${FIELD_LABEL}")"
    esc_working_dir="$(escape_sed "${FIELD_WORKING_DIR}")"
    esc_desired_state="$(escape_sed "${FIELD_DESIRED_STATE}")"
    esc_tags="$(escape_sed "${tags_yaml}")"
    # Task description needs special handling for the sed expression
    esc_task_desc="$(printf '%s\n' "${FIELD_TASK_DESC}" | sed 's/[\/&"]/\\&/g')"

    # Apply field substitutions. The tags: key may span multiple lines in
    # the worker template (list-style), so we use awk to collapse those
    # trailing "    - item" lines after replacing the tags: line itself.
    sed \
        -e "s/^\(  name:\) CHANGE_ME\(.*\)$/  name: ${esc_name}/" \
        -e "s/^\(  host:\) hermes\(.*\)$/  host: ${esc_host}/" \
        -e "s/^\(  label:\) CHANGE_ME\(.*\)$/  label: ${esc_label}/" \
        -e "s|^\(  workingDirectory:\).*$|  workingDirectory: ${esc_working_dir}|" \
        -e "s/^\(  taskDescription:\).*$/  taskDescription: \"${esc_task_desc}\"/" \
        -e "s/^\(  desiredState:\).*$/  desiredState: ${esc_desired_state}/" \
        -e "s/^\(  tags:\).*$/  tags: ${esc_tags}/" \
        "$template" \
    | awk '
        # After we rewrite "  tags: [...]", suppress any immediately
        # following "    - item" lines from the original multi-line block.
        /^  tags:/ { in_tags=1; print; next }
        in_tags && /^    - / { next }   # skip old list items
        { in_tags=0; print }
    ' > "$output_file"

    echo "$output_file"
}

# ---------------------------------------------------------------------------
# Schema validation on the generated file
# ---------------------------------------------------------------------------
validate_generated_file() {
    local output_file="$1"

    info "\nValidating generated file..."

    local errors=0

    # YAML syntax
    if ! yq eval '.' "$output_file" > /dev/null 2>&1; then
        error "Generated file has invalid YAML syntax."
        errors=$((errors + 1))
    fi

    # apiVersion
    local api_version
    api_version="$(yq eval '.apiVersion // ""' "$output_file")"
    if [[ "$api_version" != "myrmidons/v1" ]]; then
        error "apiVersion: expected myrmidons/v1, got '${api_version}'"
        errors=$((errors + 1))
    fi

    # kind
    local kind
    kind="$(yq eval '.kind // ""' "$output_file")"
    if [[ "$kind" != "Agent" ]]; then
        error "kind: expected Agent, got '${kind}'"
        errors=$((errors + 1))
    fi

    # Required fields
    local name host program workdir
    name="$(yq eval '.metadata.name // ""' "$output_file")"
    host="$(yq eval '.metadata.host // ""' "$output_file")"
    program="$(yq eval '.spec.program // ""' "$output_file")"
    workdir="$(yq eval '.spec.workingDirectory // ""' "$output_file")"

    [[ -z "$name" ]]    && { error "metadata.name is missing"; errors=$((errors + 1)); }
    [[ -z "$host" ]]    && { error "metadata.host is missing"; errors=$((errors + 1)); }
    [[ -z "$program" ]] && { error "spec.program is missing";  errors=$((errors + 1)); }
    [[ -z "$workdir" ]] && { error "spec.workingDirectory is missing"; errors=$((errors + 1)); }

    # No leftover placeholders
    if grep -q "CHANGE_ME" "$output_file" 2>/dev/null; then
        error "Generated file still contains CHANGE_ME placeholders."
        errors=$((errors + 1))
    fi

    # desiredState enum
    local ds
    ds="$(yq eval '.spec.desiredState // ""' "$output_file")"
    if [[ -n "$ds" ]] && ! validate_desired_state "$ds"; then
        error "spec.desiredState: must be active or hibernated, got '${ds}'"
        errors=$((errors + 1))
    fi

    if [[ $errors -gt 0 ]]; then
        rm -f "$output_file"
        die "Validation failed (${errors} error(s)). Generated file removed."
    fi

    success "  Validation passed."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    # Verify yq is available
    if ! command -v yq &>/dev/null; then
        die "yq not found. Install from https://github.com/mikefarah/yq"
    fi

    if $NON_INTERACTIVE; then
        validate_non_interactive
        # Set defaults for optional fields
        FIELD_NAME="$OPT_NAME"
        FIELD_HOST="$OPT_HOST"
        FIELD_PROGRAM="$OPT_PROGRAM"
        FIELD_WORKING_DIR="$OPT_WORKING_DIR"
        FIELD_TASK_DESC="$OPT_TASK_DESC"
        FIELD_LABEL="${OPT_LABEL:-$OPT_NAME}"
        FIELD_DESIRED_STATE="${OPT_DESIRED_STATE:-active}"
        FIELD_TAGS="${OPT_TAGS:-}"

        # Ensure host directory exists
        local host_dir="${REPO_ROOT}/agents/${FIELD_HOST}"
        if [[ ! -d "$host_dir" ]]; then
            die "Host directory agents/${FIELD_HOST}/ does not exist. Create it first."
        fi
    else
        collect_fields
    fi

    echo ""
    info "Generating agent YAML..."
    local output_file
    output_file="$(generate_agent_yaml)"

    validate_generated_file "$output_file"

    local rel_path
    rel_path="agents/${FIELD_HOST}/${FIELD_NAME}.yaml"

    echo ""
    success "Agent created: ${rel_path}"
    echo ""
    echo -e "${BOLD}Next steps:${RESET}"
    echo "  just plan ${FIELD_HOST}          # Preview what will change"
    echo "  just apply ${FIELD_HOST}         # Apply to Agamemnon"
    echo "  git add ${rel_path}"
    echo "  git commit -m 'add agent ${FIELD_NAME}'"
}

main
