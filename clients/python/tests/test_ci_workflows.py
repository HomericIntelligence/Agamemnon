"""Regression smoke tests for required CI workflow contracts."""

import os
import re
import subprocess
from pathlib import Path

import pytest
import tomllib
import yaml

WORKFLOW_DIR = Path(__file__).parents[3] / ".github" / "workflows"
WORKFLOW_PATH = WORKFLOW_DIR / "_required.yml"

# Cold-runner Conan cache precondition (issue #493). A cache miss leaves
# $HOME/.conan2 absent, so every job that bind-mounts it must create it first
# or podman fails to start the container with `statfs: no such file or
# directory`.
REPO_ROOT = WORKFLOW_DIR.parents[1]
CONAN_CACHE_BOOTSTRAP = REPO_ROOT / "scripts" / "prepare-conan-cache.sh"
CONAN_CACHE_MOUNT = '-v "$HOME/.conan2:/home/ci/.conan2:Z"'
CONAN_CACHE_JOBS = {
    "lint",
    "unit-tests",
    "integration-tests",
    "security-dependency-scan",
    "build",
    "package",
    "install",
}

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

# ── clang-tidy must stay out of the Build and Test matrix (issue #515) ──────
# clang-tidy costs 55-95s per translation unit. When it is enabled by default,
# every Build and Test matrix job inherits it, which pinned each one at its
# 30-minute `timeout-minutes` cap and cancelled them under concurrent PR load —
# so the required ubuntu-24.04-* contexts never reported and PRs sat BLOCKED.
# Enforcement belongs to the jobs that opt in explicitly.
ANALYZERS_PATH = REPO_ROOT / "cmake" / "StaticAnalyzers.cmake"
CLANG_TIDY_JOBS_THAT_OPT_IN = (
    ("_required.yml", "lint"),
    ("static-analysis.yml", "clang-tidy"),
)
CLANG_TIDY_OPTION = re.compile(
    r'option\(\s*\$\{PROJECT_NAME\}_ENABLE_CLANG_TIDY\s+"[^"]*"\s+(ON|OFF)\s*\)'
)
# Same defect class, sibling option (#517): cppcheck was left defaulting ON when
# clang-tidy was fixed in #515, and the regex above deliberately matches only
# clang-tidy, which is how it went unnoticed.
CPPCHECK_OPTION = re.compile(
    r'option\(\s*\$\{PROJECT_NAME\}_ENABLE_CPPCHECK\s+"[^"]*"\s+(ON|OFF)\s*\)'
)
ANALYZER_OPTION = re.compile(
    r'option\(\s*\$\{PROJECT_NAME\}_ENABLE_(CLANG_TIDY|CPPCHECK)\s+"[^"]*"\s+(ON|OFF)\s*\)'
)


def _load_workflow(path: Path = WORKFLOW_PATH) -> dict:
    """Load a workflow as a parsed YAML dict."""
    return yaml.safe_load(path.read_text())


def _workflow_triggers(workflow: dict) -> dict:
    """Return triggers despite PyYAML 1.1 parsing the unquoted `on` key as true."""
    return workflow.get("on", workflow.get(True, {}))


def _transitive_needs(workflow: dict, root_job_id: str) -> set[str]:
    """Return every job that the root job depends on directly or indirectly."""
    dependencies: set[str] = set()
    pending = list(workflow["jobs"][root_job_id].get("needs", []))

    while pending:
        job_id = pending.pop()
        if job_id in dependencies:
            continue
        dependencies.add(job_id)
        needs = workflow["jobs"][job_id].get("needs", [])
        pending.extend([needs] if isinstance(needs, str) else needs)

    return dependencies


def _job_runs_for_event(job: dict, event_name: str) -> bool:
    """Evaluate the simple event conditions used by required workflow jobs."""
    condition = str(job.get("if", "")).strip()
    if not condition or condition == "always()":
        return True

    match = re.fullmatch(r"github\.event_name\s*(==|!=)\s*'([^']+)'", condition)
    assert match is not None, f"cannot prove event reachability for condition: {condition}"
    operator, compared_event = match.groups()
    return (event_name == compared_event) if operator == "==" else (event_name != compared_event)


def test_conan_cache_bootstrap_creates_a_cold_home(tmp_path: Path) -> None:
    """The repository bootstrap must create the bind source on a cold runner."""
    assert CONAN_CACHE_BOOTSTRAP.is_file()
    assert os.access(CONAN_CACHE_BOOTSTRAP, os.X_OK)

    home = tmp_path / "cold-runner-home"
    home.mkdir()
    cache = home / ".conan2"
    assert not cache.exists()

    subprocess.run(
        [str(CONAN_CACHE_BOOTSTRAP)],
        check=True,
        env={"HOME": str(home), "PATH": os.environ["PATH"]},
    )

    assert cache.is_dir()


