#!/usr/bin/env bats
# tests/unit/test_mock_server_routes.bats
#
# Tests for mock_server.py per-route response feature (issue #192).
# Verifies that a single mock server instance can serve different responses
# for different method+path combinations, enabling multi-step workflow tests.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"
HELPERS_DIR="${SCRIPT_DIR}/tests/helpers"

MOCK_PORT=18085
MOCK_PID_FILE="/tmp/bats-mock-routes-$$.pid"
ROUTES_FILE="/tmp/bats-mock-routes-$$.json"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_start_mock_with_routes() {
    local routes_json="$1"
    printf '%s' "$routes_json" > "$ROUTES_FILE"
    python3 "${HELPERS_DIR}/mock_server.py" "$MOCK_PORT" --routes "$ROUTES_FILE" \
        > /dev/null 2>&1 &
    echo $! > "$MOCK_PID_FILE"
    sleep 0.2
}

_stop_mock_server() {
    if [[ -f "$MOCK_PID_FILE" ]]; then
        kill "$(cat "$MOCK_PID_FILE")" 2>/dev/null || true
        rm -f "$MOCK_PID_FILE"
    fi
    rm -f "$ROUTES_FILE"
}

_curl_get() {
    # -s: silent, no -f so that 4xx/5xx responses still print the status code
    curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${MOCK_PORT}${1}"
}

_curl_get_body() {
    curl -sf "http://127.0.0.1:${MOCK_PORT}${1}"
}

_curl_post_code() {
    curl -s -o /dev/null -w "%{http_code}" -X POST \
        -H "Content-Type: application/json" -d '{}' \
        "http://127.0.0.1:${MOCK_PORT}${1}"
}

teardown() {
    _stop_mock_server
}

# ---------------------------------------------------------------------------
# Flat-array JSON format (issue #192 spec)
# ---------------------------------------------------------------------------

@test "mock_server: per-route flat-array — GET /api/v1/agents returns 200 with agent list" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":[{"id":"a1","name":"agent-one","status":"active"}]},
        {"method":"POST","path":"/api/v1/agents","status":201,"body":{"id":"a2","name":"agent-two","status":"offline"}}
    ]'
    code="$(_curl_get /api/v1/agents)"
    [[ "$code" == "200" ]]
}

@test "mock_server: per-route flat-array — GET body contains expected agent data" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":[{"id":"a1","name":"agent-one","status":"active"}]}
    ]'
    body="$(_curl_get_body /api/v1/agents)"
    name="$(echo "$body" | jq -r '.[0].name')"
    [[ "$name" == "agent-one" ]]
}

@test "mock_server: per-route flat-array — POST /api/v1/agents returns 201" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":[]},
        {"method":"POST","path":"/api/v1/agents","status":201,"body":{"id":"new1","name":"new-agent"}}
    ]'
    code="$(_curl_post_code /api/v1/agents)"
    [[ "$code" == "201" ]]
}

@test "mock_server: per-route — different paths return different responses" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":{"route":"list"}},
        {"method":"GET","path":"/api/v1/status","status":200,"body":{"route":"status"}}
    ]'
    body_agents="$(_curl_get_body /api/v1/agents)"
    body_status="$(_curl_get_body /api/v1/status)"
    route_agents="$(echo "$body_agents" | jq -r '.route')"
    route_status="$(echo "$body_status" | jq -r '.route')"
    [[ "$route_agents" == "list" ]]
    [[ "$route_status" == "status" ]]
}

# ---------------------------------------------------------------------------
# Wildcard path matching
# ---------------------------------------------------------------------------

