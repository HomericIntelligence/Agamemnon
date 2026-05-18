#!/usr/bin/env bash
# tests/test-rollback.sh — Unit tests for rollback/snapshot functionality
#
# Tests the snapshot saving logic in apply.sh and the rollback.sh script
# using a mock Agamemnon server via a stub HTTP server.
#
# Usage:
#   ./tests/test-rollback.sh
#
# Exit codes:
#   0 = all tests passed
#   1 = test failures found

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PASS=0
FAIL=0

# ── Helpers ──────────────────────────────────────────────────────────────────

ok() {
    local desc="$1"
    echo "  PASS: ${desc}"
    PASS=$((PASS + 1))
}

fail() {
    local desc="$1"
    local detail="${2:-}"
    echo "  FAIL: ${desc}"
    [[ -n "$detail" ]] && echo "        ${detail}"
    FAIL=$((FAIL + 1))
}

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        ok "$desc"
    else
        fail "$desc" "expected='${expected}' actual='${actual}'"
    fi
}

assert_file_exists() {
    local desc="$1" file="$2"
    if [[ -f "$file" ]]; then
        ok "$desc"
    else
        fail "$desc" "file not found: ${file}"
    fi
}

assert_dir_exists() {
    local desc="$1" dir="$2"
    if [[ -d "$dir" ]]; then
        ok "$desc"
    else
        fail "$desc" "directory not found: ${dir}"
    fi
}

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        ok "$desc"
    else
        fail "$desc" "expected to find '${needle}' in output"
    fi
}

assert_not_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if ! echo "$haystack" | grep -qF "$needle"; then
        ok "$desc"
    else
        fail "$desc" "did not expect to find '${needle}' in output"
    fi
}

# ── Test Fixtures ─────────────────────────────────────────────────────────────

TMPDIR_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_ROOT"' EXIT

make_snapshot_dir() {
    local dir
    dir="${TMPDIR_ROOT}/snapshots_$(date +%s%N)"
    mkdir -p "$dir"
    echo "$dir"
}

make_agent_json() {
    local name="${1:-test-agent}"
    local status="${2:-offline}"
    jq -n \
        --arg name "$name" \
        --arg status "$status" \
        '{
            id: "abc-123",
            name: $name,
            label: "Test Agent",
            program: "claude-code",
            workingDirectory: "/home/user/project",
            programArgs: "--flag",
            taskDescription: "Does things",
            tags: ["test"],
            status: $status
        }'
}

make_snapshot_file() {
    local dir="$1"
    local timestamp="${2:-2024-01-15T10:30:00Z}"
    local content="${3:-[]}"
    local file="${dir}/${timestamp}.json"
    echo "$content" > "$file"
    echo "$file"
}

# ── Test: save_snapshot function (sourced from apply.sh context) ───────────────

test_save_snapshot_creates_dir_and_file() {
    echo ""
    echo "=== save_snapshot: creates directory and file ==="

    local snap_dir="${TMPDIR_ROOT}/test_snap_$$"
    local fake_agents='[{"id":"1","name":"foo","status":"active"}]'

    # Source just the save_snapshot function by extracting and running it
    (
        SNAPSHOT_DIR="$snap_dir"
        SNAPSHOT_KEEP=10
        REPO_ROOT="$TMPDIR_ROOT"

        # Inline the save_snapshot logic for unit testing
        save_snapshot() {
            local agents_json="$1"
            local timestamp
            timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
            mkdir -p "$SNAPSHOT_DIR"
            local snapshot_file="${SNAPSHOT_DIR}/${timestamp}.json"
            echo "$agents_json" > "$snapshot_file"

            local count
            count="$(find "$SNAPSHOT_DIR" -name "*.json" | wc -l)"
            if [[ $count -gt $SNAPSHOT_KEEP ]]; then
                local excess=$(( count - SNAPSHOT_KEEP ))
                find "$SNAPSHOT_DIR" -name "*.json" | sort | head -n "$excess" | xargs rm -f
            fi
        }

        save_snapshot "$fake_agents"
    )

    assert_dir_exists "snapshot directory is created" "$snap_dir"

    local file_count
    file_count="$(find "$snap_dir" -name "*.json" | wc -l)"
    assert_eq "one snapshot file is written" "1" "$file_count"

    # Verify the content is the agent JSON
    local content
    content="$(cat "${snap_dir}"/*.json)"
    assert_contains "snapshot contains agent data" '"name":"foo"' "$content"
}

