# NATS Event Reference

Agamemnon publishes to NATS on every mutating operation. Subjects follow the
`hi.*` namespace. Odysseus subscribes to `hi.tasks.>` and `hi.pipeline.>`;
myrmidons pull from `hi.myrmidon.{type}.>`.

All payloads are JSON-encoded strings.

---

## Published Events (Agamemnon → NATS)

| Endpoint | NATS Subject | Trigger | Payload |
|----------|-------------|---------|---------|
| `POST /v1/agents` | `hi.agents.{host}.{name}.created` | Always | Full `{"id":..., "agent":{...}}` response |
| `POST /v1/agents/docker` | `hi.agents.{host}.{name}.created` | Always | Full `{"id":..., "agent":{...}}` response |
| `POST /v1/agents/{id}/start` | `hi.agents.{host}.{name}.updated` | Always | `{"status":"online","id":"..."}` |
| `POST /v1/agents/{id}/stop` | `hi.agents.{host}.{name}.updated` | Always | `{"status":"offline","id":"..."}` |
| `PATCH /v1/agents/{id}` | `hi.agents.{host}.{name}.updated` | Always | Full agent object |
| `DELETE /v1/agents/{id}` | `hi.agents.{host}.{name}.deleted` | Always | `{"id":"<deleted-id>"}` |
| `POST /v1/teams` | `hi.agents.team.created` | Always | Full `{"team":{...}}` response |
| `PUT /v1/teams/{id}` | `hi.agents.team.updated` | Always | Full `{"team":{...}}` response |
| `DELETE /v1/teams/{id}` | `hi.agents.team.deleted` | Always | `{"id":"<deleted-id>"}` |
| `POST /v1/teams/{team_id}/tasks` | `hi.tasks.created` | Always | Full `{"task":{...}}` response |
| `POST /v1/teams/{team_id}/tasks` | `hi.myrmidon.{type}.{task_id}` | Always | Myrmidon work payload (see below) |
| `PUT /v1/teams/{team_id}/tasks/{task_id}` | `hi.tasks.{team_id}.{task_id}.updated` | Always | Full `{"task":{...}}` response |
| `PATCH /v1/teams/{team_id}/tasks/{task_id}` | `hi.tasks.{team_id}.{task_id}.updated` | Always | Full `{"task":{...}}` response |
| `POST /v1/chaos/{type}` | `hi.agents.chaos.injected` | Always | Full `{"fault":{...}}` response |
| `DELETE /v1/chaos/{id}` | `hi.agents.chaos.removed` | Always | `{"id":"<deleted-id>"}` |

### Conditional log events

| Endpoint | NATS Subject | Trigger | Payload |
|----------|-------------|---------|---------|
| `POST /v1/agents` | `hi.logs.agamemnon.agent_created` | Always | Log envelope with `agent_id`, `name`, `type`, `host` |
| `POST /v1/agents/docker` | `hi.logs.agamemnon.agent_created` | Always | Log envelope with `agent_id`, `name`, `type`, `host` |
| `POST /v1/teams/{team_id}/tasks` | `hi.logs.agamemnon.task_dispatched` | Always | Log envelope with `task_id`, `team_id`, `type`, `subject` |
| `PUT /v1/teams/{team_id}/tasks/{task_id}` | `hi.logs.agamemnon.task_completed` | Only when `status == "completed"` | Log envelope with `task_id`, `team_id`, `type`, `assignee` |
| `PATCH /v1/teams/{team_id}/tasks/{task_id}` | `hi.logs.agamemnon.task_completed` | Only when `status == "completed"` | Log envelope with `task_id`, `team_id`, `type`, `assignee` |

---

## Subscribed Events (NATS → Agamemnon)

Agamemnon also subscribes to one subject to receive myrmidon completions:

| Subject | Publisher | Action |
|---------|-----------|--------|
| `hi.tasks.*.*.completed` | Myrmidons | Calls `store.mark_task_completed(task_id)` — sets `status=completed` and `completedAt=now()` |

The myrmidon payload is expected to contain either `task_id` at the top level or nested
under a `data` key:

```json
{"task_id": "<uuid>"}
// or
{"data": {"task_id": "<uuid>"}}
```

---

## Myrmidon Work Payload

When a task is created, Agamemnon dispatches to `hi.myrmidon.{type}.{task_id}`:

```json
{
  "task_id": "<uuid>",
  "team_id": "<uuid>",
  "subject": "<task subject>",
  "description": "<task description>",
  "type": "<task type>",
  "assignee": "<assigneeAgentId>"
}
```

The NATS stream for myrmidon subjects uses `MaxAckPending=1` (pull-based, one-at-a-time).

---

## Subject Namespace Reference

| Prefix | Description |
|--------|-------------|
| `hi.agents.>` | Agent lifecycle events and team events |
| `hi.tasks.>` | Task state updates (Odysseus subscribes here) |
| `hi.pipeline.>` | Pipeline state updates (Odysseus subscribes here) |
| `hi.myrmidon.{type}.>` | Work queues — PULL consumers, myrmidons pull from here |
| `hi.logs.agamemnon.>` | Structured log events from Agamemnon |
