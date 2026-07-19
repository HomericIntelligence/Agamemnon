"""Regression smoke tests for required CI workflow contracts."""

from pathlib import Path

import yaml

WORKFLOW_DIR = Path(__file__).parents[3] / ".github" / "workflows"
WORKFLOW_PATH = WORKFLOW_DIR / "_required.yml"

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

    assert regression["working-directory"] == "clients/python"
    assert regression["run"] == (
        "pixi run --environment default python -m pytest tests/test_ci_workflows.py -v"
    )


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