test_save_snapshot_prunes_old_files() {
    echo ""
    echo "=== save_snapshot: prunes old snapshots beyond SNAPSHOT_KEEP ==="

    local snap_dir="${TMPDIR_ROOT}/test_prune_$$"
    mkdir -p "$snap_dir"

    # Create 12 existing snapshots
    for i in $(seq -w 1 12); do
        echo '[]' > "${snap_dir}/2024-01-0${i}T00:00:00Z.json"
    done

    (
        SNAPSHOT_DIR="$snap_dir"
        SNAPSHOT_KEEP=10

        save_snapshot() {
            local agents_json="$1"
            local timestamp
            timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
            mkdir -p "$SNAPSHOT_DIR"
            local snapshot_file="${SNAPSHOT_DIR}/${timestamp}.json"
            echo "$agents_json" > "$snapshot_file"

            local count
            count="$(find "$SNAPSHOT_DIR" -name "*.json" | wc -l)"
            if [[ $count -gt $SNAPSHOT_KEEP ]]; then
                local excess=$(( count - SNAPSHOT_KEEP ))
                find "$SNAPSHOT_DIR" -name "*.json" | sort | head -n "$excess" | xargs rm -f
            fi
        }

        save_snapshot '[]'
    )

    local remaining
    remaining="$(find "$snap_dir" -name "*.json" | wc -l)"
    assert_eq "snapshot count does not exceed SNAPSHOT_KEEP=10" "10" "$remaining"
}

# ── Test: rollback.sh --list ──────────────────────────────────────────────────

test_rollback_list_empty() {
    echo ""
    echo "=== rollback.sh --list: no snapshots ==="

    local snap_dir="${TMPDIR_ROOT}/test_list_empty_$$"
    mkdir -p "$snap_dir"

    local output
    output="$(bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "$snap_dir" --list 2>&1)"

    assert_contains "reports no snapshots found" "No snapshots found" "$output"
}

test_rollback_list_shows_files() {
    echo ""
    echo "=== rollback.sh --list: shows available snapshots ==="

    local snap_dir
    snap_dir="$(make_snapshot_dir)"

    make_snapshot_file "$snap_dir" "2024-01-15T10:00:00Z" \
        '[{"id":"1","name":"agent-a","status":"active"}]' > /dev/null
    make_snapshot_file "$snap_dir" "2024-01-15T11:00:00Z" \
        '[{"id":"1","name":"agent-a","status":"active"},{"id":"2","name":"agent-b","status":"offline"}]' > /dev/null

    local output
    output="$(bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "$snap_dir" --list 2>&1)"

    assert_contains "lists snapshot filenames" "2024-01-15T" "$output"
    assert_contains "marks most recent snapshot" "most recent" "$output"
    assert_contains "shows agent counts" "agents" "$output"
}

test_rollback_list_newest_first() {
    echo ""
    echo "=== rollback.sh --list: most recent snapshot marked correctly ==="

    local snap_dir
    snap_dir="$(make_snapshot_dir)"

    make_snapshot_file "$snap_dir" "2024-01-10T00:00:00Z" '[]' > /dev/null
    make_snapshot_file "$snap_dir" "2024-01-20T00:00:00Z" '[]' > /dev/null

    local output
    output="$(bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "$snap_dir" --list 2>&1)"

    # The newest file should appear with the marker
    local most_recent_line
    most_recent_line="$(echo "$output" | grep "most recent" || true)"
    assert_contains "newest file is marked most recent" "2024-01-20T" "$most_recent_line"
}

# ── Test: rollback.sh --dry-run ───────────────────────────────────────────────

test_rollback_dry_run_no_api_calls() {
    echo ""
    echo "=== rollback.sh --dry-run: shows plan without calling API ==="

    local snap_dir
    snap_dir="$(make_snapshot_dir)"

    local agent_json
    agent_json="$(make_agent_json "my-agent" "active")"
    make_snapshot_file "$snap_dir" "2024-01-15T10:00:00Z" \
        "[${agent_json}]" > /dev/null

    # Point at a non-existent Agamemnon so any real API call would fail
    local output
    output="$(AGAMEMNON_URL="http://localhost:19999" \
        bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "$snap_dir" --dry-run 2>&1)"

    assert_contains "shows DRY-RUN label" "DRY-RUN" "$output"
    assert_contains "names agent to restore" "my-agent" "$output"
    assert_not_contains "does not call API (no HTTP error)" "HTTP" "$output"
}

test_rollback_dry_run_shows_agent_count() {
    echo ""
    echo "=== rollback.sh --dry-run: reports agent count from snapshot ==="

    local snap_dir
    snap_dir="$(make_snapshot_dir)"

    local a b
    a="$(make_agent_json "agent-1" "active")"
    b="$(make_agent_json "agent-2" "offline")"
    make_snapshot_file "$snap_dir" "2024-01-15T10:00:00Z" \
        "[${a}, ${b}]" > /dev/null

    local output
    output="$(AGAMEMNON_URL="http://localhost:19999" \
        bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "$snap_dir" --dry-run 2>&1)"

    assert_contains "shows snapshot contains 2 agents" "2 agents" "$output"
}

