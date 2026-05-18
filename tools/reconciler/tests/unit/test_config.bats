#!/usr/bin/env bats
# tests/unit/test_config.bats — unit tests for scripts/lib/config.sh

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

# Create a temporary directory for test config files
setup() {
    TEST_TMP_DIR="$(mktemp -d)"
    export TEST_TMP_DIR
    ORIGINAL_DIR="$(pwd)"
    export ORIGINAL_DIR

    # Create a justfile to mark repo root
    touch "${TEST_TMP_DIR}/justfile"

    # Change to the test directory so _cfg_repo_root finds our files
    cd "$TEST_TMP_DIR" || return 1

    # Clear any environment variables that might interfere
    unset AGAMEMNON_URL
    unset MYRM_LOG_LEVEL
    unset MYRM_PRUNE_POLICY
    unset MYRM_SNAPSHOT_RETENTION
    unset HOST

    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/scripts/lib/config.sh"
}

teardown() {
    cd "$ORIGINAL_DIR" || return 1
    rm -rf "$TEST_TMP_DIR"
}

# ---------------------------------------------------------------------------
# Defaults when no config files exist
# ---------------------------------------------------------------------------

@test "load_config: uses defaults when no config files exist" {
    load_config
    [[ "$MYRM_DEFAULT_HOST" == "hermes" ]]
    [[ "$MYRM_AIM_HOST" == "http://localhost:8080" ]]
    [[ "$MYRM_LOG_LEVEL" == "info" ]]
    [[ "$MYRM_PRUNE_POLICY" == "manual" ]]
    [[ "$MYRM_SNAPSHOT_RETENTION" == "7" ]]
}

# ---------------------------------------------------------------------------
# Project config overrides defaults
# ---------------------------------------------------------------------------

@test "load_config: project config overrides defaults" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
aimHost: http://agamemnon.example.com:9000
logLevel: debug
prunePolicy: auto
snapshotRetention: 14
EOF

    load_config
    [[ "$MYRM_DEFAULT_HOST" == "apollo" ]]
    [[ "$MYRM_AIM_HOST" == "http://agamemnon.example.com:9000" ]]
    [[ "$MYRM_LOG_LEVEL" == "debug" ]]
    [[ "$MYRM_PRUNE_POLICY" == "auto" ]]
    [[ "$MYRM_SNAPSHOT_RETENTION" == "14" ]]
}

@test "load_config: project config partial override (some fields)" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: zeus
logLevel: warn
EOF

    load_config
    [[ "$MYRM_DEFAULT_HOST" == "zeus" ]]
    [[ "$MYRM_LOG_LEVEL" == "warn" ]]
    # Check defaults for other fields
    [[ "$MYRM_AIM_HOST" == "http://localhost:8080" ]]
    [[ "$MYRM_PRUNE_POLICY" == "manual" ]]
}

# ---------------------------------------------------------------------------
# Local config overrides project config
# ---------------------------------------------------------------------------

@test "load_config: local config overrides project config" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
aimHost: http://project.example.com:8080
logLevel: debug
EOF

    cat > "${TEST_TMP_DIR}/.myrmidons.local.yaml" <<'EOF'
defaultHost: poseidon
aimHost: http://local.example.com:9999
EOF

    load_config
    # Local should win
    [[ "$MYRM_DEFAULT_HOST" == "poseidon" ]]
    [[ "$MYRM_AIM_HOST" == "http://local.example.com:9999" ]]
    # Project config for unset fields in local
    [[ "$MYRM_LOG_LEVEL" == "debug" ]]
}

@test "load_config: local config partial override (some fields)" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
logLevel: info
prunePolicy: auto
EOF

    cat > "${TEST_TMP_DIR}/.myrmidons.local.yaml" <<'EOF'
defaultHost: hades
EOF

    load_config
    # Local overrides host
    [[ "$MYRM_DEFAULT_HOST" == "hades" ]]
    # Project config for other fields
    [[ "$MYRM_LOG_LEVEL" == "info" ]]
    [[ "$MYRM_PRUNE_POLICY" == "auto" ]]
}

# ---------------------------------------------------------------------------
# Environment variables override file-based values
# ---------------------------------------------------------------------------

