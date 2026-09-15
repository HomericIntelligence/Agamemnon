# Fleet control integration

The Fleet slice extends the C++ Agamemnon server. Odysseus remains the web
interface; Hephaestus owns provider execution. GitHub issues labelled
`agamemnon-fleet` hold control records, commands, ownership, and control-event
history. They contain references to private inputs, not conversation content.

## Run and inspect

Use the normal `just build` server build and existing deployment entry point.
The service reads `GITHUB_TOKEN`, `GITHUB_REPO` (default
`HomericIntelligence/Agamemnon`), `AGAMEMNON_API_KEY`, `NATS_URL`, and `PORT`.
Clients use `Authorization: Bearer <key>` or `X-API-Key`.
`AGAMEMNON_BIND_ADDRESS` optionally selects the listen address; use `127.0.0.1`
for private local canaries. Its deployment default remains `0.0.0.0`.
An explicit `NATS_URL` is the sole connection endpoint; the NATS client never
tries a default broker before that configured endpoint.
Fleet requests return 503 when GitHub persistence is absent or hydration is
incomplete. Existing non-Fleet APIs retain their development memory mode.

The [OpenAPI extension](api/fleet.yaml) is referenced by the main API document.
The Python `AgamemnonClient` exposes `fleet_list`, `fleet_get`, `fleet_create`,
`fleet_command`, `fleet_acknowledge`, `fleet_observe`, and `fleet_events`.
The optional [Projects projection](fleet-projects.md) exposes board health and
reconciliation without changing task ownership or implementation state labels.

1. Create a pool with `{id, capacity}`.
2. Register a worker with `{id, poolId, capacity, host, allocationId?}`.
3. Create a session with `{id, workerId, agentId, workspace, taskId?, issueUrl?,
   domain?, hmasRole?, stage?}`. The controller copies host/allocation/pool linkage
   from the registered worker. A session receives a durable execution ID if omitted.
   A task-backed start requires an eligible, unassigned Pending HMAS leaf with
   completed dependencies. Previously delegated legacy work requires inventory
   reconciliation before adoption.
4. Submit a `start` command with `{commandId, idempotencyKey, generation, payload:{}}`.
   This creates a provider session without supplying a prompt.
5. After its command acknowledgment, submit `input` with exactly one `inputRef`
   or `promptRef`. Submit approval responses with `respond`, `responseRef`, and
   `requestId`. The trusted attachment adapter resolves private bodies before
   the Hephaestus call; private content never goes into GitHub or Keystone.
6. Read resources with `GET /v1/fleet/{kind}` (`{items,total}`) or
   `GET /v1/fleet/{kind}/{id}`. Kinds are `pools`, `workers`, `sessions`,
   `executions`, and `build-jobs`.

IDs are bounded ASCII identifiers. A command ID identifies one immutable intent.
Reusing it with changed payload, operation, target, or generation returns 409.
Normal commands cannot overtake an unacknowledged command. Stop requests fence
new input while their result remains uncertain.
An interrupt or cancel supersedes older pending controls. Its correlated stop
fact completes the stop command so a later explicit resume can renew admission.

## Import a Nestor research intake

