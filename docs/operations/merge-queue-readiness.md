# Merge Queue Readiness

Agamemnon stages merge-queue rollout in two steps. Repository changes first ensure every required
status context runs for `merge_group` events with the `checks_requested` activity. A repository
administrator may activate the queue only after that support has merged and a representative queued
pull request can be observed.

The required contexts are supplied by these workflows:

- `_required.yml`: the canonical lint, test, security, build, schema, dependency, package, install,
  and release-readiness contexts
- `build-test.yml`: `All Build/Test Checks` and its four compiler/build-type matrix contexts
- `static-analysis.yml`: `All Static Analysis Checks`

Their `push` and `pull_request` triggers, job names, permissions, and security gates are part of the
required-check contract. Tag-only publishing workflows remain separate and must not run for merge
groups.

Enabling or changing the live ruleset, branch protection, queue policy, or merge method is an
administrative operation outside workflow-readiness changes. For the initial rollout, track
activation and the post-merge queue smoke check in issue #452.
