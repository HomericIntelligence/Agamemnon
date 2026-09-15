# Trusted Fleet attachment bridge

`python -m agamemnon.fleetd` connects one already-provisioned Keystone binding to
an existing Hephaestus worker. It does not create workers, claims, provider logins,
issues, or follow-up tasks. Agamemnon remains the orchestration authority.

The package exposes the installed `agamemnon-fleetd` entry point. From this
checkout, `just fleet-attachment-run --help` invokes the same implementation
without installing the package. Set `FLEET_ATTACHMENT_PYTHON` to an already
provisioned Python executable when the system Python is unsuitable.

The bridge uses only Python standard-library modules. It lives in the canonical
`agamemnon/src/agamemnon/fleetd/` package. Run it with Python 3.11 or newer, as the
orchestration agent contract requires. The existing Python manifest and type-check
configuration still list Python 3.9; their version alignment is a separate change.

## Delivery and confirmation

1. Read one admitted frame through `keystone-fleet-gateway`'s JSONL `pull` operation.
2. Read `GET /v1/fleet/commands/{commandId}`. Require the immutable command to match
   exactly and verify the record's target, owner, generation, and assignment.
3. For a new delivery, require command status `pending`, the current record's
   `commandId`, and a reserved or claimed session. Worker drain requires a durable
   draining record. A task-bearing start must use the exact canonical role/task
   subject; other controls use `hi.fleet.control.{workerId}`.
4. Resolve private input, copy authoritative top-level assignment fields into the
   worker payload, and reject conflicting nested values.
5. Flush a command digest to the private journal before sending one JSONL request
   through the worker's existing Unix socket. Renew the Keystone delivery while
   waiting for the bounded worker response.
6. Flush the sanitized worker receipt before publishing it to
   `hi.fleet.events.{workerId}`, with its original `eventId` as `Nats-Msg-Id`.
7. Confirm the identical fact through `POST /v1/fleet/events`. Require
   `acknowledged: true`, the exact event ID, and the current owner/generation record.
8. Acknowledge the Keystone delivery only after owner confirmation. Require the
   gateway to report broker confirmation of that acknowledgment.

Step 7 is an interim confirmation path. The current Agamemnon subscriber uses
Core NATS and cannot replay a fact missed while the controller is disconnected.
A JetStream publish acknowledgment alone cannot establish that Agamemnon applied
the fact. Keystone publication remains mandatory; the supported REST fact handler
confirms the same receipt idempotently. REST never supplies a work queue or dispatches
a task. Replace this extra confirmation only when durable owner acknowledgment is
implemented and verified end to end.

A successful command receipt does not complete an issue or release its claim.
`start` creates a conversation only. A separately admitted `input` starts or steers
a turn. `respond` answers a session-owned provider request. The bridge never
synthesizes a second command. Interrupt and cancellation outcomes remain the
worker's observed facts and Agamemnon's decisions.

## Private input contract

References are bare filenames matching `[0-9a-f]{32}.json`. They are not URLs,
filesystem paths, or `private:` URIs. The operator supplies one private spool
directory. The Odysseus backend writes each object with mode `0600` into that
owner-only directory, using a fresh random basename and exclusive creation.
Publish the reference only after the complete file is atomically available.
Treat the object as immutable for the lifetime of its command.

For `input`, put exactly one of `inputRef` or `promptRef` in the durable payload.
The private file has this shape:

```json
{
  "schema": "hi/fleet/private-input/v1",
  "commandId": "command-1",
  "workerId": "worker-1",
  "generation": 1,
  "sessionId": "session-1",
  "kind": "input",
  "text": "User-authored private input"
}
```

`promptRef` uses `kind: "prompt"` with the same fields. The body must match the
delivered command ID, runtime worker, generation, and target session exactly.
Unknown fields, duplicate JSON keys, empty text, and bodies above 128 KiB fail.

For `respond`, the durable payload contains `responseRef` and a string `requestId`.
The private body uses `kind: "response"` and replaces `text` with `requestId` and
`response`. The response has the provider's approved decision or answer shape:

```json
{
  "schema": "hi/fleet/private-input/v1",
  "commandId": "command-2",
  "workerId": "worker-1",
  "generation": 1,
  "sessionId": "session-1",
  "kind": "response",
  "requestId": 19,
  "response": {"decision": "accept"}
}
```

The body preserves the provider request ID's integer or string type. Its string
representation must match the durable metadata. Hephaestus verifies request
ownership and the supported response shape before answering the provider.

The bridge rejects directory and leaf symlinks, traversal, non-regular files,
hard-linked files, wrong owners, public permissions, and files changed during a
read. The spool and worker state directories must already exist and use canonical
absolute paths. The spool must be outside shared system scratch, including
`/tmp`, `/var/tmp`, macOS `/private/var/folders`, and the configured temporary
directory. This check applies when opening each private input as well as at
attachment startup. There is no CLI bypass. References resolve only in the
explicitly configured spool.
Before reading a private body, the bridge requires the admitted workspace to be
an existing canonical absolute directory. It rejects equal paths and either
direction of containment between that workspace and the spool.
No raw prompt, answer, request detail, or provider output enters the bridge journal,
Keystone publication, REST fact confirmation, or CLI status output.

Keep source workspaces, worker state, authentication, the private spool, and the
attachment journal in separate, non-overlapping directories. For one worker:

```text
/private/fleet/
  workspaces/agent-1/   source checkout and per-session .fleet-runtime/
  worker/              worker.sock and worker receipts
  auth/                private native provider authentication home
  spool/               immutable private command inputs
  attachment/          bridge delivery journal
  observations/        observation-only FIFO
```

The operator owns the private directories with mode `0700`. Source snapshots and
delivery artifacts must exclude each workspace's `.fleet-runtime` directory,
including session homes, caches, and temporary files. Authentication, worker state,
spool files, and attachment journals must never be included in a source snapshot
or mounted into another logical agent's workspace.

## Journal and recovery

Each attachment journal has one writer and binds to one worker generation. The
append-only `deliveries.jsonl` file has mode `0600`, a 64 MiB default limit, and
a 1 MiB record limit. It stores command and materialized-input digests, sanitized
receipts, publication/acknowledgment records, and pending activity facts/cursors.
It is execution evidence, not a second task database.

- If durable validation or private-input validation fails, send nothing to the worker.
- If an intent has no trustworthy worker receipt, retain it as uncertain. A reconnect
  must not resend it. The controller and worker inventory must establish an outcome
  through explicit reconciliation.
- If publication or owner confirmation fails after a receipt exists, retain the
  exact fact. Retry publication and confirmation without rerunning the worker or
  requiring the original private body to remain available.
- A cached receipt can be confirmed after claim release if exact intent and current
  owner/generation still match. This path cannot send a fresh worker command.
- Superseded commands are not executed. This initial bridge leaves their deliveries
  unacknowledged for explicit reconciliation.
- A full, corrupt, incomplete, or concurrently opened journal stops the attachment.
  Do not delete the journal or change its generation to make a retry proceed.

`forward_events()` reads one bounded page from the worker's private `events`
interface. It verifies contiguous worker-local sequence numbers and current target
ownership, validates timestamps and assignment identities, and removes unknown
fields. It journals each sanitized fact before publication. The cursor advances
only after both Keystone publication and owner confirmation. It does not infer
task completion or release capacity. Each worker event source needs one designated
collector; do not treat cursors from different journals as a global sequence.

The bridge preserves `backgroundCleanup: "confirmed_empty"` only when the worker
supplies that exact value, and preserves `waitingReason:
"background_cleanup_unconfirmed"`. Other cleanup values are omitted. These facts
allow Agamemnon to apply its release gate; the bridge never infers cleanup from
an idle activity or a completed command acknowledgment.

The configured command deadline is at most 300 seconds and defaults to 60.
Worker waits renew delivery progress every five seconds by default. Provision
the consumer's acknowledgment wait above the renewal interval, and leave enough
headroom for controller validation and attachment latency. A timeout preserves
uncertainty; it does not prove that a provider turn stopped.

