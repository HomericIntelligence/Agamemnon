# ProjectAgamemnon API Reference

Full machine-readable spec: [`openapi.yaml`](openapi.yaml) (OpenAPI 3.1)

**Base URL:** `http://localhost:8080` (override port with `PORT` env var)

**Authentication:** None — Phase 1 is unauthenticated. All endpoints are open.

---

## Endpoint Summary

### Health

| Method | Path | Summary | Response |
|--------|------|---------|----------|
| GET | `/health` | Root health check | `{"status":"ok","service":"ProjectAgamemnon"}` |
| GET | `/v1/health` | Versioned health check | `{"status":"ok"}` |
| GET | `/v1/version` | Service version | `{"version":"0.1.0","name":"ProjectAgamemnon"}` |

---

### Agents

| Method | Path | Summary | Request Body | Response |
|--------|------|---------|-------------|----------|
| GET | `/v1/agents` | List all agents | — | `{"agents":[...]}` |
| POST | `/v1/agents` | Create agent | `AgentCreate` | 201 `{"id":"...","agent":{...}}` |
| POST | `/v1/agents/docker` | Create Docker agent | `AgentCreate` | 201 `{"id":"...","agent":{...}}` |
| GET | `/v1/agents/by-name/{name}` | Get agent by name | — | `{"agent":{...}}` |
| GET | `/v1/agents/{id}` | Get agent by ID | — | `{"agent":{...}}` |
| PATCH | `/v1/agents/{id}` | Partially update agent | `AgentPatch` | `{"agent":{...}}` |
| DELETE | `/v1/agents/{id}` | Delete agent | — | `{"deleted":"<id>"}` |
| POST | `/v1/agents/{id}/start` | Set agent online | — | `{"status":"online","id":"..."}` |
| POST | `/v1/agents/{id}/stop` | Set agent offline | — | `{"status":"offline","id":"..."}` |

**Agent object fields:**

| Field | Type | Notes |
|-------|------|-------|
| `id` | string (UUID) | Assigned at creation; immutable |
| `name` | string | Default `"unnamed"` |
| `label` | string | Optional label |
| `program` | string | Executable path |
| `workingDirectory` | string | Working directory for the process |
| `programArgs` | string[] | CLI arguments |
| `taskDescription` | string | Human-readable task description |
| `tags` | string[] | Arbitrary tags |
| `owner` | string | Owning entity |
| `role` | string | Default `"worker"` |
| `host` | string | Default `"local"`; `"docker"` for Docker agents |
| `status` | `online`\|`offline` | Always `"offline"` at creation |
| `createdAt` | ISO 8601 UTC | Assigned at creation; immutable |

**Route registration note:** `POST /v1/agents/docker` and `GET /v1/agents/by-name/{name}` and
`POST /v1/agents/{id}/start` / `POST /v1/agents/{id}/stop` are registered in cpp-httplib
*before* the generic `POST /v1/agents` and `GET /v1/agents/{id}` routes to avoid prefix
collision.

---

### Teams

| Method | Path | Summary | Request Body | Response |
|--------|------|---------|-------------|----------|
| GET | `/v1/teams` | List all teams | — | `{"teams":[...]}` |
| POST | `/v1/teams` | Create team | `TeamCreate` | 201 `{"team":{...}}` |
| GET | `/v1/teams/{id}` | Get team by ID | — | `{"team":{...}}` |
| PUT | `/v1/teams/{id}` | Update team fields | `TeamUpdate` | `{"team":{...}}` |
| DELETE | `/v1/teams/{id}` | Delete team | — | `{"deleted":"<id>"}` |

**Team object fields:**

| Field | Type | Notes |
|-------|------|-------|
| `id` | string (UUID) | Assigned at creation; immutable |
| `name` | string | Default `"unnamed-team"` |
| `agentIds` | string[] | Agent UUIDs; also accepted as `agent_ids` on input |
| `createdAt` | ISO 8601 UTC | Assigned at creation; immutable |