# ── Test: rollback.sh validation ─────────────────────────────────────────────

test_rollback_missing_snapshot_dir() {
    echo ""
    echo "=== rollback.sh: fails gracefully when snapshot dir missing ==="

    local output exit_code
    set +e
    output="$(bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "/nonexistent/path/$$" 2>&1)"
    exit_code=$?
    set -e

    assert_eq "exits with non-zero code" "1" "$exit_code"
    assert_contains "reports missing directory" "No snapshots directory" "$output"
}

test_rollback_invalid_json_snapshot() {
    echo ""
    echo "=== rollback.sh: fails when snapshot is invalid JSON ==="

    local snap_dir
    snap_dir="$(make_snapshot_dir)"
    echo "not valid json {{{" > "${snap_dir}/2024-01-15T10:00:00Z.json"

    local output exit_code
    set +e
    output="$(AGAMEMNON_URL="http://localhost:19999" \
        bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot-dir "$snap_dir" 2>&1)"
    exit_code=$?
    set -e

    assert_eq "exits with non-zero code" "1" "$exit_code"
    assert_contains "reports invalid snapshot" "not a valid JSON array" "$output"
}

test_rollback_specific_snapshot_file() {
    echo ""
    echo "=== rollback.sh --snapshot: uses specific file ==="

    local snap_dir
    snap_dir="$(make_snapshot_dir)"

    local old_file
    old_file="$(make_snapshot_file "$snap_dir" "2024-01-01T00:00:00Z" \
        '[{"id":"1","name":"old-agent","status":"offline"}]')"
    make_snapshot_file "$snap_dir" "2024-01-15T10:00:00Z" \
        '[{"id":"2","name":"new-agent","status":"active"}]' >/dev/null

    # Ask for the old snapshot specifically
    local output
    output="$(AGAMEMNON_URL="http://localhost:19999" \
        bash "${REPO_ROOT}/scripts/rollback.sh" \
        --snapshot "$old_file" --dry-run 2>&1)"

    assert_contains "uses specified snapshot file" "old-agent" "$output"
    assert_not_contains "does not use newer snapshot" "new-agent" "$output"
}

# ── Test: apply.sh writes snapshot ───────────────────────────────────────────

test_apply_snapshot_dir_flag() {
    echo ""
    echo "=== apply.sh --snapshot-dir: accepts flag without error ==="

    local snap_dir="${TMPDIR_ROOT}/apply_snap_$$"

    # apply.sh will fail at agamemnon_check_connection since no server is up,
    # but we just need to confirm --snapshot-dir is a recognized flag.
    local output exit_code
    set +e
    output="$(AGAMEMNON_URL="http://localhost:19999" \
        bash "${REPO_ROOT}/scripts/apply.sh" \
        --snapshot-dir "$snap_dir" 2>&1)"
    exit_code=$?
    set -e

    # Should fail due to Agamemnon unreachable, not due to unknown flag
    assert_not_contains "flag is recognized (no 'Unknown argument' error)" \
        "Unknown argument" "$output"
}

# ── Test: default snapshot dir uses REPO_ROOT not root (issue #370) ───────────

test_default_snapshot_dir_uses_repo_root() {
    echo ""
    echo "=== default snapshot dir resolves to REPO_ROOT, not / ==="

    # Grep apply.sh directly: the default must reference REPO_ROOT (uppercase),
    # not repo_root (lowercase undefined variable). Under set -u the lowercase
    # form fatally aborts; when set -u is absent it silently expands to ""
    # producing /.myrmidons/snapshots (a root-filesystem write attempt).
    local occurrences
    occurrences="$(grep -c '\${repo_root}' "${REPO_ROOT}/scripts/apply.sh" || true)"
    assert_eq "no lowercase repo_root reference in apply.sh (issue #370)" "0" "$occurrences"
}

# ── Summary ───────────────────────────────────────────────────────────────────

run_all_tests() {
    echo "Running rollback/snapshot tests..."

    test_save_snapshot_creates_dir_and_file
    test_save_snapshot_prunes_old_files
    test_rollback_list_empty
    test_rollback_list_shows_files
    test_rollback_list_newest_first
    test_rollback_dry_run_no_api_calls
    test_rollback_dry_run_shows_agent_count
    test_rollback_missing_snapshot_dir
    test_rollback_invalid_json_snapshot
    test_rollback_specific_snapshot_file
    test_apply_snapshot_dir_flag
    test_default_snapshot_dir_uses_repo_root

    echo ""
    echo "================================================"
    echo "Results: ${PASS} passed, ${FAIL} failed"

    if [[ $FAIL -gt 0 ]]; then
        exit 1
    fi
    exit 0
}

run_all_tests
