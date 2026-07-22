# Roadmap — Agamemnon

This roadmap distinguishes what is **implemented today** from what is **planned**.
Each deferred feature includes its current status, why it was deferred, and measurable
acceptance criteria so contributors can pick up work with clear targets.

## Current State (v0.x)

The following are genuinely implemented and tested:

- **NATS JetStream transport** — 6 streams, pub/sub, graceful degradation when NATS
  is unavailable
- **REST API** — `/v1/agents`, `/v1/teams`, `/v1/teams/:id/tasks`, `/v1/chaos/*`,
  `/v1/health`, `/v1/version`
- **In-memory store** — `std::unordered_map`-backed store for agents, teams, tasks,
  and fault state
- **Structured logging** — ADR-005 compliant; log events published via `publish_log()`
  to NATS
- **CI pipeline** — build, test, coverage, static analysis, markdownlint, uv-lock,
  justfile, symlink checks

## Project Board

Work is tracked on the HomericIntelligence organization GitHub Projects board:
<https://github.com/orgs/HomericIntelligence/projects/TBD>

This board is shared across all HomericIntelligence repositories and provides
visibility into cross-project planning, dependency tracking, and sprint cycles.

## Deferred Features

### 1. HMAS 4-Layer Hierarchy (L0–L3)

- **Status:** Not yet implemented
- **Why:** Core infrastructure (NATS streams, REST API, in-memory store) needed to
  land first. Building delegation logic before the transport layer was stable would
  have caused significant churn.
- **Acceptance Criteria:**
  - L0 ChiefArchitect breaks incoming briefs into per-repo subtasks and publishes
    them to `hi.tasks.>`
  - L1–L3 delegation chain routes work to L3 TaskAgent PULL consumers on
    `hi.myrmidon.{type}.>`
  - Escalation path (L3 → L2 → L1 → L0) is implemented, covered by tests, and
    observable via structured log events

### 2. GitHub Issues/Projects Backing Store

- **Status:** In-memory stub (`std::unordered_map`); no GitHub API integration
- **Why:** GitHub API integration requires auth token management and rate-limit
  handling. Deferring avoided blocking REST API and NATS stream work on auth
  plumbing that was not yet designed.
- **Acceptance Criteria:**
  - Task state survives process restart (GitHub Issues as source of truth)
  - GitHub Project board reflects live task status in real time
  - In-memory store is replaced or wrapped by the GitHub-backed implementation,
    not left as a parallel path

### 3. Tailscale Peer Discovery

- **Status:** Not yet implemented; `NATS_URL` is sourced from an environment variable
  only
- **Why:** Tailscale integration requires network-layer coordination with other
  HomericIntelligence services. Deferred pending Keystone transport stabilization.
- **Acceptance Criteria:**
  - Startup scan of the `100.64.0.0/10` subnet identifies peer Agamemnon nodes
  - Discovered peers are registered in the NATS cluster or routing table
  - Discovery reruns on a configurable refresh interval and tolerates transient
    scan failures without crashing

### 4. Workflow Management (`/v1/workflows`)

- **Status:** Stub — `GET /v1/workflows` returns an empty array; no create, update,
  or delete operations exist
- **Why:** Workflow semantics depend on the HMAS hierarchy being defined first so
  that workflows can reference real task delegation chains.
- **Acceptance Criteria:**
  - Full CRUD endpoints for workflows are implemented and covered by integration
    tests
  - Workflows reference tasks from the GitHub-backed store
  - Workflow state transitions are published to `hi.pipeline.>`

## Future Enhancements (post-core)

- Expanded test suite — route-level, store-level, and `NatsClient` integration tests
- OpenAPI/Swagger spec for `/v1/*` endpoints
- Prometheus metrics endpoint
- GitHub Projects board for this repository (enable via repo Settings → Projects,
  then link issues)

---

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to pick up a deferred feature or
enhancement.