---

### Tasks

| Method | Path | Summary | Request Body | Response |
|--------|------|---------|-------------|----------|
| GET | `/v1/tasks` | List all tasks (all teams) | — | `{"tasks":[...]}` |
| GET | `/v1/teams/{team_id}/tasks` | List tasks for a team | — | `{"tasks":[...]}` |
| POST | `/v1/teams/{team_id}/tasks` | Create task | `TaskCreate` | 201 `{"task":{...}}` |
| GET | `/v1/teams/{team_id}/tasks/{task_id}` | Get task by ID | — | `{"task":{...}}` |
| PUT | `/v1/teams/{team_id}/tasks/{task_id}` | Update task (full merge) | `TaskUpdate` | `{"task":{...}}` |
| PATCH | `/v1/teams/{team_id}/tasks/{task_id}` | Partially update task | `TaskUpdate` | `{"task":{...}}` |

**Task object fields:**

| Field | Type | Notes |
|-------|------|-------|
| `id` | string (UUID) | Assigned at creation; immutable |
| `teamId` | string | Set from URL path; immutable |
| `subject` | string | Short title |
| `description` | string | Long-form description |
| `assigneeAgentId` | string | Assigned agent UUID; empty if unassigned |
| `blockedBy` | string[] | Task IDs blocking this task |
| `type` | string | Default `"general"`; used as myrmidon queue key |
| `status` | string | `pending`\|`in_progress`\|`completed`\|`failed`; always `"pending"` at creation |
| `createdAt` | ISO 8601 UTC | Assigned at creation; immutable |
| `completedAt` | ISO 8601 UTC \| null | Auto-set when `status` → `"completed"` |

**PUT vs PATCH:** Both use identical merge semantics server-side. PUT is used by Telemachy
(the myrmidon completion protocol); PATCH is available for partial updates.

**Myrmidon dispatch:** On task creation, Agamemnon publishes to
`hi.myrmidon.{type}.{task_id}` so a myrmidon can pull the work. The myrmidon later
publishes to `hi.tasks.{team_id}.{task_id}.completed`, which Agamemnon subscribes to and
auto-marks the task completed.

---

### Workflows

| Method | Path | Summary | Response |
|--------|------|---------|----------|
| GET | `/v1/workflows` | List workflows | `{"workflows":[]}` |

> **Not implemented.** Always returns an empty array. Placeholder for future workflow support.

---

### Chaos

| Method | Path | Summary | Request Body | Response |
|--------|------|---------|-------------|----------|
| GET | `/v1/chaos` | List active faults | — | `{"faults":[...]}` |
| POST | `/v1/chaos/{type}` | Inject fault | — | 201 `{"fault":{...}}` |
| DELETE | `/v1/chaos/{id}` | Remove fault | — | `{"deleted":"<id>"}` |

**Fault object fields:**

| Field | Type | Notes |
|-------|------|-------|
| `id` | string (UUID) | Assigned at creation |
| `type` | string | Fault type from path (e.g. `latency`, `drop`, `corrupt`) |
| `active` | boolean | Always `true` when created |
| `createdAt` | ISO 8601 UTC | |

---

## Error Responses

All error responses use the same shape:

```json
{"error": "<human-readable message>"}
```

| Status | Condition |
|--------|-----------|
| 400 | Invalid JSON body |
| 404 | Resource not found (`"<resource> not found"`) |

---

## Known Gaps

- **No authentication** — Phase 1 intentionally unauthenticated
- **No pagination** — all list endpoints return unbounded arrays
- **No filtering** — no query parameter filtering on list endpoints
- **Workflows stub** — `GET /v1/workflows` always returns `[]`
- **In-memory only** — all state is lost on server restart; GitHub Issues/Projects
  backing store is planned for Phase 2
- **No TLS** — internal mesh traffic over Tailscale only