Durable authority HTTP calls use one monotonic operation deadline through name
resolution, each TCP connection attempt, TLS handshake, request writes, response
headers, chunk metadata, and body reads. Numeric IP addresses connect directly.
Hostnames use an owned resolver process with only the host and port as command
arguments. The fixed helper inherits the parent's environment. The adapter closes
unsuccessful sockets and reaps that exact process. The
one-second resolver cleanup allowance cannot extend successful result eligibility.
TLS retains the standard verified SSL context and original hostname.

An expired authority read cannot authorize worker delivery or acknowledge work.
A timeout during fact confirmation retains the existing execution receipt for
reconciliation and replay. Local regression tests use finite loopback peers,
controlled resolver and connection delays, and actual TLS handshakes. These
fixtures do not qualify remote transport or production scheduling.

## Run a bounded canary

Run the bridge where its private spool and worker socket are accessible. Remote
SSH/Slurm staging, credential provisioning, and compute-node reachability remain
deployment responsibilities. The bridge does not submit a Slurm job or open a
worker listener. Its gateway argv comes from the trusted operator, never from a
work item.

```sh
python -m agamemnon.fleetd \
  --worker-id worker-1 --generation 1 \
  --spool-dir /private/fleet/spool \
  --journal-dir /private/fleet/attachment \
  --worker-state-dir /private/fleet/worker \
  --agamemnon-url http://127.0.0.1:8080 \
  --max-deliveries 1 \
  -- keystone-fleet-gateway \
  --stream homeric-myrmidon --consumer admitted-agent-1 \
  --subject hi.fleet.control.worker-1 \
  --publish-prefix hi.fleet.events.worker-1 \
  --worker-id worker-1 --generation 1
```

Use the repository's `just` entry point or the installed package's sanctioned
supervisor when running the command. The example is a process contract; it does
not represent a completed deployment. Supply `AGAMEMNON_API_KEY` privately in the
adapter environment when authentication is required. HTTP is allowed only for
loopback authority endpoints; remote authority reads require HTTPS. Redirects
are rejected. Gateway credentials follow Keystone's existing private environment
contract and must not appear in command arguments.

Command attachments do not collect activity automatically. Run `--events-only`
with one designated collector journal per worker to forward one activity page
without pulling work; all logical bindings sharing that worker use this same
collector. Reuse its journal across collector invocations so its cursor survives.
Gateway observation frames are drained and optionally forwarded through the
observation-only output below. They never confer command authority. The
Odysseus/Argus observation collector remains a separate reader at its supported seam.
One CLI invocation handles a bounded number of deliveries, default one; a supervisor
must make any further attachment/admission decision explicitly.

## Connect gateway observations to Odysseus

`--observations-fd N` enables a dedicated inherited pipe write descriptor, with
`N >= 3`. Output defaults off. Only an owner-owned anonymous pipe or an owner-only
named FIFO is accepted; regular files, sockets, and standard output/error are
rejected. The CLI takes ownership of that descriptor. It is not inherited by the
gateway child. The library `ObservationSink` owns a duplicate and leaves the
caller's descriptor lifetime to the caller. Nonblocking mode affects both copies;
use a dedicated descriptor, never one shared with another application protocol.

Wire one attachment to an Odysseus process as follows:

1. Choose an operator-owned private directory and create its mode-0600 FIFO. Run
   these once with the repository's command entry point:

   ```sh
   just --command mkdir -m 700 /private/fleet/observations
   just --command mkfifo -m 600 /private/fleet/observations/flow.fifo
   ```

2. In the Odysseus checkout, start its already-provisioned web application with
   that FIFO as observation input. The FIFO open waits for the writer in step 3:

   ```sh
   just --command node web/server/main.mjs --flow-stdin \
     < /private/fleet/observations/flow.fifo
   ```

