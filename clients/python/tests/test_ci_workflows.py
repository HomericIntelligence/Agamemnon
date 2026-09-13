"""Regression smoke tests for required CI workflow contracts."""

from pathlib import Path

import tomllib
import yaml

WORKFLOW_DIR = Path(__file__).parents[3] / ".github" / "workflows"
WORKFLOW_PATH = WORKFLOW_DIR / "_required.yml"

# ── Python audit dependency floor (issue #495: PYSEC-2026-3721) ────────────
# The Python client's locked audit environment is what security/dependency-scan
# audits, so three things must move together: the manifest floor in the `lint`
# dependency group, the resolver-owned lock, and the exact command CI runs.
# Any one of them drifting reintroduces a failing-close audit on every PR.
PYPROJECT_PATH = Path(__file__).parents[1] / "pyproject.toml"
LOCK_PATH = Path(__file__).parents[1] / "uv.lock"

# First pip release carrying the PYSEC-2026-3721 fix (per the GitHub advisory
# database). The manifest floor and the resolved lock must both stay at or
# above this.
PIP_AUDIT_FLOOR = (26, 2)

# Exact command the security-dependency-scan job runs for the Python audit.
PIP_AUDIT_COMMAND = "cd clients/python && uv run --only-group lint pip-audit --skip-editable"
PYPROJECT_SECTION = "dependency-groups.lint"

REQUIRED_WORKFLOWS = ("_required.yml", "build-test.yml", "static-analysis.yml")
SMOKE_WORKFLOW = "merge-queue-smoke.yml"
REQUIRED_CONTEXT_JOBS = {
    "lint": ("_required.yml", "lint"),
    "unit-tests": ("_required.yml", "unit-tests"),
    "integration-tests": ("_required.yml", "integration-tests"),
    "security/dependency-scan": ("_required.yml", "security-dependency-scan"),
    "security/secrets-scan": ("_required.yml", "security-secrets-scan"),
    "build": ("_required.yml", "build"),
    "schema-validation": ("_required.yml", "schema-validation"),
    "deps/version-sync": ("_required.yml", "deps-version-sync"),
    "test": ("_required.yml", "test"),
    "package": ("_required.yml", "package"),
    "install": ("_required.yml", "install"),
    "release": ("_required.yml", "release"),
    "All Build/Test Checks": ("build-test.yml", "check-all"),
    "All Static Analysis Checks": ("static-analysis.yml", "check-all"),
}


def _load_workflow(path: Path = WORKFLOW_PATH) -> dict:
    """Load a workflow as a parsed YAML dict."""
    return yaml.safe_load(path.read_text())


def _workflow_triggers(workflow: dict) -> dict:
    """Return triggers despite PyYAML 1.1 parsing the unquoted `on` key as true."""
    return workflow.get("on", workflow.get(True, {}))


def test_merge_group_runs_only_the_smoke_workflow() -> None:
    """The merge queue must run exactly one fast smoke job (one runner slot).

    The full workflows must NOT re-run for merge_group — that starved the
    runner pool and pushed queue merges to 70-90 min. merge-queue-smoke.yml
    owns the merge_group event and emits the single `merge-queue-smoke`
    context; PR-side CI is untouched.
    """
    for filename in REQUIRED_WORKFLOWS:
        triggers = _workflow_triggers(_load_workflow(WORKFLOW_DIR / filename))
        assert triggers["push"]["branches"] == ["main"]
        assert triggers["pull_request"]["branches"] == ["main"]
        assert "merge_group" not in triggers, (
            f"{filename} must not trigger on merge_group — merge-queue-smoke.yml "
            "owns that event"
        )

    smoke = _load_workflow(WORKFLOW_DIR / SMOKE_WORKFLOW)
    assert _workflow_triggers(smoke) == {"merge_group": {"types": ["checks_requested"]}}
    assert list(smoke["jobs"]) == ["merge-queue-smoke"]
    assert smoke["jobs"]["merge-queue-smoke"]["name"] == "merge-queue-smoke"
    assert smoke["jobs"]["merge-queue-smoke"]["timeout-minutes"] == 5


