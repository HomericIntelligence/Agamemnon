# Third-Party SLAs and Outage Playbook

ProjectAgamemnon depends on four external services whose availability
directly bounds the availability of the orchestration mesh. This document
records the expected SLA for each, the failure mode Agamemnon sees, and
the recovery procedure operators should follow.

## SLA matrix

| Dependency               | Vendor SLA (target)      | Failure mode in Agamemnon                           | Auto-recovery?                  | Playbook |
|--------------------------|--------------------------|-----------------------------------------------------|---------------------------------|----------|
| GitHub Issues / Projects | 99.9% monthly uptime     | Task / agent state writes fail with HTTP 5xx        | Retry-with-backoff on 5xx/429   | [§1](#1-github-issuesprojects-outage) |
| NATS JetStream (via Keystone) | Best-effort (internal)  | `nats.c` reconnect loop; pub/sub stalls until reconnect | Yes — `natsOptions_SetReconnectWait` + JetStream redelivery on consumer reconnect | [§2](#2-nats-jetstream-outage) |
| Tailscale tailnet        | 99.9% monthly uptime     | Peer discovery scan (`100.x.x.x`) returns empty; cross-host NATS unreachable | Partial — reachable peers continue; isolated hosts stall | [§3](#3-tailscale-outage) |
| PyPI                     | 99.95% monthly (Fastly)  | `pip install` / wheel publish blocked               | Yes — Fastly mirrors; retry release later | [§4](#4-pypi-outage) |

Vendor SLA numbers reference the publicly advertised targets at the time
of writing (May 2026). They are **targets, not contractual guarantees**
for any of these free-tier services. Internal recovery procedures matter
more than the raw uptime number.

## Operational tolerances

| Component                 | Acceptable degraded duration | Hard breakage threshold |
|---------------------------|------------------------------|-------------------------|
| GitHub Issues writes      | < 5 minutes                  | > 15 minutes — escalate |
| NATS JetStream            | < 60 seconds reconnect       | > 5 minutes — escalate  |
| Tailscale peer discovery  | < 2 minutes                  | > 10 minutes — escalate |
| PyPI (release-time only)  | hours                        | > 24h — postpone release|

## Outage playbooks

### 1. GitHub Issues/Projects outage

**Symptoms.** REST handlers in `/v1/tasks` and `/v1/agents` return 502 /
503, or REST clients see `httpx.HTTPStatusError` from `MaestroClient`.
Daemon logs report `gh api ... HTTP 5xx`.

**Recovery.**

1. Check <https://www.githubstatus.com/> for an active incident.
2. The C++ store retries 5xx and 429 with exponential backoff up to the
   configured `gh_retry_max` ceiling (default 5). No operator action is
   required during the auto-retry window.
3. If the outage exceeds 15 minutes, **pause the work queue** by
   stopping new `myrmidon` agents from pulling: temporarily disable the
   pull consumers on `hi.myrmidon.{type}.>` via the NATS CLI:
   `nats consumer rm <stream> <consumer>` (re-create with the same
   config when GitHub recovers).
4. After recovery, validate state by walking `task.state.changed`
   events against the GitHub Issues backing store using
   `scripts/reconcile-task-state.sh` (see §reconciliation in
   `migration-from-keystone.md`).

### 2. NATS JetStream outage

**Symptoms.** `hi.tasks.>` / `hi.pipeline.>` publishes block, daemon
logs `nats: connection closed` / `nats: timeout`, myrmidons stop
receiving work.

**Recovery.**

1. The `nats.c` client auto-reconnects (configured wait =
   `nats_reconnect_wait_ms`). JetStream re-delivers any messages that
   were not acked before disconnect.
2. If the outage exceeds the consumer's `MaxAckWait`, redelivery may
   exceed `MaxDeliver` and messages will land in the JetStream DLQ.
   Inspect with `nats stream view <STREAM> --dlq`.
3. If the NATS server itself is gone, restart the Keystone-managed
   server (`systemctl restart keystone-nats` on the broker host). All
   ProjectAgamemnon-side reconnects then succeed automatically.

### 3. Tailscale outage

**Symptoms.** Peer discovery scan returns an empty set; cross-host NATS
publishes fail with `connection refused`; `tailscale status` reports
nodes offline.

**Recovery.**

1. Local agents on the same host continue functioning over the
   BlazingMQ + C++20 MessageBus path (intra-host transport is
   independent of Tailscale).
2. Cross-host work stalls. Once the Tailscale control plane recovers,
   re-authenticate any expired node keys with `tailscale up
   --auth-key=...`.
3. Long-lived outages: fall back to direct IP routing by setting the
   `AGAMEMNON_PEER_OVERRIDE` env var to a static peer list (`host:port,
   host:port,...`). Remove the override once Tailscale recovers.

### 4. PyPI outage

**Symptoms.** `python-client-release.yml` workflow fails at the
`Publish to PyPI` step; consumers cannot `pip install
HomericIntelligence-Agamemnon` or its companion client wheel.

**Recovery.**

1. PyPI's Fastly CDN typically recovers within hours. Re-run the failed
   release workflow once <https://status.python.org/> reports green.
2. If a tag has already been pushed but the workflow failed cleanly
   before upload, simply re-run the job (the workflow is idempotent on
   re-run — it re-builds and re-uploads).
3. If the upload partially succeeded (one wheel uploaded, others
   failed), bump the patch version with `scripts/bump-version.py` and
   re-tag rather than fighting PyPI's no-overwrite rule.

## Related documents

- `SECURITY.md` — transport security gaps and tracking issues
- `docs/api-versioning.md` — API compatibility commitments
- `docs/migration-from-keystone.md` — state reconciliation procedures