def test_every_conan_mount_is_preceded_by_the_repository_bootstrap() -> None:
    """No required job may mount a cache directory that a cache miss left absent."""
    workflow = _load_workflow()
    mounted_jobs: set[str] = set()

    for job_id, job in workflow["jobs"].items():
        steps = job.get("steps", [])
        mount_indexes = [
            index
            for index, step in enumerate(steps)
            if CONAN_CACHE_MOUNT in str(step.get("run", ""))
        ]
        if not mount_indexes:
            continue

        mounted_jobs.add(job_id)
        bootstrap_indexes = [
            index
            for index, step in enumerate(steps)
            if step.get("run") == "./scripts/prepare-conan-cache.sh"
        ]
        assert bootstrap_indexes, f"{job_id} does not prepare the Conan cache"
        assert any(index < min(mount_indexes) for index in bootstrap_indexes), (
            f"{job_id} prepares the Conan cache only after mounting it"
        )

    assert mounted_jobs == CONAN_CACHE_JOBS


def test_required_workflows_have_pull_request_and_merge_group_parity() -> None:
    """Each required context must be reachable for PR and merge-group commits."""
    workflows = {
        filename: _load_workflow(WORKFLOW_DIR / filename) for filename in REQUIRED_WORKFLOWS
    }

    for filename in REQUIRED_WORKFLOWS:
        triggers = _workflow_triggers(workflows[filename])
        assert triggers["push"]["branches"] == ["main"]
        assert triggers["pull_request"]["branches"] == ["main"]
        assert triggers["merge_group"] == {"types": ["checks_requested"]}

    expected_contexts = _required_context_names(workflows)
    reachable_contexts: dict[str, set[str]] = {}

    for event_name in ("pull_request", "merge_group"):
        event_contexts = set()
        for context_name, (filename, job_id) in REQUIRED_CONTEXT_JOBS.items():
            assert event_name in _workflow_triggers(workflows[filename])
            condition = str(workflows[filename]["jobs"][job_id].get("if", ""))
            assert "github.event" not in condition, (
                f"{filename}:{job_id} suppresses the {context_name!r} context "
                f"for one or more events: {condition}"
            )
            event_contexts.add(context_name)

        matrix_job = workflows["build-test.yml"]["jobs"]["build-test"]
        assert "github.event" not in str(matrix_job.get("if", ""))
        event_contexts.update(_matrix_context_names(matrix_job))
        reachable_contexts[event_name] = event_contexts

    assert reachable_contexts == {
        "pull_request": expected_contexts,
        "merge_group": expected_contexts,
    }


def test_required_workflows_use_event_scoped_concurrency() -> None:
    """PR and merge-group runs must not share a cancellation group."""
    for filename in REQUIRED_WORKFLOWS:
        workflow = _load_workflow(WORKFLOW_DIR / filename)
        concurrency = workflow["concurrency"]
        group = concurrency["group"]

        assert "${{ github.workflow }}" in group
        assert "${{ github.event_name }}" in group
        assert "${{ github.ref }}" in group or "${{ github.sha }}" in group
        assert concurrency["cancel-in-progress"] is True


def test_build_test_gate_dependencies_run_for_required_events() -> None:
    """All jobs behind the build/test gate must run for PR and merge-group commits."""
    workflow = _load_workflow(WORKFLOW_DIR / "build-test.yml")
    dependencies = _transitive_needs(workflow, "check-all")

    assert dependencies
    for job_id in dependencies:
        job = workflow["jobs"][job_id]
        assert _job_runs_for_event(job, "pull_request"), (
            f"build-test.yml:{job_id} is suppressed for pull_request"
        )
        assert _job_runs_for_event(job, "merge_group"), (
            f"build-test.yml:{job_id} is suppressed for merge_group"
        )

    assert not _job_runs_for_event(workflow["jobs"]["docs"], "push")


@pytest.mark.parametrize(
    ("event_name", "docs_result", "expected_success"),
    (
        ("pull_request", "success", True),
        ("pull_request", "skipped", False),
        ("merge_group", "success", True),
        ("merge_group", "skipped", False),
        ("push", "success", True),
        ("push", "skipped", True),
        ("push", "failure", False),
    ),
)
def test_build_test_gate_allows_docs_skip_only_for_push(
    event_name: str, docs_result: str, expected_success: bool
) -> None:
    """The aggregate gate must fail if required documentation validation is skipped."""
    workflow = _load_workflow(WORKFLOW_DIR / "build-test.yml")
    gate_step = next(
        step
        for step in workflow["jobs"]["check-all"]["steps"]
        if step.get("name") == "Check all jobs passed"
    )
    environment = {name: "success" for name in gate_step["env"]}
    environment.update(EVENT_NAME=event_name, DOCS_RESULT=docs_result)

    result = subprocess.run(
        ["/bin/bash", "-eu", "-o", "pipefail", "-c", gate_step["run"]],
        check=False,
        capture_output=True,
        env=environment,
        text=True,
    )

    assert (result.returncode == 0) is expected_success, result.stdout + result.stderr


