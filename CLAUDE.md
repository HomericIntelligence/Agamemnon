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
- Cross-host: NATS JetStream via nats.c v3.12.0 over Tailscale

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
just coverage     # Build + run coverage report
```