@test "load_config: env var AGAMEMNON_URL overrides all file configs" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
aimHost: http://project.example.com:8080
EOF

    export AGAMEMNON_URL="http://env.example.com:7000"
    load_config
    [[ "$MYRM_AIM_HOST" == "http://env.example.com:7000" ]]
}

@test "load_config: env var HOST overrides all file configs" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
EOF

    export HOST="heracles"
    load_config
    [[ "$MYRM_DEFAULT_HOST" == "heracles" ]]
}

@test "load_config: env var MYRM_LOG_LEVEL overrides all file configs" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
logLevel: info
EOF

    export MYRM_LOG_LEVEL="error"
    load_config
    [[ "$MYRM_LOG_LEVEL" == "error" ]]
}

@test "load_config: env var MYRM_PRUNE_POLICY overrides all file configs" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
prunePolicy: manual
EOF

    export MYRM_PRUNE_POLICY="auto"
    load_config
    [[ "$MYRM_PRUNE_POLICY" == "auto" ]]
}

@test "load_config: env var MYRM_SNAPSHOT_RETENTION overrides all file configs" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
snapshotRetention: 7
EOF

    export MYRM_SNAPSHOT_RETENTION="30"
    load_config
    [[ "$MYRM_SNAPSHOT_RETENTION" == "30" ]]
}

# ---------------------------------------------------------------------------
# Full precedence chain
# ---------------------------------------------------------------------------

@test "load_config: full precedence chain (defaults < project < local < env)" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
aimHost: http://project.example.com:8080
logLevel: debug
prunePolicy: auto
snapshotRetention: 14
EOF

    cat > "${TEST_TMP_DIR}/.myrmidons.local.yaml" <<'EOF'
defaultHost: poseidon
logLevel: warn
EOF

    export MYRM_SNAPSHOT_RETENTION="30"

    load_config

    # Host: local config wins
    [[ "$MYRM_DEFAULT_HOST" == "poseidon" ]]
    # aimHost: project config (no local override)
    [[ "$MYRM_AIM_HOST" == "http://project.example.com:8080" ]]
    # logLevel: local config wins
    [[ "$MYRM_LOG_LEVEL" == "warn" ]]
    # prunePolicy: project config (no local or env)
    [[ "$MYRM_PRUNE_POLICY" == "auto" ]]
    # snapshotRetention: env var wins
    [[ "$MYRM_SNAPSHOT_RETENTION" == "30" ]]
}

# ---------------------------------------------------------------------------
# show_config output format
# ---------------------------------------------------------------------------

@test "show_config: produces output with header and field rows" {
    load_config
    output="$(show_config)"

    # Check for header
    [[ "$output" == *"Effective Myrmidons configuration"* ]]
    [[ "$output" == *"Sources (lowest → highest precedence):"* ]]
    [[ "$output" == *"[defaults]"* ]]
    [[ "$output" == *"[env]"* ]]
}

@test "show_config: lists all fields with values and sources" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
logLevel: debug
EOF

    export MYRM_PRUNE_POLICY="custom"

    output="$(show_config)"

    # Check that all fields appear
    [[ "$output" == *"defaultHost"* ]]
    [[ "$output" == *"aimHost"* ]]
    [[ "$output" == *"logLevel"* ]]
    [[ "$output" == *"prunePolicy"* ]]
    [[ "$output" == *"snapshotRetention"* ]]

    # Check that values appear
    [[ "$output" == *"apollo"* ]]
    [[ "$output" == *"debug"* ]]
    [[ "$output" == *"custom"* ]]
}

@test "show_config: identifies correct source for each field" {
    cat > "${TEST_TMP_DIR}/.myrmidons.yaml" <<'EOF'
defaultHost: apollo
logLevel: debug
EOF

    export AGAMEMNON_URL="http://env.example.com:9000"

    output="$(show_config)"

    # defaultHost should show source=project
    [[ "$output" == *"defaultHost"*"apollo"*"project"* ]]
    # aimHost should show source=env (from AGAMEMNON_URL)
    [[ "$output" == *"aimHost"*"http://env.example.com:9000"*"env"* ]]
    # logLevel should show source=project
    [[ "$output" == *"logLevel"*"debug"*"project"* ]]
}

@test "show_config: indicates missing config files" {
    output="$(show_config)"

    # Should indicate project config not found
    [[ "$output" == *".myrmidons.yaml"* ]]
    [[ "$output" == *"not found"* ]]
}