def test_smoke_only_merge_queue_carrier_is_absent() -> None:
    """A separate smoke workflow must not replace the required producers."""
    smoke_path = WORKFLOW_DIR / "merge-queue-smoke.yml"
    assert not smoke_path.exists()

    for path in WORKFLOW_DIR.glob("*.yml"):
        workflow = _load_workflow(path)
        assert all(
            job_id != "merge-queue-smoke" and job.get("name") != "merge-queue-smoke"
            for job_id, job in workflow.get("jobs", {}).items()
        ), f"{path.name} still defines the smoke-only merge-queue carrier"


def _matrix_context_names(matrix_job: dict) -> set[str]:
    """Return the concrete names emitted by the required build matrix."""
    matrix = matrix_job["strategy"]["matrix"]
    return {
        f"{os_name}-{compiler}-{build_type}"
        for os_name in matrix["os"]
        for compiler in matrix["compiler"]
        for build_type in matrix["build_type"]
    }


def _required_context_names(workflows: dict[str, dict]) -> set[str]:
    """Return the required context contract from one canonical mapping."""
    contexts = set(REQUIRED_CONTEXT_JOBS)
    contexts.update(_matrix_context_names(workflows["build-test.yml"]["jobs"]["build-test"]))
    return contexts


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
    actual_contexts.update(_matrix_context_names(matrix_job))

    assert actual_contexts == _required_context_names(workflows)


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


def test_build_matrix_does_not_pay_for_clang_tidy() -> None:
    """clang-tidy must be opt-in, or the matrix races its own timeout (issue #515)."""
    option = CLANG_TIDY_OPTION.search(ANALYZERS_PATH.read_text())
    assert option is not None, (
        "cmake/StaticAnalyzers.cmake no longer declares "
        "Agamemnon_ENABLE_CLANG_TIDY"
    )
    assert option.group(1) == "OFF", (
        "ENABLE_CLANG_TIDY defaults ON, so every Build and Test matrix job pays "
        "55-95s per TU and is cancelled at its 30-minute timeout (issue #515)"
    )

    build_test = _load_workflow(WORKFLOW_DIR / "build-test.yml")
    enabling_steps = [
        f"{job_id}:{step.get('name')}"
        for job_id, job in build_test["jobs"].items()
        for step in (job.get("steps") or [])
        if "ENABLE_CLANG_TIDY" in str(step.get("run", ""))
    ]
    assert not enabling_steps, (
        "build-test.yml enables clang-tidy — enforcement belongs to the lint and "
        f"clang-tidy jobs (issue #515): {enabling_steps}"
    )


def test_build_matrix_does_not_pay_for_cppcheck() -> None:
    """cppcheck must be opt-in too, or installing it re-breaks the matrix (issue #517)."""
    option = CPPCHECK_OPTION.search(ANALYZERS_PATH.read_text())
    assert option is not None, (
        "cmake/StaticAnalyzers.cmake no longer declares Agamemnon_ENABLE_CPPCHECK"
    )
    assert option.group(1) == "OFF", (
        "ENABLE_CPPCHECK defaults ON, so every Build and Test matrix job runs "
        "cppcheck per TU as soon as it is installed — the #515 failure mode "
        "(issue #517)"
    )

    build_test = _load_workflow(WORKFLOW_DIR / "build-test.yml")
    enabling_steps = [
        f"{job_id}:{step.get('name')}"
        for job_id, job in build_test["jobs"].items()
        for step in (job.get("steps") or [])
        if "ENABLE_CPPCHECK" in str(step.get("run", ""))
    ]
    assert not enabling_steps, (
        "build-test.yml enables cppcheck — enforcement belongs to a dedicated job "
        f"(issue #517): {enabling_steps}"
    )


def test_no_analyzer_defaults_on() -> None:
    """Guard the class, not just the instances: no analyzer may default ON (#517)."""
    defaults = dict(
        (name, state) for name, state in ANALYZER_OPTION.findall(ANALYZERS_PATH.read_text())
    )
    assert set(defaults) == {"CLANG_TIDY", "CPPCHECK"}, (
        f"expected both analyzer options in cmake/StaticAnalyzers.cmake, found {sorted(defaults)}"
    )
    on_by_default = sorted(name for name, state in defaults.items() if state != "OFF")
    assert not on_by_default, (
        "these analyzers default ON, so they run in every build configuration and "
        f"its matrix jobs pay for them (issues #515, #517): {on_by_default}"
    )


@pytest.mark.parametrize(("filename", "job_id"), CLANG_TIDY_JOBS_THAT_OPT_IN)
def test_dedicated_jobs_still_enable_clang_tidy(filename: str, job_id: str) -> None:
    """Defaulting the option off must not drop clang-tidy enforcement (issue #515)."""
    workflow = _load_workflow(WORKFLOW_DIR / filename)
    steps = workflow["jobs"][job_id]["steps"]

    assert any(
        "-DAgamemnon_ENABLE_CLANG_TIDY=ON" in str(step.get("run", ""))
        for step in steps
    ), f"{filename}:{job_id} no longer enables clang-tidy (issue #515)"


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
