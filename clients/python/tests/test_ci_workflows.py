"""Regression smoke tests for CI workflow hardening — Gitleaks secrets scan gate."""

from pathlib import Path

import yaml

WORKFLOW_PATH = Path(__file__).parents[3] / ".github" / "workflows" / "_required.yml"


def _load_workflow() -> dict:
    """Load the _required.yml workflow as a parsed YAML dict."""
    return yaml.safe_load(WORKFLOW_PATH.read_text())


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
