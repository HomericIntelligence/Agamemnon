# Merge Queue Readiness

The queue commit must report all 18 required contexts. The three required workflows subscribe to
`merge_group` with the `checks_requested` activity, as well as their existing `push` and
`pull_request` events on `main`:

- `_required.yml`: the 12 canonical lint, unit, integration, security, build, schema, dependency,
  aggregate-test, package, install, and release-readiness contexts.
- `build-test.yml`: `All Build/Test Checks` and its four compiler/build-type matrix contexts.
- `static-analysis.yml`: `All Static Analysis Checks`.

The former `merge-queue-smoke.yml` workflow is removed. Required job names, permissions, aggregate
result checks, scanner policies, and matrix entries remain part of the required-check contract.
Documentation validation runs on PR and merge-group commits; only a push may skip that job.
Tag-only publishing workflows retain their separate event rules. Each required workflow uses an
event-scoped concurrency group so PR and merge-group runs cannot cancel one another.

`clients/python/tests/test_ci_workflows.py` checks queue subscriptions, exact context names and
required-job eligibility. It also executes the aggregate shell blocks with controlled successful
and failed dependency results. These tests do not execute a hosted merge group or a container.
A complete local CI result and the required hosted checks on the final source remain necessary
before merge. A queued commit must then receive its own required checks.

Enabling or changing the live ruleset, branch protection, queue policy, or merge method is an
administrative operation outside workflow-readiness changes. For the initial rollout, track
activation and a representative merge-group check cycle in issue #491.
