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
before peer discovery or service attachment. Enabled import also requires the
shared repository registry and import-state branch described below, GitHub
persistence, and the existing authenticated API middleware. Keep the namespace
stable for the same Nestor authority; changing it creates a different identity.

The configured URL is an HTTPS origin with certificate and hostname verification,
without user information, a query, a fragment, or a base path. Plain HTTP is
limited to numeric loopback origins. The lookup uses the configured Nestor
credential, disables ambient proxies, refuses redirects, connects within two
seconds and limits the complete lookup to five seconds and 64 KiB. These bounds
apply to Nestor lookup. The subsequent GitHub operations and Store lock acquisition
share a separate 30-second budget, for a declared sequence of at most 35 seconds.

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

The Store serializes both intake kinds against HMAS task mutations and scans all
open and closed HMAS backing records on each attempt. Import waits for the HMAS
lock within its existing GitHub deadline. Agents, teams, legacy tasks, faults,
and task briefs use separate collection locks and can proceed during import. One valid open matching backing
record is replayed. Closed, duplicate, malformed or changed identities require
reconciliation. A direct-import task for the same canonical work issue returns
409 `work_issue_already_imported`; it is never converted into research provenance.
The [shared recovery contract](#shared-import-configuration-and-recovery) defines
the optional canonical task reference on a verified conflict.
The shared conditional attempt record described below prevents a retry from
creating a replacement while an earlier create remains uncertain.

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

Disable the research route by removing all three Nestor configuration values.
Retain the shared registry and attempt namespace for guards and reconciliation;
removing them does not permit generic work acquisition. Do not delete backing
records or change the namespace to retry a conflict. This component does not
implement Nestor interviews, Telemachy workflow promotion, research-worker
isolation, or live model acceptance; those remain separate component gates.

## Import a planned GitHub issue

Direct issue import uses an operator-registered GitHub repository and an explicit
issue-body or issue-comment reference. It is available independently of Nestor.
Agamemnon owns the resulting Pending L3 task and implementation routing; it does
not approve a plan, copy its content, change the canonical work issue, or dispatch
execution. Odysseus projects these supported APIs without becoming another task
authority.

1. Read authenticated `GET /v1/fleet/issue-intakes/repositories`. Select one
   returned `key` with its case-sensitive canonical `repository` and native
   `repositoryId`. This is configuration projection, not a fresh GitHub check.
2. Read `GET /v1/fleet/issue-intakes/{key}/{number}`. With no query, inspection
   selects the exact current issue body. The sole optional query parameter is
   `planCommentId`, an explicit native issue-comment ID. Inspection verifies that
   the comment belongs to the selected issue. It returns native identities,
   title, canonical issue URL, open/closed state, the selected `plan` reference,
   and `observedAt`. It returns no issue or comment body.
3. Retain the exact selection and send authenticated
   `POST /v1/fleet/issue-intakes` with exactly `schema`, `repositoryKey`,
   `issueNumber`, `repositoryId`, `issueId`, and `plan`. The schema is
   `hi/agamemnon/issue-import/v1`. The plan is either
   `{kind: "issue_body", digest}` or
   `{kind: "issue_comment", nodeId, digest}`. Use the actual inspected values;
   there are no caller-controlled URLs, credentials, routing or timestamps.
4. A 201 response acknowledges one new unassigned Pending leaf. A 200 response
   replays its current canonical state with the original provenance. Receipt
   fields are `schema`, `taskId`, `state`, `provenance`, `issue`, and `routing`.
   The receipt schema is `hi/agamemnon/issue-import-receipt/v1`; fixed routing is
   `domain: pipeline`, `hmasRole: task-agent`, `stage: implementation`.
5. Separately qualify a worker and use the existing Fleet task-backed start flow.
   Import creates no session, allocation, task claim, TaskBrief or NATS message.

Registry reads and POST accept no query parameters. Inspection accepts at most
one `planCommentId`; digests never belong in URLs. Requests reject duplicate JSON
keys, unknown fields, invalid UTF-8 and non-integer issue numbers. POST is limited
to 4096 raw UTF-8 bytes; native IDs are nonempty and at most 128 UTF-8 bytes;
issue numbers are 1 through 2147483647. Selected content must be nonempty valid
UTF-8 and at most 128 KiB. Its SHA-256 covers the exact bytes, with no whitespace
or newline normalization. `observedAt` records observation, not approval. An
edit followed by an exact content revert has the same content digest.

The task key is `issue-` plus SHA-256 over compact, lexically sorted UTF-8 JSON
with no trailing newline containing `forge: github`, `repositoryId`, `issueId`,
and `schema: hi/agamemnon/issue-task-key/v1`. Registry selectors, plan digests and
timestamps are not key inputs. `delivery.issueIntake` retains only its schema,
forge, native identities, canonical issue reference, plan reference, fixed
routing and original observation timestamp. All of these are immutable. Replay
preserves state, claim, assignment, resolution and delivery checkpoints; it does
not revive completed work. An unchanged existing task can replay after the work
issue closes, but a first import requires an open work issue and open backing
record. A research owner returns 409 `work_issue_already_imported`, not a direct
receipt with rewritten provenance.
The [shared recovery contract](#shared-import-configuration-and-recovery) defines
the optional canonical task reference on a verified conflict.

Invalid selection, an unknown registry key, or a supplied repository ID that
disagrees with that registry entry returns 400 before GitHub I/O. A missing work
issue returns 404. A changed actual issue identity or selected content, a closed
work issue before first import, and ownership conflicts return 409. Oversized
POST returns 413. Disabled configuration, bounded upstream failure or uncertain
persistence returns 503. Retry only the same retained reference; inspection,
status reads and browser reload do not themselves import.

## Shared import configuration and recovery

Both intake paths and generic work-acquisition guards use one operator registry
and conditional attempt namespace in the existing GitHub backing repository.
Set `AGAMEMNON_ISSUE_INTAKE_CONFIG` to a private owned regular file with mode 0400
or 0600, at most 64 KiB, without symlink ancestors, a symlink final component,
hard links or descriptor aliases. The closed JSON wrapper has exactly `schema`
and `repositories`:

```json
{
  "schema": "hi/agamemnon/issue-intake-config/v1",
  "repositories": [
    {"key": "implementation", "repository": "Example/Project", "repositoryId": "R_example"}
  ]
}
```

The example native ID is illustrative; verify and supply the actual GitHub
repository identity. Entries are unique by key, native ID and case-insensitive
repository name, with 1 through 64 entries. Keys use lower-case ASCII letters,
digits, `_` and `-`, begin with a letter or digit, and have at most 64 characters.
Repository names are canonical ASCII `owner/name`, at most 255 bytes, without
consecutive dots or a `.` component. Renames and native-ID changes require
explicit operator reconciliation rather than implicit alias adoption.

Set `AGAMEMNON_IMPORT_STATE_BRANCH` to an existing branch in `GITHUB_REPO`.
The service does not create that branch. The existing GitHub credential needs
read access to registered work issue/comment metadata and read/write access to
the backing issues and that branch's contents. Never put credentials in the
registry or commit its operator file. Both variables absent with research
disabled leaves direct import disabled; partial, empty, malformed, or missing
configuration with research enabled stops startup before external setup.
Configured import requires durable persistence and API authentication.

For a new import, Store writes a bounded `prepared` intent, conditionally moves
that same blob to `creating`, then attempts one backing-issue POST. Each contents
write needs its exact conditional acknowledgment and same-branch readback under
the shared budget. Only the invocation that confirmed its own preparation may
grant creation. A retained `prepared` or `creating` record permits observation
and reconciliation, not another grant. This includes a lost prepare or grant
acknowledgment. A unique valid task that later becomes visible is canonical even
if the auxiliary `linked` write was uncertain. V1 provides no reset, deletion or
automatic recovery transition for a conclusively unstarted retained attempt.

An empty scan, timeout, client disconnect or controller restart cannot disprove
an earlier possible POST. Before enabling this version, positively reconcile
pre-upgrade operations with a unique exact backing task or authoritative evidence
that creation was never dispatched or was definitively rejected. Quiescing the
old importer alone is insufficient. Keep imports disabled for an uncertain
backing namespace; do not establish fresh fences as a substitute for that proof.

Run one active controller. The SHA-conditional record is import creation
metadata, not a distributed controller lease, task claim, scheduler or queue.
Generic create, work-identity retarget, delivery replacement and split paths
check retained ownership and the shared attempt namespace before acquiring work.
Missing configuration or failed reconciliation refuses those mutations, even
when import routes are disabled. Nonconflicting memory-mode operations and
existing same-identity reads/state-only updates retain their existing behavior.
Valid preexisting research/direct tasks replay without rewriting provenance or
requiring a new attempt record.

After a complete supported history scan validates one retained task for the
resolved work issue, a 409 conflict can include an optional `canonical`
reference. Its `hi/agamemnon/import-conflict-reference/v1` schema carries the
exact task ID, native repository and issue IDs, canonical work-issue reference,
and the HMAS backing issue's `backingState` (`open` or `closed`). Legacy task IDs
remain unchanged. The reference supplies reconciliation metadata and grants no
execution permission. It does not assert a successful import, imported provenance,
current task state or live worker claim. A closed record remains a conflict.
Ambiguous, malformed or incomplete history and an attempt record without a
verified task supply no reference. Conflict handling does not write a replacement
task, create a new attempt or dispatch work.

HMAS hydration and ownership scans reject duplicate JSON member names, including
escaped spellings and nested members, before any fence or task write. Malformed
retained bodies require reconciliation and are not rewritten or skipped. Splits
check all proposed child IDs and resolved canonical work identities together
before writing the parent or any child. Distinct work issues and unassigned
legacy children with issue number zero remain supported.

Typed `issueIntake` and `researchIntake` records require an explicit recognized
`state` before hydration or a Fleet claim. A missing state is invalid; it cannot
default to `Pending`. Existing defaults for untyped legacy records remain unchanged.

The fixed GitHub import client uses normal HTTPS verification, no redirects,
ambient proxy or automatic retry. Its runtime must expose libcurl asynchronous
DNS (`CURL_VERSION_ASYNCHDNS`); otherwise it returns unavailable before I/O.
The 30-second monotonic budget includes Store lock acquisition, source lookup,
complete enumeration, conditional writes/readback, and the possible one POST.
Each connection uses at most one second or the remaining budget. Enumeration
uses all states, ten records per page, at most 256 records and 27 requests with
explicit terminal-page proof. Each compact complete REST record is at most
512 KiB, each actual response body at most 8 MiB, and aggregate response bodies
at most 144 MiB. Attempt records are at most 16 KiB. Larger otherwise valid
histories return unavailable and remain unchanged; these are supported read
limits, not a new universal HMAS storage limit. Incomplete pagination is never
absence. Existing generic GitHub methods are not claimed bounded by this profile.

## Subordinate build jobs

A subordinate build references an active parent session or execution and its
canonical HMAS claim. It owns a separate tool allocation and immutable snapshot
workspace. It never acquires or releases the parent issue-writer claim and never
creates a provider worker or another agent slot. Existing generic build-job
records retain their old API behavior; the versioned `build` discriminator
selects the subordinate protocol.

An operator must register the exact parent workspace/repository, snapshot policy,
fixed recipe and independently qualified allocation before admission. The first
recipe is `hephaestus-test-unit-v1`, with exactly `just test-unit` and empty
parameters. Its policy binds recipe/lock digests, Linux aarch64 platform,
immutable image/toolchain digests, CPU/memory/disk/wall/output/artifact bounds and
zero GPUs. Separate tool capacity must include the supervisor reserve; provider
worker capacity is never reused. A registered policy describes qualified
capacity; loading the policy does not allocate resources or qualify a runtime.

Provider and tool capacity conflict when their worker IDs match or a provider's
explicit `allocationId` matches the tool allocation ID. Different workers,
hosts or generations do not make the same allocation separate capacity.
Registration, new provider start/resume, pending start/resume republication,
build admission, initial build redelivery and new run grants check the current
worker inventory. Legacy provider records may omit `allocationId` or use null;
the worker-ID check still applies.

`AGAMEMNON_FLEET_BUILD_CONFIG` selects a private operator file with the closed
`hi/fleet/build-configuration/v1` wrapper: `schema`, `catalog`, and `authorities`.
With the variable absent, new admission is disabled. A configured file requires
GitHub-backed persistence and API authentication, is limited to 1 MiB, and must
be an owned regular file with mode 0400 or 0600. Do not place this file in source
control. The catalog has `schema: hi/fleet/build-catalog/v1`, `workspaces`,
`recipes`, and `allocations`. Authorities use
`schema: hi/fleet/build-authorities/v1` and a closed `authorities` array; each
entry binds its ID and private key to a tool worker, allocation and generation.
Keys are never copied into public policy, GitHub records or Keystone commands.

Set `AGAMEMNON_FLEET_BUILD_STATE_BRANCH` to an existing branch in `GITHUB_REPO`.
Enabled build admission requires this separate setting. The service does not
create the branch. The backing credential needs read/write access to its
contents. Keep the branch setting when the catalog is disabled or its file is
removed. Recovery and provider-capacity checks still read the retained attempt.
Removing or changing the recovery namespace before every attempt is resolved is
unsupported. Removing both settings cannot serve as a release operation.
Existing deployments that have never enabled builds can retain the default
construction without a build state branch.

The fixed path `fleet/build-admission/current.json` holds one bounded
`hi/fleet/build-create-attempt/v1` record. It binds a unique attempt ID to the
build ID, selected worker/allocation/generation, and digests of the immutable
request, policy, parent claim binding, and first command. It contains no worker
credential. It is a GitHub-backed creation barrier, not another task queue.

Before one issue POST, the current invocation must confirm a conditional
`creating` write and exact same-branch readback. A retained attempt never grants
another POST. A lost acknowledgment, empty listing, restart, or new parent
observation cannot clear it. While creation is unresolved, all new build
admissions stop. The selected worker and allocation also remain unavailable to
provider admission when the catalog is disabled. Other admitted work can
continue through its existing controls.

After the original canonical issue is visible, reconciliation validates its
complete lifecycle document and the same immutable admission fields. A valid
cancellation, grant, or terminal fact does not change those fields. An exact
match can conditionally mark the attempt `linked`; it cannot publish or acquire
a parent claim. A later creation can replace that linked barrier only after the
prior issue is present and valid. Active capacity then remains reserved by the
canonical issue. Malformed, conflicting, duplicate, or absent records preserve
uncertainty. There is no automatic reset for a conclusively unstarted retained
attempt in this version.

Policy, parameter and grant digests use SHA-256 over compact, sorted-key UTF-8
JSON with no trailing newline. The owning Hephaestus snapshot contract separately
defines its canonical manifest with one trailing newline. The controller checks
the six snapshot commitment fields; it does not read or verify snapshot bytes.

1. Produce and independently verify an immutable source snapshot through the
   owning Hephaestus snapshot service. Export includes eligible dirty and
   untracked source under a registered policy; a clean commit alone is not a
   substitute. Preserve the snapshot reference, manifest digest, base commit,
   member count, total file bytes and policy digest.
2. Send `POST /v1/fleet/build-jobs/submit` with
   `schema: hi/fleet/build-submit/v1`, `workspaceId`, `recipeId`,
   `parameters: {}`, `idempotencyKey`, `parent`, and `snapshot`.
   The closed parent binding contains `targetKind`, `targetId`, `sessionId`,
   `executionId`, and integer `generation`. The closed snapshot commitment
   contains `reference`, `manifestDigest`, `baseCommit`, positive integer
   `members`, positive integer `bytes`, and `policyDigest`. References are opaque
   IDs; admission never opens them as a path or URL.
3. Read the returned `{record, command}` and retain the deterministic build ID.
   Admission confirms its creation barrier, acknowledges the durable issue write,
   and confirms the link before it publishes the fixed command through Keystone.
   A response lost after a write is uncertain;
   replay the identical submission. Exact replay returns the retained job and
   can confirm its barrier link. It does not create another reservation, select
   another policy, or publish a command. Changed
   requests or colliding command IDs return 409.
4. If initial command publication is uncertain, use the separate `/deliver`
   operation with the exact admitted command ID, generation and attempt.
   Delivery checks the current parent and retained initial command. Status and
   submission replay do not themselves authorize delivery or process start.
5. The trusted tool supervisor verifies the actual snapshot and recipe bytes,
   then sends `/claim-run` with its exact claim and additional
   `X-Fleet-Build-Key`. The durable run grant is the controller authorization
   point. Parent stop and grant are serialized by the single controller: stop
   first prevents a new grant; a grant persisted first remains valid for that
   recorded generation. This is not an atomic remote spawn transaction. A lost
   grant response requires replay of the same claim, including after parent
   stop, rather than another claim or process attempt.
6. Cancel through `POST /v1/fleet/build-jobs/{id}/cancel` with
   `schema: hi/fleet/build-cancel/v1`, `commandId`, `idempotencyKey`,
   `generation`, and `attempt`. This journals a child stop before publication;
   its acknowledgment is not cleanup proof. Exact replay does not write another
   control transition; it may redeliver the same pending stop command.
7. The trusted supervisor sends an exact `/facts` terminal record with the
   additional supervisor key, binding the worker/allocation/generation, attempt,
   command, policy/parameters/snapshot digests, runtime identities, outcome and
   owned cleanup. Cancellation must fence a never-started command before
   confirming empty. Only a matching terminal fact with confirmed cleanup
   releases child capacity. The parent claim remains unchanged. Generic ACK,
   activity and manual-resolution routes cannot complete a typed subordinate
   build.

Only one attempt is supported in this first protocol. Generation replacement or
retry is not an implicit new attempt. A later parent generation cannot adopt an
earlier result. Startup hydration validates the retained typed command, policy,
grant, cancellation and compact history before replay, and marks parent
observations unknown. Corrupted typed records, including an unknown outer
resource kind, return 503 without dispatch or persistence writes.
Reconciliation must restore current parent observations
before a new grant or redelivery. With the admission catalog removed, retained
jobs can still be read, cancelled and reconciled using the same separately
configured recovery authority. A missing recovery key preserves uncertainty and
reservation; it does not authorize a substitute fact.

An overlapping allocation discovered after restart blocks fresh authorization
and delivery while leaving inspection, exact historical replay, cancellation
and confirmed cleanup available. A retained run grant remains replayable with
the same claim. Tool capacity remains unavailable to provider registration and
activation until the allocation leaves the catalog and every retained build on
that capacity has confirmed release; catalog removal alone is insufficient.

### Private log pages and receipt references

`GET /v1/fleet/build-jobs/{id}/logs?stream=stdout&after=0&limit=65536` reads from
the configured local artifact backend. `stream` is `stdout` or `stderr`,
`after` is an integer from 0 through 2^63-1, and `limit` is from 1 through 65536.
Repeated or unknown query fields are rejected. The requester cannot select a
backend URL, credential or filesystem path.

`AGAMEMNON_FLEET_BUILD_ARTIFACTS` selects a separate private operator file. It
uses the same 1 MiB, ownership, mode and no-link checks as the admission file,
and requires durable persistence and API authentication. Its closed document
contains `schema: hi/fleet/build-artifacts/v1`, `origin`, and the dedicated `key`.
An absent variable or an empty object disables log reads. Invalid configuration
stops startup before external services; loading the file makes no connection.

The first backend profile accepts only canonical literal
`http://127.0.0.1:<port>` or `http://[::1]:<port>` origins with explicit ports
1 through 65535. DNS names, other addresses, HTTPS, URL credentials, paths,
queries and fragments are rejected before connecting. Redirects and inherited
proxies are disabled. The local read uses its dedicated backend key, a 500 ms
connection limit, a 1 s total limit and a 400,000-byte encoded response ceiling.
Remote artifact backends are not enabled by this profile and require a separate
qualified interface.

The returned `hi/fleet/build-logs/v1` object binds `buildId`, `attempt`,
`snapshotDigest`, `stream`, `after`, `next`, `data`, `chunkDigest`, `complete`,
`truncated`, and `manifest`. Data is a UTF-8 string; offsets and page limits count
its actual UTF-8 bytes. The digest hashes those bytes. A complete page requires
an immutable `{reference,digest}` manifest. The controller rechecks any retained
terminal log manifest after the bounded backend read. Malformed, duplicate,
oversized or mismatched responses fail without a GitHub write. An ahead cursor
returns 409; absent or unavailable backend returns 503.

Log bytes, cursor indices and chunks remain in the private artifact service.
The durable build stores only bounded terminal receipt/log/artifact references
and digests. It never becomes a log index. The admission intent is limited to
30,000 serialized bytes before its first transition. Nonterminal persistence
reserves space below 45,000 bytes,
and final cancellation/cleanup remains below the existing 60,000-byte body cap.
Oversized optional evidence is rejected before writes. Missing evidence remains
`incomplete`; present references remain `unverified`. `collectionVerified` stays
false because a digest string is not independent verification of artifact bytes.
The separate collector must verify complete bytes and fresh source identity;
stale source cannot be reported as the current checkout's result.

The Python client exposes `fleet_build_submit`, `fleet_build_status`,
`fleet_build_deliver`, `fleet_build_claim_run`, `fleet_build_cancel`,
`fleet_build_fact`, and `fleet_build_logs`. These seven methods apply the finite
`AgamemnonConfig.timeout` to the whole request, including streamed response reads,
and cap identity-encoded response bodies at 512 KiB. They reject other content
encodings and redirects, perform no automatic retry, and close the response on
completion, failure or cancellation. Normal API authentication and the separate
supervisor header use the same client.

Service bridges must construct `AgamemnonClient(config, trust_env=False)` so that
proxy and other HTTPX environment settings cannot select their transport. The
keyword-only option defaults to `True` for existing client callers. The client
uses its configured HTTP host and port; it does not establish remote TLS or
qualify an operational transport. Required Fleet MCP operations use the same
controller service through their owning Hephaestus adapter. The controller
contains no second scheduler or local process executor.

`just fleet-build-export <private-output>` executes the registered export test
and writes actual controller-produced start/grant/cancel/terminal wire data.
`FLEET_BUILD_SNAPSHOT_INPUT` may select the separate bounded snapshot commitment
fixture. This export uses synthetic external boundaries and does not execute a
recipe, qualify an allocation or establish complete offload acceptance.

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
Subordinate build creation additionally uses the conditional barrier above;
an empty hydration result alone cannot release its uncertain reservation.

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