def test_live_required_context_names_remain_exact() -> None:
    """Required job names must continue to match the live ruleset contexts exactly."""
    workflows = {
        filename: _load_workflow(WORKFLOW_DIR / filename) for filename in REQUIRED_WORKFLOWS
    }
    actual_contexts = {
        workflows[filename]["jobs"][job_id]["name"]
        for filename, job_id in REQUIRED_CONTEXT_JOBS.values()
    }

    matrix_job = workflows["build-test.yml"]["jobs"]["build-test"]
    assert matrix_job["name"] == "${{ matrix.os }}-${{ matrix.compiler }}-${{ matrix.build_type }}"
    matrix = matrix_job["strategy"]["matrix"]
    actual_contexts.update(
        f"{os_name}-{compiler}-{build_type}"
        for os_name in matrix["os"]
        for compiler in matrix["compiler"]
        for build_type in matrix["build_type"]
    )

    assert actual_contexts == set(REQUIRED_CONTEXT_JOBS) | {
        "ubuntu-24.04-clang-debug",
        "ubuntu-24.04-clang-release",
        "ubuntu-24.04-gcc-debug",
        "ubuntu-24.04-gcc-release",
    }


def test_merge_queue_regression_runs_in_required_job() -> None:
    """The required workflow must execute this regression on every queue run."""
    workflow = _load_workflow()
    steps = workflow["jobs"]["lint"]["steps"]
    regression = next(
        step for step in steps if step.get("name") == "Run merge-queue workflow regression"
    )

    # Runs podman-by-default inside the CI container, mounting the workspace.
    assert regression["run"].startswith("podman run")
    assert "--userns=keep-id" in regression["run"]
    assert "-w /workspace agamemnon-ci:local" in regression["run"]
    assert "uv run python -m pytest tests/test_ci_workflows.py -v" in regression["run"]


def _find_gitleaks_scan_step(workflow: dict) -> dict:
    """Return the 'Run Gitleaks' step from the security-secrets-scan job."""
    steps = workflow["jobs"]["security-secrets-scan"]["steps"]
    for step in steps:
        if step.get("name") == "Run Gitleaks":
            return step
    raise AssertionError("Could not find 'Run Gitleaks' step in security-secrets-scan job")


def _find_gitleaks_upload_step(workflow: dict) -> dict:
    """Return the 'Upload Gitleaks SARIF' step from the security-secrets-scan job."""
    steps = workflow["jobs"]["security-secrets-scan"]["steps"]
    for step in steps:
        if step.get("name") == "Upload Gitleaks SARIF":
            return step
    raise AssertionError("Could not find 'Upload Gitleaks SARIF' step in security-secrets-scan job")


def _release_tuple(version: str) -> tuple[int, ...]:
    """Return the numeric release tuple for a PEP 440 version string."""
    core = version.split("+", 1)[0].split("-", 1)[0]
    return tuple(int(part) for part in core.split("."))


def _lint_group_pip_requirement() -> str:
    """Return the raw pip requirement string declared in the lint group."""
    groups = tomllib.loads(PYPROJECT_PATH.read_text()).get("dependency-groups", {})
    for requirement in groups.get("lint", []):
        name = requirement.split(";", 1)[0]
        for delimiter in (">=", "<=", "==", "~=", ">", "<", "[", " "):
            name = name.split(delimiter, 1)[0]
        if name.strip().lower() == "pip":
            return requirement
    raise AssertionError(
        f"{PYPROJECT_SECTION} declares no pip requirement — the audit floor is unbound"
    )


def _lint_group_pip_floor() -> tuple[int, ...]:
    """Return the manifest floor for pip as a comparable release tuple."""
    specifier = _lint_group_pip_requirement().split(";", 1)[0]
    if ">=" not in specifier:
        raise AssertionError(
            f"{PYPROJECT_SECTION} pip requirement '{specifier}' declares no '>=' floor"
        )
    return _release_tuple(specifier.split(">=", 1)[1].strip())


