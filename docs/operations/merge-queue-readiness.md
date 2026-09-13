# Merge Queue Readiness

The queue commit must report all 18 required contexts. The three required workflows subscribe to
`merge_group` with the `checks_requested` activity, as well as their existing `push` and
`pull_request` events on `main`:

- `_required.yml`: the 12 canonical lint, unit, integration, security, build, schema, dependency,
  aggregate-test, package, install, and release-readiness contexts.
- `build-test.yml`: `All Build/Test Checks` and its four compiler/build-type matrix contexts.
- `static-analysis.yml`: `All Static Analysis Checks`.

`merge-queue-smoke.yml` remains a separate five-minute smoke check. Its result cannot replace those
18 contexts. Required job names, permissions, aggregate result checks, scanner policies, and
matrix entries remain part of the required-check contract. The existing optional PR-only docs
job and tag-only publishing workflows retain their separate event rules.

`clients/python/tests/test_ci_workflows.py` checks queue subscriptions, exact context names and
required-job eligibility. It also executes the aggregate shell blocks with controlled successful
and failed dependency results. These tests do not execute a hosted merge group or a container.
A complete local CI result and the required hosted checks on the final source remain necessary
before merge. A queued commit must then receive its own required checks.

Changing the live ruleset, branch protection, queue policy, or merge method is an administrative
operation outside this workflow correction. Track the operational queue observation in issue #491.
