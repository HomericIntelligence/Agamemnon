# CLAUDE.md — ProjectAgamemnon

## Project Overview

ProjectAgamemnon is the planning, coordination, and agentic orchestration service for the
HomericIntelligence distributed agent mesh. It replaces ai-maestro's task coordination role
(per ADR-006 in Odysseus).

**Role in the pipeline:** User <-> Odysseus <-> Nestor <-> **Agamemnon** <-> agentic pipeline loop -> completion

Agamemnon receives researched briefs from ProjectNestor and manages:

- Planning breakdown (inter-repo -> per-repo -> module -> sub-module -> impl details)
- HMAS 4-layer agentic hierarchy (L0 ChiefArchitect -> L1 ComponentLead -> L2 ModuleLead -> L3 TaskAgent)
- State machine coordination for each task
- Pull-based work queue: enqueues tasks for myrmidons to pull
- GitHub Issues/Projects as backing store (not SQLite)
- REST API: `/v1/*` (coordination) and `/v1/chaos/*` (chaos injection for ProjectCharybdis)
- Peer discovery via Tailscale (100.x.x.x scan)

**Agamemnon does NOT:** research (that's Nestor), provide UI (that's Odysseus), make
myrmidon-level decisions (myrmidons communicate peer-to-peer directly).

## Architecture

All inter-component communication flows **through ProjectKeystone** (invisible transport layer).
Components publish/subscribe to logical subjects — Keystone routes transparently:

- Local (intra-host): BlazingMQ + C++20 MessageBus
- Cross-host: NATS JetStream via nats.c v3.9.1 over Tailscale

Relevant NATS subjects Agamemnon uses:

- `hi.tasks.>` — task state updates (pub/sub, Odysseus reads)
- `hi.pipeline.>` — pipeline state updates (pub/sub, Odysseus reads)
- `hi.myrmidon.{type}.>` — work queues (PULL consumers, myrmidons pull from here)

## Key Principles

1. **Pull-based:** Agamemnon enqueues work. Myrmidons pull when ready. MaxAckPending=1.
2. **GitHub = backing store:** All task state lives in GitHub Issues/Projects.
3. **Bidirectional:** Agents can clarify upstream at every stage.
4. **No research:** Receives researched briefs only. All research is Nestor's responsibility.
5. **HMAS hierarchy:** L0->L3 internal orchestration primitives manage delegation and escalation.

## Development Guidelines

- Language: C++20 exclusively
- Build: `cmake --preset debug` / `cmake --build --preset debug`
- Test: `ctest --preset debug`
- All tool invocations via `scripts/` wrappers
- Never `--no-verify`. Fix pre-commit hooks, don't bypass.
- Never merge with red CI. Green is the only valid state.

## Common Commands

```bash
just build        # Configure + build (debug)
just test         # Run tests
just lint         # Run clang-tidy
just format       # Run clang-format
just coverage     # Build + run coverage report (depends on `just deps-coverage`)
```

## HMAS Model Tier Assignments

The 4-layer agent hierarchy (also documented in `AGENTS.md`) has fixed model
tiers. Keep these in sync with `AGENTS.md` — divergence between the two files
is treated as a documentation bug.

| Layer | Role | Approved Model |
| --- | --- | --- |
| L0 | ChiefArchitect | Opus |
| L1 | ComponentLead | Opus |
| L2 | ModuleLead | Sonnet |
| L3 | TaskAgent | Sonnet |

## Python Package: `agamemnon/`

The `agamemnon/` directory holds the Python orchestration sub-package
(`HomericIntelligence-Agamemnon-Orchestration`) migrated from ProjectKeystone.
It is a hatchling-built, mypy-strict, pixi-managed package targeting Python
3.11+.

### Layout

```
agamemnon/
├── pyproject.toml          # hatchling build, ruff + mypy(strict) + pytest config
├── pixi.toml               # pixi env (default + test feature)
├── __init__.py             # top-level package marker
├── orchestration/          # main Python modules
│   ├── config.py           # Settings dataclass + load_settings()
│   ├── daemon.py           # async daemon entry; routes NATS -> DAG walker
│   ├── dag_walker.py       # walks Task graph, advances state machine
│   ├── logging.py          # structured JSON stdlib logger (AgamemnonLogger)
│   ├── models.py           # pydantic Task / Agent / TaskEvent models
│   ├── nats_listener.py    # NATSListener: subscribes to hi.* subjects
│   ├── task_claimer.py     # per-team concurrency guard for claim ops
│   ├── validation.py       # validate_id() — safe URL path construction
│   └── __main__.py         # `python -m agamemnon.orchestration` entry
└── tests/                  # pytest suite (asyncio mode = auto)
```

### Dependency rationale

- **pydantic (>=2,<3)** — typed `Task` / `Agent` / `TaskEvent` models with
  runtime validation at the NATS message boundary.
- **nats-py (>=2,<3)** — async NATS JetStream client powering `NATSListener`
  (Keystone routes `hi.tasks.>` / `hi.pipeline.>` / `hi.myrmidon.*` traffic
  to this daemon).
- **httpx (>=0.27,<1)** — used by `MaestroClient` (in
  `clients/python/src/agamemnon_client/`) to talk to the `/v1/*` REST API
  shipped from the C++ core.

### Common commands

```bash
cd agamemnon && pixi run test       # pytest (tests/)
cd agamemnon && pixi run lint       # ruff check
cd agamemnon && pixi run format     # ruff format
cd agamemnon && pixi run typecheck  # mypy --strict src/agamemnon/
```

### Migration context

Modules under `agamemnon/orchestration/` were lifted from ProjectKeystone as
part of consolidating orchestration logic into Agamemnon. Keystone retains
only the invisible transport layer (BlazingMQ + NATS routing); all task
state-machine, DAG walking, and claim-coordination logic now lives here. A
parallel copy under `clients/python/src/agamemnon/orchestration/` exists for
the published client package and is kept in sync until the consumer split
lands.