def _locked_pip_version() -> tuple[int, ...]:
    """Return the pip version resolved in the client's resolver-owned lock."""
    lock = tomllib.loads(LOCK_PATH.read_text())
    for package in lock.get("package", []):
        if str(package.get("name", "")).lower() == "pip":
            return _release_tuple(str(package["version"]))
    raise AssertionError("uv.lock contains no pip package")


def test_pip_audit_manifest_floor_meets_advisory_fix() -> None:
    """The lint-group pip floor must be at or above the PYSEC-2026-3721 fix."""
    floor = _lint_group_pip_floor()
    assert floor >= PIP_AUDIT_FLOOR, (
        f'{PYPROJECT_SECTION} pip floor {floor} is below the advisory fix '
        f"{PIP_AUDIT_FLOOR} — security/dependency-scan fails closed on PYSEC-2026-3721"
    )


def test_locked_pip_satisfies_the_manifest_floor() -> None:
    """The resolved lock must honour the manifest floor and the advisory fix.

    A floor that the lock does not apply is exactly the drift that let pip
    26.1.2 keep shipping in the audited environment.
    """
    floor = _lint_group_pip_floor()
    locked = _locked_pip_version()
    assert locked >= floor, (
        f"uv.lock resolves pip {locked} below the {PYPROJECT_SECTION} floor {floor} — "
        "regenerate the lock with `uv lock`"
    )
    assert locked >= PIP_AUDIT_FLOOR, (
        f"uv.lock resolves pip {locked} below the advisory fix {PIP_AUDIT_FLOOR} — "
        "security/dependency-scan fails closed on PYSEC-2026-3721"
    )


def test_dependency_scan_runs_the_bound_pip_audit_command() -> None:
    """security/dependency-scan must run the bound audit command, fail closed."""
    workflow = _load_workflow()
    steps = workflow["jobs"]["security-dependency-scan"]["steps"]
    runnable = [step for step in steps if "run" in step]
    audit_step = next((step for step in runnable if PIP_AUDIT_COMMAND in step["run"]), None)
    assert audit_step is not None, (
        f"security-dependency-scan does not run '{PIP_AUDIT_COMMAND}'"
    )

    run_block = audit_step["run"]
    assert "continue-on-error" not in audit_step, (
        "Dependency scan step has continue-on-error — the audit no longer fails closed"
    )
    assert "--ignore-vuln" not in run_block, (
        "Dependency scan suppresses advisories with --ignore-vuln instead of fixing them"
    )
    command_line = next(
        line for line in run_block.splitlines() if PIP_AUDIT_COMMAND in line
    )
    assert "||" not in command_line, (
        "Dependency scan masks pip-audit failures with a shell fallback"
    )
    assert "--skip-editable" in command_line, (
        "Dependency scan dropped --skip-editable, widening the audited set"
    )


def test_gitleaks_scan_step_is_blocking() -> None:
    """The Run Gitleaks step must not have continue-on-error set."""
    workflow = _load_workflow()
    step = _find_gitleaks_scan_step(workflow)
    assert "continue-on-error" not in step, (
        "Run Gitleaks step has continue-on-error — secrets scan is not a blocking gate"
    )


def test_gitleaks_uses_exit_code_1() -> None:
    """The Run Gitleaks step must use --exit-code 1, not --exit-code 0."""
    workflow = _load_workflow()
    step = _find_gitleaks_scan_step(workflow)
    run_block: str = step["run"]

    assert "--exit-code 0" not in run_block, (
        "Run Gitleaks step still uses --exit-code 0 — scan result is informational-only"
    )
    assert "--exit-code 1" in run_block, (
        "Run Gitleaks step does not use --exit-code 1 — scan is not blocking on secrets"
    )


def test_gitleaks_sarif_upload_step_not_affected() -> None:
    """The SARIF upload step must still use if: always() so reports are uploaded even on failure."""
    workflow = _load_workflow()
    step = _find_gitleaks_upload_step(workflow)
    condition: str = step.get("if", "")
    assert "always()" in condition, (
        "Upload Gitleaks SARIF step lost its 'always()' condition — "
        "SARIF reports will not be uploaded when the scan fails"
    )
