# Durable epic registration

GitHub-backed Fleet uses the durable `agamemnon-epics` pull consumer on
`homeric-pipeline`, filtering `hi.pipeline.epic.*.registered`. It does not attach
the legacy Core epic callback at the same time. Memory-only legacy operation
retains its old callback; requesting `AGAMEMNON_DURABLE_EPICS=1` without GitHub
persistence fails startup. GitHub-backed Fleet cannot disable the durable path.
Missing API authentication is rejected before peer discovery or service startup.

The incoming contract matches Telemachy's existing `hi/v1` envelope: `msg_id`,
`epic {repo, issue, key}`, `children`, and `workflow`; optional `team_id` defaults
to `mesh`. The subject must match the epic key. The business identity hashes
ASCII-lowercased `owner/repo#issue`, matching GitHub's case-insensitive repository
identity. Only the repository field is normalized in registration and dispatch
metadata; workflow text remains case-sensitive. Replay with different repository
casing or a new message UUID reuses the brief and root. A changed
child set or workflow for that identity is a conflict requiring reconciliation.
This is registration, not a plan amendment API.

Earlier local versions derived root IDs from the original repository spelling.
A hydrated root for the same canonical repository and issue under a different
ID now stops registration with `epic_identity_migration_required` before any
new brief, graph, or dispatch. There is no automatic rename: parent references,
claims, and publication identities may already reference the old ID. Preserve
those records for explicit offline migration/reconciliation. Regenerate old
disposable local fixtures when appropriate. No live deployment or live GitHub
record migration was performed as part of this implementation.

The orchestrator confirms the brief and canonical root in GitHub before any
dispatch. Deterministic brief creation reconciles open and closed backing issues,
including a lost create response. Root `delivery` metadata records registration,
publication intent, and confirmed publication. The outgoing task uses only the
canonical `hi.myrmidon.pipeline.chief-architect.task.{task_id}` subject with
`schema: hi/v1`, stable `msg_id`, and `operation: decompose`. It is not dual-published
to the legacy task address in this path.

The consumer ACKs only after the callback completes: GitHub writes, JetStream
PubAck, and the GitHub publication receipt must all be confirmed. A failed write
or publish propagates. Publication retries reuse the message ID within a
conservative one-minute window. An older uncertain intent stops for reconciliation;
the code does not claim an atomic transaction across GitHub and JetStream or an
unlimited exactly-once guarantee. A single active controller remains required.

## Retry and retention

The required streams use file storage, Limits retention, no age expiry,
`DiscardNew`, and a deduplication window of at least two minutes. New absent Fleet
work streams receive those settings. Existing incompatible streams cause attachment
or strict publication to fail; the server does not migrate, purge, or replace them.
The default 50-MiB bound remains. A full stream rejects new publications, preserving
existing data. Approved capacity changes and safe archival are operational work.

The consumer uses explicit acknowledgments, a 30-second ACK deadline, one pending
message, and a persistent pull cursor. A processing heartbeat extends the deadline
while a callback runs. It is not a distributed writer lease.

Application processing is limited to three delivery attempts. Invalid input is
quarantined immediately; exhausted processing is also quarantined. The private
`hi.pipeline.quarantine.{consumer}` record contains a stable event ID, source
stream/sequence/subject, reason, and up to 64 KiB of source bytes as hex. Larger
payloads are marked truncated; the original remains in the source stream.
Quarantine is a transport failure disposition, not a canonical task failure or
successful registration.

The source receives TERM only after the quarantine PubAck. If quarantine is
unavailable, broker redelivery continues, including after restart, without another
application attempt once the limit is reached. Source data is retained. Logs
report the unconfirmed quarantine. Incompatible prior finite-MaxDeliver consumers
are rejected rather than silently changed. A full pipeline stream can block its
quarantine too; an operator must restore capacity without discarding unresolved work.

## Parent wakeups

A five-second reconciliation pass derives wakeups from already-durable canonical
child completion in a durable epic tree. It publishes a versioned `child_completed`
operation on the parent's existing role task subject, with a stable message ID and
a publication checkpoint on the child. Restart reuses that checkpoint. This does
not complete the parent, approve a PR, or infer completion from a worker activity
fact. Only parked, unassigned parents without Fleet claims are eligible in this
slice; active or uncertain parent ownership must first be reconciled.

## Reproducible validation and remaining gates

1. Build the existing focused target with `just fleet-native-build <nats-build>`.
   This uses cached libraries; it does not install dependencies.
2. Run `just fleet-jetstream-test <nats-server> <python>`. The harness creates a
   fresh loopback broker, random port, private storage, and bounded test process.
   It never uses a default broker or inherited broker URL.
3. Run the compiled contract suite through `just fleet-test`. It checks authority
   failure propagation, stable identity, restart hydration, uncertain publication,
   and canonical parent wakeups against GitHub fixtures.
4. Run `just fleet-startup-test <private-evidence-directory> <python>` to verify
   the actual server rejects missing API authentication before contacting even an
   isolated loopback sentinel.

Private broker tests exercise offline replay, explicit ACK, processing retry,
quarantine recovery after restart, incompatible retention/consumer rejection,
and the actual native epic-to-parent-wakeup path with a GitHub fixture. They do not
prove positive live GitHub admission, a planner executing the wakeup, or throughput.
Telemachy's legacy publisher uses Core publish plus flush and recreates issues on
retry. Its separate Fleet path now supplies issue-backed progress and real PubAck
for an existing reviewed epic under a single writer. The explicit cross-repository
contract below feeds actual producer bytes from a private broker through the
compiled native handler, including receipt-write loss, exact replay, and parent
wakeup. Legacy task lifecycle Core subscriptions are also outside this epic
consumer slice. No production
broker configuration or live issues were changed by these tests.

The implementation follows the [NATS consumer contract](https://docs.nats.io/learn/jetstream/pull-consumers)
and the installed nats.c headers for pull binding, explicit ACK, delayed NAK, TERM,
publication IDs, and synchronous PubAck.

## Producer-to-native contract

Build the fixture executable through `just fleet-epic-import-build`. Then, from
the standalone Telemachy checkout, run:

```bash
just fleet-native-contract /path/to/Agamemnon/build/fleet/fleet_epic_import \
  /path/to/nats-server /private/new-output-directory /path/to/python
```

The output directory must not exist. The driver starts a private loopback broker
and invokes the real Telemachy registration/publish functions. It verifies the
issue-backed outbox bytes against the broker, simulates a failed receipt write,
and checks that a fresh producer invocation reuses children and gets a duplicate
PubAck for identical bytes. The native fixture rejects an altered capture before
processing, rejects its first controlled GitHub write without dispatch or ACK,
and then processes and replays the actual producer body across controller
restart. A deliberately changed transport deduplication header exercises receiver
idempotence without waiting for the broker window; the body stays unchanged.
A controlled canonical child completion must wake its parent without completing
the parent. Captures and process output are written only by the executing test.
This does not provision a worker or establish live GitHub admission.
