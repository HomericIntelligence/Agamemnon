"""Regression smoke tests for required CI workflow contracts."""

import os
import subprocess
from pathlib import Path

import pytest
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


@pytest.mark.parametrize("filename", REQUIRED_WORKFLOWS)
def test_required_workflows_run_for_queue_commit(filename: str) -> None:
    """The queue commit must run each workflow that owns a required check."""
    triggers = _workflow_triggers(_load_workflow(WORKFLOW_DIR / filename))
    assert triggers["push"]["branches"] == ["main"]
    assert triggers["pull_request"]["branches"] == ["main"]
    assert triggers.get("merge_group") == {"types": ["checks_requested"]}, filename


def test_queue_smoke_remains_a_separate_bounded_check() -> None:
    """The supplemental smoke check retains its existing event and time bound."""
    smoke = _load_workflow(WORKFLOW_DIR / SMOKE_WORKFLOW)
    assert _workflow_triggers(smoke) == {"merge_group": {"types": ["checks_requested"]}}
    assert list(smoke["jobs"]) == ["merge-queue-smoke"]
    assert smoke["jobs"]["merge-queue-smoke"]["name"] == "merge-queue-smoke"
    assert smoke["jobs"]["merge-queue-smoke"]["timeout-minutes"] == 5


@pytest.mark.parametrize(
    "filename,job_id", [*REQUIRED_CONTEXT_JOBS.values(), ("build-test.yml", "build-test")]
)
def test_required_jobs_do_not_exclude_queue_events(filename: str, job_id: str) -> None:
    """Required jobs cannot use a PR-only or push-only execution condition."""
    job = _load_workflow(WORKFLOW_DIR / filename)["jobs"][job_id]
    assert job.get("if") in (None, "always()"), (filename, job_id)


@pytest.mark.parametrize(
    "filename,job_id",
    [
        ("_required.yml", "test"),
        ("build-test.yml", "check-all"),
        ("static-analysis.yml", "check-all"),
    ],
)
def test_aggregate_scripts_preserve_dependency_failures(
    filename: str, job_id: str, tmp_path: Path
) -> None:
    """Execute the actual aggregate script for success and each failed dependency."""
    job = _load_workflow(WORKFLOW_DIR / filename)["jobs"][job_id]
    step = next(step for step in job["steps"] if "run" in step)
    statuses = {key: "success" for key in step["env"]}

    def run(values: dict[str, str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", "-e", "-c", step["run"]],
            env={"PATH": os.defpath, **values},
            cwd=tmp_path,
            text=True,
            capture_output=True,
            timeout=5,
        )

    success = run(statuses)
    assert success.returncode == 0, success.stderr
    for dependency in statuses:
        failed = run({**statuses, dependency: "failure"})
        assert failed.returncode != 0, (filename, dependency, failed.stdout)
    if "DOCS_RESULT" in statuses:
        # This existing optional job runs on PRs. Its queue skip is intentional.
        assert run({**statuses, "DOCS_RESULT": "skipped"}).returncode == 0


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


def test_empty_conan_cache_is_created_before_container_mount(tmp_path: Path) -> None:
    """Execute each mount step up to a controlled container boundary on a cache miss."""
    workflow = _load_workflow()
    checked = []
    for job_id, job in workflow["jobs"].items():
        for step in job.get("steps", []):
            script = step.get("run", "")
            if "$HOME/.conan2:/home/ci/.conan2:Z" not in script:
                continue
            sandbox = tmp_path / job_id
            sandbox.mkdir()
            fake_home = sandbox / "home"
            fake_home.mkdir()
            executable = sandbox / "podman"
            executable.write_text(
                '#!/bin/sh\nif [ ! -d "$FLEET_TEST_HOME/.conan2" ]; then exit 74; fi\nexit 73\n'
            )
            executable.chmod(0o700)
            env = {
                "PATH": str(sandbox) + os.pathsep + os.defpath,
                "FLEET_TEST_HOME": str(fake_home),
                "CONAN_HOME": "/home/ci/.conan2",
                "FLEET_TEST_PACKAGE_OUTPUT": str(sandbox / "package.out"),
            }
            result = subprocess.run(
                [
                    "bash",
                    "-e",
                    "-c",
                    script.replace("$HOME", "$FLEET_TEST_HOME").replace(
                        "/tmp/agamemnon-package.out", '"$FLEET_TEST_PACKAGE_OUTPUT"'
                    ),
                ],
                cwd=sandbox,
                env=env,
                capture_output=True,
                text=True,
                timeout=5,
            )
            assert result.returncode == 73, (job_id, result.returncode, result.stderr)
            assert (fake_home / ".conan2").is_dir()
            checked.append(job_id)
    assert len(checked) == 7
