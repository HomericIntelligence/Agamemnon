# Reconciler

GitOps reconciler that drives ProjectAgamemnon's REST API from the agent
definitions in `HomericIntelligence/Myrmidons`.

## Why this lives here (not in Myrmidons)

`HomericIntelligence/Myrmidons` is **a dataset repo** — YAML schema, agent
and fleet descriptions, dataset validators, dataset docs, and the CI wrappers
that run those validators. Nothing else.

Anything that *executes* against Agamemnon's API to converge actual → desired
state is consumer-side and lives here, where Agamemnon already runs.

This directory was ported from `HomericIntelligence/Myrmidons:scripts/` and
`HomericIntelligence/Myrmidons:tests/` on 2026-05-17. See the migration
manifest in ProjectAgamemnon#403 and the matching deletion PR in Myrmidons.

## Contents

| Path | Role |
| ---- | ---- |
| `scripts/apply.sh` | Reconcile actual → desired by calling Agamemnon's API |
| `scripts/plan.sh` | Dry-run: print diff (drift) between YAML and Agamemnon |
| `scripts/status.sh` | Table of desired vs actual + drift |
| `scripts/export.sh` | Bootstrap: pull current Agamemnon state down into YAML |
| `scripts/diff.sh`, `rollback.sh`, `new-agent.sh`, `doctor.sh` | Ops helpers |
| `scripts/lib/api.sh` | Agamemnon REST API client + URL validation + xtrace guards |
| `scripts/lib/reconcile.sh` | `compute_drift`, `verify_convergence`, prune logic |
| `scripts/lib/report.sh` | JSON / webhook reporting |
| `scripts/lib/prompt.sh` | TTY confirmation helpers |
| `scripts/lib/config.sh` | Env var defaults (AGAMEMNON_*, MYRMIDONS_*, etc.) |
| `scripts/lib/git-safety.sh`, `log.sh` | Shared infra |
| `tests/` | bats unit + integration tests, fixtures, mock server |
| `.github/workflows/apply.yml` | Auto-apply on merge (needs secrets re-pointed) |
| `.github/workflows/runner-health.yml` | Self-hosted runner health |
| `docs/adr/ADR-007-*.md` | Nomad integration strategy |
| `docs/adr/ADR-009-*.md` | `compute_drift` positional-parameter interface |

## How it consumes Myrmidons

The reconciler still reads YAML from `HomericIntelligence/Myrmidons`. It
expects `agents/<host>/*.yaml` and `fleets/*.yaml` to conform to the schemas
defined in that repo (`schemas/agent-v1.schema.json` etc.).

The recommended runtime pattern is for `apply.yml` (here) to clone Myrmidons
as a submodule or fetch-on-demand, then run `scripts/apply.sh` against the
checked-out dataset.

## Open work

See ProjectAgamemnon#403 for the migration manifest. ~83 reconciler-related
issues were closed in Myrmidons during the narrow-to-charter pass; triage
them and refile here as needed if they describe live bugs.

## Env vars

| Variable | Default | Description |
| -------- | ------- | ----------- |
| `AGAMEMNON_URL` | `http://localhost:8080` | ProjectAgamemnon base URL |
| `AGAMEMNON_API_KEY` | _(unset)_ | Bearer token / API key |
| `AGAMEMNON_TIMEOUT` | `10` | HTTP timeout (s) |
| `AGAMEMNON_CA_CERT` | _(unset)_ | PEM CA cert path |
| `AGAMEMNON_CLIENT_CERT` | _(unset)_ | PEM client cert (mTLS) |
| `AGAMEMNON_CLIENT_KEY` | _(unset)_ | PEM client key (mTLS) |
| `AGAMEMNON_TLS_VERIFY` | _(true)_ | Set `false` to skip TLS verify |
| `AIM_LOCK_FILE` | `.myrmidons.lock` | Apply lock file path |
| `HIBERNATE_SETTLE_SECONDS` | `2` | Wait after hibernate |
| `MYRMIDONS_DEFAULT_OWNER` | `$(whoami)` | Fallback owner during export |
| `MYRMIDONS_YES` | _(unset)_ | Skip interactive prompts |
| `SNAPSHOT_DIR` | `${REPO_ROOT}/.myrmidons/snapshots` | Pre-apply snapshot dir |

## Security notes

`AGAMEMNON_URL` is passed directly to `curl` — point only at trusted
Agamemnon instances. In CI, source from a secret.

`AGAMEMNON_API_KEY` must NEVER be committed. Use env vars or GitHub secrets.
