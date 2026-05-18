# ADR-009: compute_drift Positional Parameter Interface

**Status:** Accepted
**Date:** 2026-04-28
**Accepted:** 2026-05-03
**Author:** mvillmow

---

## Context

The `compute_drift` function (`scripts/lib/reconcile.sh:350-427`) compares the
desired state of an agent (from its YAML file) against its live state (from the
ProjectAgamemnon REST API JSON). To perform the comparison it needs:

- The agent name (for context / error messages)
- The desired `desiredState` (`active` or `hibernated`)
- The full actual agent JSON blob from the API
- Ten desired field values: `label`, `program`, `workingDirectory`,
  `programArgs`, `taskDescription`, `tags`, `model`, `owner`, `role`,
  `deployment.type`

That is 13 inputs in total. Shell functions have no named-parameter mechanism;
the available options are positional parameters, associative arrays, or a
serialized intermediate format.

The function is called from three callers: `apply.sh`, `plan.sh`, and
`status.sh`. Each caller already reads the YAML fields into local shell
variables before calling `compute_drift`.

---

## Decision

Use **13 discrete positional parameters** in a fixed documented order:

```
$1  name               — agent name (for context)
$2  desired_state      — "active" | "hibernated"
$3  actual_json        — full agent JSON from the API
$4  desired_label
$5  desired_program
$6  desired_workdir
$7  desired_args
$8  desired_desc
$9  desired_tags_csv
$10 desired_model
$11 desired_owner
$12 desired_role
$13 desired_deploy_type
```

The function outputs one of:

- `UNCHANGED` — no drift detected
- `WAKE` — agent is offline but desired state is `active`
- `HIBERNATE` — agent is active/online but desired state is `hibernated`
- `UPDATE:<field1>,<field2>,...` — one or more fields differ

---

## Consequences

### Positive

- No runtime dependencies beyond POSIX shell. No `bash 4+` associativity, no
  `declare -A`, no `nameref`.
- Callers are already reading YAML fields into local variables; passing them
  positionally requires no additional serialization step.
- The fixed order is fully documented in the function header comment, making
  the interface self-describing without requiring callers to look up key names.

### Negative / Trade-offs

- The call sites are positionally sensitive. Adding a new field requires
  incrementing the parameter count and updating all three callers in lockstep.
  A missing or reordered argument produces a silent wrong-value bug rather than
  a named-parameter error.
- 13 parameters is at the upper end of what is readable at a call site.
  Callers pass values in a column-aligned block to compensate, but the
  interface is inherently fragile to future extension.

### Neutral

- The function header comment (`scripts/lib/reconcile.sh:335-349`) enumerates
  all 13 parameters. Any future change must update both the comment and every
  caller.

---

## Alternatives Considered

### A. Pass desired state as a JSON blob

**Rejected.** Callers (`apply.sh`, `plan.sh`, `status.sh`) read YAML fields
into local shell variables via `yq`. Serializing those variables back into a
JSON blob on every call adds unnecessary `jq` round-trips and makes the call
site opaque — the reader cannot see which fields are being passed without
inspecting the serialization logic. The positional interface is verbose but
transparent.

### B. Use bash associative arrays

**Rejected.** Associative arrays require `bash 4+` and `declare -A` at both
definition and call sites. Passing an associative array to a function requires
either a `nameref` (`declare -n`, bash 4.3+) or serialization to a string.
`nameref` behaves unexpectedly across subshells (the array is not visible in a
subprocess), which is a real risk since callers may invoke `compute_drift` in a
pipeline. The positional interface works identically in all execution contexts.

### C. Source a shared environment file

**Rejected.** Writing desired-state variables to a temp file and sourcing it
inside `compute_drift` would defeat function isolation and make parallel
invocations unsafe (multiple agents reconciled concurrently would race on the
same file). It also adds I/O for every drift computation.

---

## How to extend compute_drift (lockstep checklist)

Adding a new drifted field requires updating the function **and** all three
callers in lockstep. Missing any step produces a silent wrong-value bug because
shell positional parameters bind by position, not name.

1. **Add the parameter to `compute_drift`** (`scripts/lib/reconcile.sh`):
   - Append `$14` (or the next slot) to the function body with a `local` binding.
   - Update the header comment's parameter table (lines 335–349).
   - Add a drift comparison line (e.g. `[[ "$actual_foo" != "$desired_foo" ]] && drifted_fields+=("foo")`).

2. **Bump the arity guard**: change the `13` in `[[ $# -ne 13 ]]` to the new
   count. This is the authoritative number that `check-compute-drift-callers.sh`
   reads.

3. **Update all three callers** (`apply.sh`, `plan.sh`, `status.sh`) to pass
   the new argument in the matching position. All three call sites must change
   together — a partial update causes a positional shift for every argument that
   follows.

4. **Add a row to the drift-detection table in CLAUDE.md** so operators know
   the field is tracked.

5. **Write a new UPDATE test** in `tests/unit/test_drift_owner_role_tags.bats`
   (or a new test file) asserting that drift is detected when the new field
   changes and is absent from the result when the field is unchanged.

6. **Run the caller consistency check** — must exit 0 before committing:

   ```bash
   ./scripts/check-compute-drift-callers.sh
   ```

   This script is also wired as a pre-commit hook and fires automatically
   whenever `reconcile.sh` or any of the three callers is staged.

7. **Run the full unit test suite** — all drift tests (including the arity
   guard tests) must pass:

   ```bash
   pixi run test-unit
   ```

**Caution:** This is inherently fragile due to the positional nature. Consider
proposing a higher-level refactor (e.g., a structured parameter format) if you
find yourself adding a fourth or fifth extension.

---

## Implementation Scope (this ADR)

| File | Role |
| --- | --- |
| `scripts/lib/reconcile.sh:335-431` | `compute_drift` definition, parameter comment block, and arity guard |
| `scripts/apply.sh` | Primary caller — passes all 13 parameters |
| `scripts/plan.sh` | Caller — dry-run drift check |
| `scripts/status.sh` | Caller — status table drift column |
| `scripts/check-compute-drift-callers.sh` | Pre-commit / CI arity consistency check |
| `tests/unit/test_compute_drift_arity.bats` | Arity guard unit tests |
