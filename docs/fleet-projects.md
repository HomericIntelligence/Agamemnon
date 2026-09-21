# Fleet GitHub Projects projection

Agamemnon rebuilds an explicitly configured ProjectV2 view from its durable
HMAS issue records. Board edits never authorize work, alter task state, or
approve implementation. The projector does not write Hephaestus labels.

Each board item links to the Agamemnon issue that owns the canonical task.
Health metadata retains the work repository and issue number, the work issue
URL, and pull requests actually linked to that issue through GitHub's API.
An optional text field displays these work links directly on the board.

## Configuration

Projection is disabled unless `AGAMEMNON_PROJECTS_CONFIG` names an operator-owned
JSON file. The service uses its existing backend `GITHUB_TOKEN`; credentials
remain outside this file. Missing GitHub persistence reports `unavailable`.
Malformed configuration reports `misconfigured` and performs no mutations.
No project, field, option, or credentials are created by this integration.

| JSON field | Required value |
| --- | --- |
| `schema` | `hi/projects-projection/v1` |
| `projectId` | Existing ProjectV2 node ID |
| `stateFieldId` | Existing single-select field for orchestration state |
| `stateOptions` | Object mapping each canonical state to its actual option ID |
| `stageFieldId` | Optional, separate existing single-select field |
| `stageOptions` | Required with `stageFieldId`; exact `state:*` labels to option IDs |
| `workLinksFieldId` | Optional, separate existing text field |

The seven required `stateOptions` keys are `Pending`, `Decomposing`, `Delegated`,
`InProgress`, `Escalated`, `Completed`, and `Failed`. IDs must come from the
operator's project. They are validated against that project's fields and options
before any mutation. Field IDs must be distinct. Configure a dedicated
orchestration field so it cannot be confused with the Hephaestus stage field.

Follow GitHub's [Projects API guide](https://docs.github.com/en/issues/planning-and-tracking-with-projects/automating-your-project/using-the-api-to-manage-projects)
to retrieve IDs and establish the appropriate Projects permissions for the
backend identity. The implementation uses the documented [ProjectV2 queries
and mutations](https://docs.github.com/en/graphql/reference/projects), with
GraphQL variables and acknowledged responses. No live project was modified
during implementation or local verification.

## Reconciliation and health

With valid configuration and GitHub persistence, the server reconciles at
startup and every 300 seconds. `GET /v1/fleet/projects` returns current health;
`POST /v1/fleet/projects/reconcile` requests an immediate rebuild. Both use
the normal Agamemnon API authentication. A concurrent rebuild returns the
current running status. Python clients expose `fleet_projects()` and
`fleet_reconcile_projects()`.

Every pass reads canonical task bodies, including closed backing issues, and
enumerates the current project before adding missing items or changing mapped
fields. Repeated runs and restarts do not repeat acknowledged values. If a
response is lost after a write, the next pass observes the committed value.
External edits to mapped state fields are repaired from issue records.
Unrelated fields and items are left intact. Archived matching items require
operator reconciliation; they are not silently recreated or unarchived.

Stage projection uses the work issue's actual `state:*` labels. Exactly one
configured label is required. A confirmed missing, ambiguous, or unmapped label
clears the derived stage field and reports `stageProjection: unavailable`.
An unavailable read retains the last board value and exposes unavailable
metadata until it can be refreshed. Work and PR links use actual API data;
the projector does not infer PRs from branch names or task assignments.

Health states are `disabled`, `pending`, `running`, `healthy`, `degraded`,
`misconfigured`, and `unavailable`. Results expose the attempt and last-success
timestamps, changed/unchanged/failed item counts, unavailable metadata count,
retryability, and per-task links and stage availability. Missing counters
before the first attempt do not mean zero measured work. Projection counts
never count as completed or independently approved issue work.

Projection errors cannot roll back canonical writes or allow memory-only
dispatch. There is no scheduler or task queue in the projector. Transport,
HTTP, partial GraphQL errors, and invalid mutation acknowledgments remain
retryable failures with bounded error codes rather than private response data.

## Operational bounds

Run one active Agamemnon writer and projector. GitHub does not provide a
cross-record transaction with the board; this view is eventually consistent.
Field/item enumeration is capped at 100 pages of 100 entries. Work labels,
linked PRs, and each item's field values are read up to 100 entries; an
incomplete connection is reported as unavailable or failed, never silently
treated as a complete view. Existing GitHub transport retry behavior applies;
network calls do not yet have a hard total request deadline.

Local contract tests use controlled GitHub responses and do not establish
live project permissions or real API compatibility. Run `just fleet-test`
with the configured cached native dependencies and `just fleet-client-test`
with an environment containing the client dependencies.
