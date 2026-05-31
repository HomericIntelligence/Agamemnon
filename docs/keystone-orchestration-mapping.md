# Keystone HMAS Orchestration → Agamemnon Capability Mapping

ProjectKeystone historically carried an HMAS *orchestration* layer under
`src/network/` + `include/network/` (a gRPC coordinator, service registry,
task router, result aggregator, hierarchical-task YAML parser, and task-phase
utilities). Per **ADR-015 / ADR-016**, orchestration belongs to
**ProjectAgamemnon**; Keystone is a pure C++20 transport library
(`keystone_transport`: `MessageBus`, `NATSListener`, `NatsConnection`,
`TransparentBridge`, serializer, concurrency).

This document records the result of the verify-or-port review performed when
the orchestration layer was removed from Keystone. **Every Keystone
orchestration capability already has an Agamemnon equivalent**, so nothing was
ported verbatim. The gRPC transport itself is intentionally *not* carried over:
Agamemnon orchestrates over **NATS + an HTTP/JSON API**, not gRPC.

## Capability mapping

| Keystone (`keystone::network`) | Agamemnon equivalent | Notes |
|---|---|---|
| `TaskRouter` (round-robin / least-loaded / random load balancing) | `Orchestrator::delegate()` + `Orchestrator::myrmidon_subject()` publishing to `hi.myrmidon.<layer>.<task_id>`, combined with the work-stealing scheduler (`src/concurrency/work_stealing_scheduler.cpp`, `pull_or_steal.cpp`) | Agamemnon uses a **pull-based** work-stealing model: agents pull/steal work from per-layer NATS subjects rather than the coordinator pushing via an explicit load-balancing strategy. This subsumes round-robin / least-loaded. |
| `ResultAggregator` (WAIT_ALL / FIRST_SUCCESS / MAJORITY) | `Orchestrator::delegate_unblocked_children()` (WAIT_ALL dependency-completion semantics via `HmasTask::blocked_by`) + `ComponentLeadAgent` `AGGREGATING` state (`include/agamemnon_agents/agents/component_lead_agent.hpp`) | A child task unblocks only once **all** its blockers reach `Completed` — the WAIT_ALL strategy expressed as a dependency DAG. The component-lead agent owns module-result aggregation. |
| `YamlParser` → `HierarchicalTaskSpec` (metadata / routing / hierarchy / action / payload / subtasks / aggregation) | `TaskBrief` + `task_brief_from_json()` (`src/hmas_types.cpp`) ingested via `POST /v1/briefs` (`src/routes.cpp`) | Agamemnon ingests the **same hierarchy** (repos → modules → impl tasks) as JSON over HTTP instead of YAML. The hierarchical task spec is preserved; only the wire format differs. |
| `ServiceRegistry` (agent registration / discovery / heartbeats) | `Store` agent lifecycle (`create_agent` / `list_agents` / `start_agent` / `stop_agent`, `src/store.cpp`) + NATS peer discovery (`src/peer_discovery.cpp`) | Agent inventory lives in the `Store`; live NATS peers are discovered over Tailscale. |
| `HMASCoordinatorServiceImpl` (gRPC: SubmitTask / StreamTaskStatus / GetTaskResult / CancelTask) | HTTP API in `src/routes.cpp`: `POST /v1/briefs` (submit), `GET /v1/briefs/:id/plan` + `GET /v1/tasks/:id/state` (status/result), `POST /v1/tasks/:id/escalate`, `POST /v1/tasks/:id/complete`; cancellation at the agent level via `AgentCore::requestCancellation()` (`include/agamemnon_agents/agents/agent_core.hpp`) | Same orchestration *logic*, different transport (HTTP/JSON + NATS rather than gRPC). The gRPC wrapper is deliberately dropped. |
| `task_phase_utils` (`TaskPhase` ↔ string, terminal-state detection) | `TaskState` enum + `task_state_to_string` / `task_state_from_string` (`src/hmas_types.cpp`) and the transition table in `src/state_machine.cpp` | Agamemnon's `TaskState` (`Pending`/`Decomposing`/`Delegated`/`InProgress`/`Escalated`/`Completed`/`Failed`) covers phase tracking; `Completed` and `Failed` are the terminal states. |

## Outcome

No code was ported: Agamemnon's NATS + HTTP orchestration stack already
provides every capability the Keystone `keystone::network` orchestration layer
offered. The Keystone-side removal (the companion PR) is therefore safe and
loses no functionality.

## See also

- ADR-015 / ADR-016 — orchestration belongs to Agamemnon; Keystone is pure transport
- `docs/migration-from-keystone.md` — the earlier `KEYSTONE_*` → `AGAMEMNON_*` env-var rename
