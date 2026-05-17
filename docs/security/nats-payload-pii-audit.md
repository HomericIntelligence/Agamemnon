# NATS Payload PII Audit

**Issue:** #351
**Date:** 2026-05-17
**Auditor:** automated audit (hard-tier Sonnet agent)
**Scope:** All publish call sites on `hi.tasks.>`, `hi.pipeline.>`, `hi.myrmidon.{type}.>`, `hi.agents.>`, and `hi.logs.>`

---

## Method

Every `np->publish(...)` and `nats_.publish(...)` call was traced to its payload
construction.  Payloads derived from `result.dump()` were traced back to
`Store::create_*` / `Store::update_*` methods in `src/store.cpp` and to
`hmas_task_to_json` / `task_brief_to_json` in `src/hmas_types.cpp`.

---

## Subject-by-Subject Field Inventory

### `hi.tasks.created` (`src/routes.cpp:574`)

Payload: full task object returned by `Store::create_task` (`src/store.cpp:389-418`).

| Field | Type | PII risk |
|---|---|---|
| `id` | UUID | None |
| `teamId` | UUID | None |
| `subject` | Free-text (user-supplied) | **Potential PII** — operator-authored label; low risk, no personal data expected |
| `description` | Free-text (user-supplied) | **Potential PII** — Nestor brief text may include requirement narratives |
| `assigneeAgentId` | Agent UUID | None |
| `blockedBy` | Array of UUIDs | None |
| `type` | Enum (`general`, etc.) | None |
| `status` | Enum (`pending`, etc.) | None |
| `createdAt` | ISO-8601 timestamp | None |
| `completedAt` | ISO-8601 timestamp or null | None |

### `hi.tasks.{team_id}.{task_id}.updated` (`src/routes.cpp:646`, `699`)

Payload: `{"task": <full task object>}`.  Same fields as `hi.tasks.created` above.

### `hi.myrmidon.{type}.{task_id}` (`src/routes.cpp:587`)

Payload constructed inline (`src/routes.cpp:581-586`):

| Field | Type | PII risk |
|---|---|---|
| `task_id` | UUID | None |
| `team_id` | UUID | None |
| `subject` | Free-text (user-supplied) | **Potential PII** — same as task `subject` |
| `description` | Free-text (user-supplied) | **Potential PII** — same as task `description` |
| `type` | Enum | None |
| `assignee` | Agent UUID | None |

### `hi.myrmidon.{layer}.{task_id}` — HMAS orchestration (`src/orchestrator.cpp:34`, `57`, `83`)

Payload: `hmas_task_to_json(task)` (`src/hmas_types.cpp:67-89`).

| Field | Type | PII risk |
|---|---|---|
| `id` | UUID | None |
| `brief_id` | UUID | None |
| `parent_task_id` | UUID | None |
| `layer` | Enum (`L0_ChiefArchitect`, etc.) | None |
| `state` | Enum | None |
| `subject` | Free-text (from TaskBrief) | **Potential PII** — propagated from Nestor brief |
| `description` | Free-text (from TaskBrief) | **Potential PII** — propagated from Nestor brief |
| `repo` | Repo name string | None (internal identifier) |
| `module` | Module name string | None (internal identifier) |
| `assigned_lead_id` | Agent UUID | None |
| `blocked_by` | Array of UUIDs | None |
| `child_task_ids` | Array of UUIDs | None |
| `created_at` | ISO-8601 timestamp | None |
| `completed_at` | ISO-8601 timestamp | None |
| `escalations` | Array of `EscalationRecord` | See below |

**`EscalationRecord` fields** (`src/hmas_types.cpp:60-65`):

| Field | Type | PII risk |
|---|---|---|
| `task_id` | UUID | None |
| `reason` | Free-text (operator-supplied) | Low — internal triage note |
| `escalated_at` | ISO-8601 timestamp | None |
| `from_layer` | Enum | None |

### `hi.agents.{host}.{name}.created` / `.updated` (`src/routes.cpp:329`, `346`, `361`, `433`)

Payload: full agent object returned by `Store::create_agent` / `Store::update_agent` (`src/store.cpp:195-271`).

| Field | Type | PII risk |
|---|---|---|
| `id` | UUID | None |
| `name` | Free-text (operator-assigned handle) | Low — internal agent name, not a human name |
| `label` | Free-text | Low — internal label |
| `program` | Binary path string | Low — filesystem path |
| `workingDirectory` | Filesystem path | Low — filesystem path |
| `programArgs` | Array of strings | Low — CLI args, could theoretically include tokens if misconfigured |
| `taskDescription` | Free-text | **Potential PII** — free-text description of agent's task |
| `tags` | Array of strings | Low |
| `owner` | Free-text | **Potential PII** — intended as an operator field; could hold a human identifier |
| `role` | Enum-like string (`worker`, etc.) | None |
| `host` | Hostname string | Low — Tailscale hostname, not a human identifier |
| `status` | Enum | None |
| `createdAt` | ISO-8601 timestamp | None |

### `hi.agents.{host}.{name}.deleted` (`src/routes.cpp:448`)