3. In the Agamemnon checkout, add `--observations-fd 3` to the bounded attachment
   command above and redirect only descriptor 3 to the FIFO:

   ```sh
   just --command python -m agamemnon.fleetd \
     --worker-id worker-1 --generation 1 \
     --spool-dir /private/fleet/spool \
     --journal-dir /private/fleet/attachment \
     --worker-state-dir /private/fleet/worker \
     --agamemnon-url http://127.0.0.1:8080 \
     --max-deliveries 1 --observations-fd 3 \
     -- keystone-fleet-gateway \
     --stream homeric-myrmidon --consumer admitted-agent-1 \
     --subject hi.fleet.control.worker-1 \
     --publish-prefix hi.fleet.events.worker-1 \
     --worker-id worker-1 --generation 1 \
     3> /private/fleet/observations/flow.fifo
   ```

4. Keep both processes under the operator's supervisor. Capture the attachment's
   ordinary stdout separately for its final `observations` coverage record.
   Close unused inherited write descriptors so the reader receives EOF when its
   writer ends. EOF/disconnection remains uncertain coverage in Odysseus; it does
   not mean the full transport history was received. A new writer needs a fresh
   supervised reader attachment after EOF.

Use exactly one writer per pipe. Combining several attachments requires a
supervised relay that reads separate pipes and merges complete JSONL frames;
multiple writers must not share this FIFO. Such a relay consumes observations,
never work queues. An inherited anonymous pipe connected to Odysseus stdin is an
equivalent setup and avoids named FIFO open coordination. These wiring examples
do not provision the web application, credentials, or a live broker.

The sink re-selects bounded gateway identifier fields, route/operation/result
metadata, timestamps, and measured `bytes` for publish/delivery. It preserves
`sourceId`, `sourceSequence`, and `eventId` without renumbering. Raw payloads,
prompts, tokens, nested objects, error text, and unknown fields are excluded.
It forwards the existing `hi/keystone/fleet-attach/v1` observation frame directly;
it never recursively publishes through the gateway response loop.

The queue is limited to 64 frames and 64 KiB of remaining output, with at most
eight nonblocking write attempts per flush. A partial write keeps its remaining
bytes ahead of later frames. Full queues drop new observations; broken pipes
discard pending observations. Neither case blocks a control request, changes
its response, nor triggers a work acknowledgment. Close attempts one bounded
flush and counts unsent frames as dropped, without waiting for a slow reader.

The final coverage record reports observed frames `seen`, complete frames
`written` to the OS pipe, `dropped`, invalid frames, and queued frames/bytes.
These are observation-delivery counters, not packet counts or inferred broker
messages. `written` does not prove that Odysseus consumed a frame. A dropped
sequence can produce a visible gap; a lost final tail may have no later sequence
to expose it. `historyComplete` therefore always remains false. Shutdown can
leave a truncated final JSON line, which the reader must mark uncertain rather
than combine with a future attachment's output.

## Verification and limits

The focused tests use real subprocess pipes, Unix sockets, and a private loopback
HTTP server, with controlled gateway, worker, and controller fixtures. They cover
failed durable reads, wrong owners/generations, unsafe private inputs, request ID
types, metadata redaction, uncertainty, publication/owner downtime, replay after
claim release, journal corruption, cursor gaps, and bounded worker waits.

Run the focused package tests in an already provisioned Python environment:

```sh
just fleet-attachment-test /absolute/path/to/python
```

Tests allocate and clean private spool fixtures under `~/.codex/fleetd-tests`;
shared pytest scratch is used only for non-spool fixtures and rejection cases.

The equivalent explicit command is:

```sh
PYTHONPATH=agamemnon/src just --command python -m pytest \
  agamemnon/tests/test_fleetd_attachment.py \
  agamemnon/tests/test_fleetd_observations.py -q
```

No native Codex authentication, real NATS broker, SSH/Teleport connection, Slurm
allocation, workspace sandbox, or 108-agent performance result is established by
these fixtures. Controller single-writer operation and effective generation
fencing remain required: a durable read followed by socket delivery is not a
transaction across the controller and worker.