@test "mock_server: wildcard path — GET /api/v1/agents/* matches specific agent ID" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents/*","status":200,"body":{"id":"abc123","name":"my-agent"}}
    ]'
    body="$(_curl_get_body /api/v1/agents/abc123)"
    id="$(echo "$body" | jq -r '.id')"
    [[ "$id" == "abc123" ]]
}

@test "mock_server: wildcard path — exact match takes priority over wildcard (first match wins)" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents/special","status":200,"body":{"route":"exact"}},
        {"method":"GET","path":"/api/v1/agents/*","status":200,"body":{"route":"wildcard"}}
    ]'
    body_exact="$(_curl_get_body /api/v1/agents/special)"
    body_wild="$(_curl_get_body /api/v1/agents/other)"
    route_exact="$(echo "$body_exact" | jq -r '.route')"
    route_wild="$(echo "$body_wild" | jq -r '.route')"
    [[ "$route_exact" == "exact" ]]
    [[ "$route_wild" == "wildcard" ]]
}

# ---------------------------------------------------------------------------
# Fallback behaviour
# ---------------------------------------------------------------------------

@test "mock_server: per-route — unmatched path falls back to default_status/default_body" {
    _start_mock_with_routes '{
        "routes":[{"method":"GET","path":"/api/v1/agents","status":200,"body":[]}],
        "default_status":404,
        "default_body":{"error":"not found"}
    }'
    code="$(_curl_get /api/v1/unknown)"
    [[ "$code" == "404" ]]
}

@test "mock_server: per-route — unmatched method falls back to default even if path matches" {
    _start_mock_with_routes '{
        "routes":[{"method":"GET","path":"/api/v1/agents","status":200,"body":[]}],
        "default_status":405,
        "default_body":{"error":"method not allowed"}
    }'
    code="$(_curl_post_code /api/v1/agents)"
    [[ "$code" == "405" ]]
}

@test "mock_server: per-route — fallback to MOCK_STATUS env var when no default in config" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":[]}
    ]'
    # No MOCK_STATUS set → defaults to 200 for unmatched routes
    code="$(_curl_get /api/v1/unmatched)"
    [[ "$code" == "200" ]]
}

# ---------------------------------------------------------------------------
# Object-wrapper format (legacy / alternative)
# ---------------------------------------------------------------------------

@test "mock_server: object-format routes config — routes key works correctly" {
    _start_mock_with_routes '{
        "routes":[
            {"method":"GET","path":"/health","status":200,"body":{"healthy":true}}
        ],
        "default_status":503,
        "default_body":{"healthy":false}
    }'
    body="$(_curl_get_body /health)"
    healthy="$(echo "$body" | jq -r '.healthy')"
    [[ "$healthy" == "true" ]]
}

# ---------------------------------------------------------------------------
# Multi-step workflow test (the core use-case from issue #192)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Use-once route support (issue #433)
# ---------------------------------------------------------------------------

@test "mock_server: once route — first GET returns once-route body, second falls through to default_body" {
    _start_mock_with_routes '{
        "routes":[
            {"method":"GET","path":"/api/v1/agents","status":200,"body":[{"status":"offline"}],"once":true}
        ],
        "default_status":200,
        "default_body":[{"status":"active"}]
    }'
    body1="$(_curl_get_body /api/v1/agents)"
    status1="$(echo "$body1" | jq -r '.[0].status')"
    body2="$(_curl_get_body /api/v1/agents)"
    status2="$(echo "$body2" | jq -r '.[0].status')"
    [[ "$status1" == "offline" ]]
    [[ "$status2" == "active" ]]
}

@test "mock_server: once route — once consumed, next explicit route matches on second call" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":{"hit":"first"},"once":true},
        {"method":"GET","path":"/api/v1/agents","status":200,"body":{"hit":"second"}}
    ]'
    body1="$(_curl_get_body /api/v1/agents)"
    hit1="$(echo "$body1" | jq -r '.hit')"
    body2="$(_curl_get_body /api/v1/agents)"
    hit2="$(echo "$body2" | jq -r '.hit')"
    body3="$(_curl_get_body /api/v1/agents)"
    hit3="$(echo "$body3" | jq -r '.hit')"
    [[ "$hit1" == "first" ]]
    [[ "$hit2" == "second" ]]
    # third call still matches the non-once second route
    [[ "$hit3" == "second" ]]
}

@test "mock_server: non-once route — served on every call (regression guard)" {
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":{"stable":true}}
    ]'
    for _ in 1 2 3; do
        body="$(_curl_get_body /api/v1/agents)"
        val="$(echo "$body" | jq -r '.stable')"
        [[ "$val" == "true" ]]
    done
}

@test "mock_server: once route — works on POST method, not just GET" {
    _start_mock_with_routes '{
        "routes":[
            {"method":"POST","path":"/api/v1/agents","status":201,"body":{"created":true},"once":true}
        ],
        "default_status":200,
        "default_body":{"created":false}
    }'
    body1="$(curl -sf -X POST -H "Content-Type: application/json" -d '{}' \
        "http://127.0.0.1:${MOCK_PORT}/api/v1/agents")"
    created1="$(echo "$body1" | jq -r '.created')"
    body2="$(curl -sf -X POST -H "Content-Type: application/json" -d '{}' \
        "http://127.0.0.1:${MOCK_PORT}/api/v1/agents")"
    created2="$(echo "$body2" | jq -r '.created')"
    [[ "$created1" == "true" ]]
    [[ "$created2" == "false" ]]
}

@test "mock_server: multiple once routes — each consumed in order" {
    _start_mock_with_routes '{
        "routes":[
            {"method":"GET","path":"/api/v1/agents","status":200,"body":[{"seq":1}],"once":true},
            {"method":"GET","path":"/api/v1/agents","status":200,"body":[{"seq":2}],"once":true}
        ],
        "default_status":200,
        "default_body":[{"seq":3}]
    }'
    seq1="$(_curl_get_body /api/v1/agents | jq -r '.[0].seq')"
    seq2="$(_curl_get_body /api/v1/agents | jq -r '.[0].seq')"
    seq3="$(_curl_get_body /api/v1/agents | jq -r '.[0].seq')"
    [[ "$seq1" == "1" ]]
    [[ "$seq2" == "2" ]]
    [[ "$seq3" == "3" ]]
}

@test "mock_server: multi-step workflow — list then get then update use single server" {
    # Simulates a workflow that: lists agents, fetches one by ID, then patches it.
    _start_mock_with_routes '[
        {"method":"GET","path":"/api/v1/agents","status":200,"body":[{"id":"wf1","name":"workflow-agent","status":"offline"}]},
        {"method":"GET","path":"/api/v1/agents/*","status":200,"body":{"id":"wf1","name":"workflow-agent","status":"offline"}},
        {"method":"PATCH","path":"/api/v1/agents/*","status":200,"body":{"id":"wf1","name":"workflow-agent","status":"active"}}
    ]'

    # Step 1: list
    list_body="$(_curl_get_body /api/v1/agents)"
    agent_id="$(echo "$list_body" | jq -r '.[0].id')"
    [[ "$agent_id" == "wf1" ]]

    # Step 2: get by ID
    get_body="$(_curl_get_body "/api/v1/agents/${agent_id}")"
    get_status="$(echo "$get_body" | jq -r '.status')"
    [[ "$get_status" == "offline" ]]

    # Step 3: patch (wake)
    patch_body="$(curl -sf -X PATCH -H "Content-Type: application/json" -d '{"status":"active"}' \
        "http://127.0.0.1:${MOCK_PORT}/api/v1/agents/${agent_id}")"
    patch_status="$(echo "$patch_body" | jq -r '.status')"
    [[ "$patch_status" == "active" ]]
}