Payload: `{"id": <uuid>}` — no PII.

### `hi.agents.team.created` / `.updated` / `.deleted` (`src/routes.cpp:477`, `513`, `525`)

Team object fields (`src/store.cpp:315-334`): `id` (UUID), `name`,
`agentIds`, `createdAt`. Operator-assigned team label, low risk.

### `hi.agents.chaos.injected` / `.removed` (`src/routes.cpp:727`, `739`)

Fault object (`src/store.cpp:516-534`): `id` (UUID), `type`, `active`,
`createdAt`. No PII.

### `hi.logs.agamemnon.*` (`src/routes.cpp:330`, `588`, `650`, `703`; `src/orchestrator.cpp:35`, `84`, `149`)

Structured log events. Metadata fields are UUIDs, enums, counts.
Human-readable `message` strings include UUIDs only (e.g. `"Agent created:
<uuid>"`). No free-text user content is interpolated.

### `hi.pipeline.>` — not observed

No `hi.pipeline.>` publish in `src/` or `include/`. Python `NATSListener`
subscribes to it; Agamemnon does not publish to this subject.

---

## PII Verdict Summary

| Subject | PII-bearing fields | Risk level |
|---|---|---|
| `hi.tasks.created` | `subject`, `description` | **Medium** — free-text Nestor brief content |
| `hi.tasks.{team}.{id}.updated` | `subject`, `description` | **Medium** |
| `hi.myrmidon.{type}.{id}` (REST task dispatch) | `subject`, `description` | **Medium** |
| `hi.myrmidon.{layer}.{id}` (HMAS orchestration) | `subject`, `description`, `escalations[].reason` | **Medium** |
| `hi.agents.{host}.{name}.created/updated` | `taskDescription`, `owner` | **Low-Medium** — operator-controlled fields |
| `hi.agents.team.*` | `name` | **Low** |
| `hi.agents.chaos.*` | None | None |
| `hi.logs.agamemnon.*` | None (UUIDs only in metadata) | None |
| `hi.pipeline.>` | Not published by Agamemnon | N/A |

---

## Findings

1. **No email addresses, IP addresses, or user credentials** are serialized into any
   NATS payload.  The `host` field is a Tailscale hostname, not an IP.

2. **Free-text fields are present** on three subject families: `hi.tasks.*`,
   `hi.myrmidon.*`, and `hi.agents.*`.  Fields `subject`, `description`, and
   `taskDescription` carry operator/Nestor-authored text that _could_ contain
   user-identifying information if Nestor briefs incorporate user-supplied queries
   verbatim.  At the current system boundary (Agamemnon receives pre-processed
   briefs from Nestor), this is a second-order risk rather than a direct one.

3. **`owner` field on agents** (`src/store.cpp:208`) is free-text with no
   validation — if populated with a human name or email it would propagate
   across agent lifecycle events.  No enforcement prevents PII here.

4. **NATS subjects themselves** encode `host` and `name` for agent events
   (`hi.agents.{host}.{name}.*`); both are operator-assigned identifiers, not
   personal data.

---

## Recommendation

No immediate redaction is warranted for a closed-network, operator-only deployment.
However, two targeted mitigations are tracked as follow-up:

- **Follow-up A** — Validate the `owner` field at the `/v1/agents` boundary to
  enforce that it holds only an agent UUID or a registered team name, not a
  free-text human identifier.  This prevents accidental PII injection via
  `hi.agents.*` events.

- **Follow-up B** — Evaluate whether `subject` and `description` on
  `hi.myrmidon.*` payloads should be stripped or hashed before enqueuing,
  since myrmidons only need `task_id` / `team_id` / `type` / `assignee` to
  pick up work.  The full text is redundant at the work-queue boundary.

A follow-up tracking issue will be filed for both items.

---

## Evidence index

| Claim | File | Lines |
|---|---|---|
| Task fields serialized | `src/store.cpp` | 389–418 |
| Agent fields serialized | `src/store.cpp` | 195–224 |
| Team fields serialized | `src/store.cpp` | 315–334 |
| Fault fields serialized | `src/store.cpp` | 516–534 |
| `hmas_task_to_json` | `src/hmas_types.cpp` | 67–89 |
| `escalation_record_to_json` | `src/hmas_types.cpp` | 60–65 |
| `task_brief_to_json` | `src/hmas_types.cpp` | 91–111 |
| `hi.tasks.created` publish | `src/routes.cpp` | 574 |
| `hi.tasks.*.updated` publish | `src/routes.cpp` | 646, 699 |
| `hi.myrmidon.*` REST dispatch | `src/routes.cpp` | 581–587 |
| `hi.myrmidon.*` HMAS publish | `src/orchestrator.cpp` | 34, 57, 83 |
| `hi.agents.*` publish | `src/routes.cpp` | 329, 346, 361, 433, 448, 477, 513, 525, 727, 739 |
| `hi.logs.*` publish | `src/routes.cpp` | 330, 588, 650, 703 |
| `hi.logs.*` publish | `src/orchestrator.cpp` | 35, 84, 149 |
