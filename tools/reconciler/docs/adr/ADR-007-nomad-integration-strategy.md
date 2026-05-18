# ADR-007: Nomad Integration Strategy for Multi-Host Deployments

**Status:** Accepted
**Date:** 2026-04-10
**Accepted:** 2026-05-03
**Author:** mvillmow

---

## Context

`Architecture.md` in the HomericIntelligence ecosystem (Odysseus repo) depicts
"Myrmidons + Nomad" as the solution to Gap #3 — multi-host agent scheduling —
and includes a diagram showing Myrmidons driving a Nomad cluster.

However, no Nomad integration code exists in Myrmidons. The `apply.sh` script
calls only the ProjectAgamemnon REST API. The `spec.deployment.type` field
supports `local` (tmux on a single host) and `docker` (container on a single
host). There is no Nomad job submission, no HCL generation, and no Nomad API
client anywhere in this repository.

This creates a documentation overclaim: the architecture diagram implies an
active capability that has not been implemented.

---

## Decision

Treat Nomad integration as a **deferred future phase**, not a current capability.

- Do **not** add Nomad integration code to resolve this inconsistency. No design
  exists for it; implementing it now would be scope creep without a backing spec.
- Do **not** remove Nomad from the roadmap entirely. The multi-host scheduling
  gap is real and worth tracking.
- **Update documentation** to accurately reflect current scope: Myrmidons drives
  a single host via the ProjectAgamemnon REST API. Multi-host scheduling via
  Nomad is planned for a future phase.
- Add a **drift-detection test** (`tests/detect-doc-drift.sh`) that fails CI if
  any documentation file in this repo re-introduces overclaiming language that
  implies an active Nomad integration.
- File a **cross-repo issue** in `HomericIntelligence/Odysseus` to update
  `Architecture.md` so the ecosystem diagram accurately reflects the current
  implementation state.

---

## Consequences

### Positive

- Documentation accurately reflects what `apply.sh` and the YAML schema actually do.
- The CI drift-detection test prevents future docs from silently re-introducing
  the overclaim without a corresponding code change.
- No phantom capability is advertised to users who read the architecture docs.

### Negative / Trade-offs

- Gap #3 (multi-host scheduling) remains open. Anyone expecting Nomad integration
  today will need to rely on the roadmap/future ADR instead of existing code.

### Neutral

- When Nomad integration work actually begins, a new ADR (ADR-008 or similar)
  should document the design: how Myrmidons generates Nomad job files, which
  Nomad API endpoints it calls, and how `spec.deployment.type: nomad` would work.

---

## Alternatives Considered

### A. Implement Nomad integration now

**Rejected.** No design spec exists. The `spec.deployment.type` validation in
`validate-schemas.sh` explicitly rejects any value other than `local` or
`docker`. Implementing Nomad scheduling is a substantial, multi-week effort
requiring Nomad cluster provisioning, HCL job template design, Nomad API client
code, and changes to the agent YAML schema — none of which have been designed.

### B. Remove Nomad from the roadmap entirely

**Rejected.** The multi-host scheduling gap is a real architectural limitation.
Removing it from the roadmap would discard useful context for future planning
and make it harder to revive the work later.

### C. Do nothing

**Rejected.** Overclaiming documentation erodes trust. When a reader encounters
the Architecture.md diagram and then finds no Nomad code, they cannot tell
whether the integration exists somewhere they haven't looked, was removed, or
was never built. Explicit documentation of "planned, not implemented" is
unambiguous.

---

## Implementation Scope (this ADR)

| File | Change |
| --- | --- |
| `docs/adr/ADR-007-nomad-integration-strategy.md` | This file — documents the gap and deferral decision |
| `README.md` | Adds a "Deployment scope" section clarifying supported types and linking to this ADR |
| `tests/detect-doc-drift.sh` | Drift-detection test: fails if docs imply active Nomad integration |
| `justfile` | Adds `test-drift` target; adds drift check to `validate` |
| `.github/workflows/validate.yml` | Adds drift-detection step to CI |

---

## Future Work

**Note:** ADR-008 (Fleet ref Resolution by Filename Stem) has been accepted. Future
Nomad integration work should be covered in a subsequent ADR after the necessary
design is completed.

When multi-host scheduling work begins:

1. Write a new ADR covering the Nomad integration design.
2. Add `spec.deployment.type: nomad` to the agent schema.
3. Update `apply.sh` to generate Nomad job files and call the Nomad API.
4. Update `tests/validate-schemas.sh` to accept `nomad` as a valid deployment type.
5. Update this ADR status to `Superseded by ADR-<N>`.