The [system architecture](https://github.com/HomericIntelligence/Odysseus/blob/main/docs/architecture.md)
keeps Nestor research authority separate from the Agamemnon task graph.
Nestor owns research content and the canonical research issue. Agamemnon can
admit a confirmed intake as a standalone Pending L3 execution leaf. Import does
not research, create a second canonical work issue, create a TaskBrief, create a
Fleet session, or dispatch work. The HMAS backing issue is the existing durable
coordination record for that leaf.

An operator configures all three values: `AGAMEMNON_NESTOR_URL`,
`AGAMEMNON_NESTOR_API_KEY`, and `AGAMEMNON_NESTOR_NAMESPACE`. With all three absent,
the route returns 503. A partial or invalid configuration stops server startup
before peer discovery or service attachment. Enabled import requires GitHub
persistence and the existing authenticated API middleware. Keep the namespace
stable for the same Nestor authority; changing it creates a different identity.

The configured URL is an HTTPS origin with certificate and hostname verification,
without user information, a query, a fragment, or a base path. Plain HTTP is
limited to numeric loopback origins. The lookup uses the configured Nestor
credential, disables ambient proxies, refuses redirects, connects within two
seconds and limits the complete lookup to five seconds and 64 KiB. These bounds
apply to Nestor lookup; GitHub persistence uses the existing backing client.

1. Obtain the immutable `intakeId` and `requestDigest` from Nestor's intake receipt.
2. Send an authenticated `POST /v1/fleet/research-intakes` with exactly:

   ```json
   {
     "schema": "hi/agamemnon/research-import/v1",
     "intakeId": "research-01",
     "requestDigest": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
   }
   ```

   The example digest illustrates its format; use the actual digest from Nestor.
   The request is limited to 4096 UTF-8 bytes. Duplicate keys, extra fields and
   invalid identifiers are rejected. Do not send a URL, credential, repository,
   title, prompt, or research body.
3. A 201 response confirms a new durable Pending leaf; 200 is an exact replay and
   reports its current state. The metadata receipt contains the stable `taskId`,
   provenance, canonical issue reference and routing values `domain: research`,
   `hmasRole: task-agent`, `stage: research`.
4. After the separate worker and environment admission gates are satisfied, use
   the existing Fleet create/start/input/respond flow with that task and routing.
   Submit private content only through existing attachment references. Import
   itself never publishes to Keystone or grants worker admission.

The task ID is `research-` plus SHA-256 of canonical sorted JSON containing
`schema: hi/agamemnon/research-task-key/v1`, the configured namespace, and intake
ID. The request digest is verified provenance, not an alternate task identity.
Only the supported Nestor v1 generation-1 created record is accepted. Its issue
receipt confirms creation; it does not establish research completion or the
current live state of the canonical issue.

The Store serializes import against task mutations and scans all open and closed
HMAS backing records on each attempt. One open matching record is replayed;
closed, duplicate, malformed or changed identities return 409 for explicit
reconciliation. An uncertain creation returns 503. Retrying first rechecks the
durable records, so a committed write with a lost acknowledgement can be reused
without another create. A failed authoritative scan cannot confirm absence.

`delivery.researchIntake` retains only its schema, namespace, intake ID, request
and body digests, generation, attempt ID, canonical issue, and creation and
confirmation timestamps. Import uses generic task text and never copies the
research title or body. Later Store writes cannot replace that provenance or
its bound repository, issue, or leaf structure. Numeric types remain immutable;
a floating-point value cannot replace an integer in the provenance. Replay preserves current state,
assignment, claim, resolution, and unrelated delivery checkpoints.

Invalid input returns 400; absent intake returns 404; unconfirmed intake or a
provenance conflict returns 409. Disabled lookup, malformed/unavailable Nestor,
incomplete GitHub enumeration, or uncertain persistence returns 503 with a
non-sensitive error code. The API version header follows the existing API.

Run one controller only. GitHub does not provide a distributed compare-and-swap
lease for this import. Disable new import by removing all three configuration
values; existing tasks and Fleet controls remain durable. Do not delete backing
records or change the namespace to retry a conflict. This component does not
implement Nestor interviews, Telemachy workflow promotion, research-worker
isolation, or live model acceptance; those remain separate component gates.

## Worker and attachment contracts

Task-backed starts use
`hi.myrmidon.{domain}.{hmasRole}.task.{taskId}`. Interactive starts and subsequent
controls use `hi.fleet.control.{workerId}`. These are Keystone transport
boundaries, not a new scheduler. The command schema is `hi/fleet/v1` with
`commandId`, `idempotencyKey`, `generation`, `operation`, `targetKind`, `targetId`,
`workerId`, assignment metadata, and reference-only `payload`.

The attachment verifies `GET /v1/fleet/commands/{commandId}` immediately before
delivery. Its `{command,status,record}` response provides the durable original
intent and the current generation, worker, claim, and control ID. This endpoint
is an inspection gate and must not be polled as an alternate work queue.

Command receipts use `POST /v1/fleet/{kind}/{id}/ack` and include `schema`,
`eventId`, `workerId`, `commandId`, `generation`, `status`, and bounded `receipt`
metadata. The status is `accepted`, `completed`, or `failed` for the **command**.
It does not complete a turn or a GitHub issue. Provider IDs, private receipt
references, waiting reasons, drain flags, and stable error codes are permitted;
raw model output is not.

Activity uses `POST /v1/fleet/events` with `schema`, `eventId`, `workerId`,
`generation`, `sourceSequence`, `kind: activity`, `targetKind`, `targetId`, and
`event`. The event contains `activity`, `observedAt`, optional `stage`,
`waitingReason`, provider IDs, and a turn `outcome`. Activity values are
`model_working`, `tool_running`, `waiting_approval`, `waiting_input`, `idle`,
`disconnected`, and `unknown`.

The same facts enter through `hi.fleet.events.{workerId}`. Agamemnon returns
`{record,eventId,acknowledged:true}` on `hi.fleet.acks.{workerId}` only after
accepting the fact. A worker retains its journal until acknowledgment. Core
NATS publication is not, by itself, a durable acknowledgment.

Provider activity and turn outcomes do not complete canonical issues. An idle
turn retains the session claim. A cancellation or interruption releases its
active admission only after an idle stop outcome matches the current
`stopCommandId` through `event.commandId` and carries
`backgroundCleanup: confirmed_empty`. A missing or invalid cleanup marker
returns 409 and retains the claim. `background_cleanup_unconfirmed` observations
remain unknown and do not free capacity. Workspace retention remains the
worker's responsibility; no resource deletion is implied.
An interrupted conversation retains exclusive agent/workspace ownership while
freeing its active execution slot. Resume must reacquire capacity. Cancel is
terminal for that conversation; canonical issue ownership remains reserved
until an explicit orchestration decision.
Cleanup is accepted only on an idle observation matching the latest provider
turn acknowledged for input. Stop receipts do not replace that expected turn.
New input or resume clears prior cleanup evidence. Optional worker fact linkage
must match the canonical task, logical agent, session, execution, worker, and
generation before it can update activity.

## Canonical task ownership and manual resolution

HMAS records carry additive `fleet_claim` metadata with schema
`hi/fleet/claim/v1`, target kind/ID, worker, logical agent, workspace and generation.
The Store's HMAS collection lock serializes Fleet claims, dependency checks,
legacy mutations, and retryable hydration. Unrelated agent, team, task, fault,
and brief collections use separate locks. The canonical claim is acknowledged
by GitHub before the Fleet command intent is written.
Legacy assignment, state changes, completion and recreation cannot overwrite a
Fleet claim. A matching worker observation moves the task to InProgress.

Legacy splitting is serialized with Fleet admission. The parent links to every
planned child before any child issue is created. A failed child write leaves
those links for explicit reconciliation and prevents admitting the parent as an
independent leaf. Legacy completion returns a conflict for Fleet-owned tasks;
it cannot report successful completion after the canonical owner rejects it.

The two backing issues do not form a transaction. An uncertain write retains
ownership and invalidates the affected cache. Hydration includes closed records
and fails on malformed or incomplete HMAS/Fleet state. Retrying the same claim
can finish an interrupted admission without authorizing a second owner.

HMAS task creation and snapshot reads recheck hydration after acquiring the
collection lock. A request queued behind an uncertain write cannot create another
backing issue or return the old cached task state. It fails closed; a fresh request
must hydrate the durable records before it can proceed.

Manual resolution is disabled unless `AGAMEMNON_FLEET_RESOLUTION_KEY` is set.
`POST /v1/fleet/{kind}/{id}/resolve` requires the normal API credential and a
separate `X-Fleet-Resolution-Key` header. Do not give this operator credential to
workers or private attachment adapters. The request contains `decisionId`,
`generation`, `outcome` (`completed` or `failed`), `decision`
(`approve_completion` or `reject_completion`), `reviewerId`, and `evidenceRef`.

Resolution requires an observed inactive execution, current
`backgroundCleanup: confirmed_empty` evidence, no pending commands, and
the current canonical claim. Agamemnon's orchestrator persists the canonical
outcome before the Fleet admission is released. An identical decision can be
retried after an uncertain response. The record exposes `resolution.provenance: manual`
and `verifiedApproval: false`; a reference does not verify evidence.
These decisions do not count as independently approved acceptance work.
Autonomous resolution remains unavailable until Hephaestus review/check evidence
can be validated. The Python client provides `fleet_resolve` for the manual path.

## Persistence and observability

The single controller serializes admission, writes GitHub before publication,
and invalidates its cache after an uncertain write. The next operation must
hydrate again before deciding that capacity is available. Closed backing issues
are included in hydration; closing an issue is not claim release or archival.
GitHub issue creation and GraphQL POST operations make one transport attempt.
An ambiguous response returns to the owning operation for identity reconciliation;
the HTTP client never automatically repeats a possibly committed creation.

GitHub stores control transitions only. Frequent activity updates refresh the
read model without writing GitHub on each message. A control transition
checkpoints current observations. `GET /v1/fleet/events?after=N` returns ordered
durable control transitions with `{events,cursor}`. Worker activity has its own
`sourceSequence`; it is not part of that durable cursor. Reconnect clients read
resource snapshots and resume their Keystone activity stream separately.
Replayed lower source sequences are acknowledged with `superseded: true` without
changing the latest observation. Reusing the latest sequence with another event
ID fails. The attachment must obtain Agamemnon's fact acknowledgment in addition
to its Keystone publication acknowledgment before advancing its private journal.

Cold hydration preserves claims but marks observations unknown until fresh
worker reconciliation. The UI must distinguish persisted assignment, historical
observation, and current model/tool activity. `lastActivityAt` is the worker's
`observedAt`, not the time at which a record was assigned.

## Validation and release limits

GitHub-backed Fleet now requires the [durable epic transport](durable-epics.md).
Its private JetStream tests cover registration replay and canonical parent wakeups;
they do not establish positive live GitHub admission or planner execution.

`just fleet-test` builds focused native tests using installed or cached
dependencies from `CMAKE_PREFIX_PATH`; it does not run Conan or install packages.
The target compiles the production HTTP routes, store, GitHub client, and Fleet
service. `just fleet-client-test <python>` runs the HTTP client contracts using a
Python environment with the existing client dependencies.

`just fleet-native-build <existing-nats-build>` also links the real server at
`build/fleet/fleet_native_server`, using the specified cached nats.c library and
headers. It does not fetch dependencies or launch the server. This path supplements
the normal production build; it does not establish broker or provider readiness.

This slice is not the 108-agent acceptance demonstration. The following gates
remain explicit:

- A single active controller is required. Cross-host writer fencing/failover is
  not implemented; do not deploy competing writers against the same records.
- Record histories are bounded to keep GitHub issue bodies below their limit.
  Admission returns 507 before the reserved stop-confirmation headroom is used.
  There is no automatic archival or backing-issue deletion.
- Pool lifecycle, Slurm allocation/requeue reconciliation, generation replacement,
  and verified autonomous issue-stage completion are not implemented
  by this record API. They remain in their owning adapters/components.
- Production attachment, scoped worker authentication, scheduler execution,
  provider/account concurrency, and full-server deployment need their canaries.
  The focused HTTP tests use fake external boundaries and are not infrastructure
  evidence. No cluster workload or performance result is claimed here.

`just fleet-export /tmp/fleet-contracts.json` executes the bounded contract test
and exports actual API-produced start/input/interrupt/resume/cancel envelopes.
It uses fake external boundaries and is protocol evidence only.

`just fleet-import <grouped-worker-facts.json>` consumes a separately captured
`hi/fleet/bridge-fixture-facts/v1` artifact using the compiled FleetService.
It recreates the original durable commands with their exact execution identity,
accepts the captured sanitized worker facts, and checks unchanged replay and
retained canonical task ownership. Only an explicitly recorded fixture workspace
mapping may differ. This check uses an in-process GitHub test double; it does not
establish live GitHub persistence, provider authentication, or issue throughput.
