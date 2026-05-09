"""Tests for scripts/bump-version.py."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "bump-version.py"

MINIMAL_TOML = """\
[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[project]
name = "example"
version = "0.1.0"
description = "Example"
"""

TOML_MULTI_VERSION = """\
[build-system]
requires = ["hatchling>=1.0.0"]
build-backend = "hatchling.build"

[project]
name = "example"
version = "0.1.0"
description = "uses version in other sections too"

[tool.hatch.metadata]
# hatchling version = "unrelated"
"""

TOML_NO_VERSION = """\
[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[project]
name = "example"
description = "no version here"
"""


def _run(
    version: str, toml_content: str | None = None, *, tmp_path: Path
) -> subprocess.CompletedProcess[str]:
    toml = tmp_path / "pyproject.toml"
    content = toml_content if toml_content is not None else MINIMAL_TOML
    toml.write_text(content, encoding="utf-8")

    # Patch the script so it targets our tmp pyproject.toml
    script_src = SCRIPT.read_text(encoding="utf-8")
    patched_src = script_src.replace(
        'repo_root / "clients" / "python" / "pyproject.toml"',
        f'Path(r"{toml}")',
    )
    patched_script = tmp_path / "bump_version_patched.py"
    patched_script.write_text(patched_src, encoding="utf-8")

    return subprocess.run(
        [sys.executable, str(patched_script), version],
        capture_output=True,
        text=True,
    )


# ── Happy path ─────────────────────────────────────────────────────────────────


def test_bumps_version_field(tmp_path: Path) -> None:
    result = _run("2.3.4", tmp_path=tmp_path)
    assert result.returncode == 0
    content = (tmp_path / "pyproject.toml").read_text()
    assert 'version = "2.3.4"' in content


def test_prints_confirmation(tmp_path: Path) -> None:
    result = _run("1.0.0", tmp_path=tmp_path)
    assert result.returncode == 0
    assert "bumped version to 1.0.0" in result.stdout


def test_only_replaces_project_version(tmp_path: Path) -> None:
    result = _run("9.9.9", TOML_MULTI_VERSION, tmp_path=tmp_path)
    assert result.returncode == 0
    content = (tmp_path / "pyproject.toml").read_text()
    assert 'version = "9.9.9"' in content
    # Build-system comment must remain untouched
    assert 'requires = ["hatchling>=1.0.0"]' in content


def test_idempotent_same_version(tmp_path: Path) -> None:
    result = _run("0.1.0", tmp_path=tmp_path)
    assert result.returncode == 0
    content = (tmp_path / "pyproject.toml").read_text()
    assert content.count('version = "0.1.0"') == 1


@pytest.mark.parametrize("version", ["0.0.1", "1.2.3", "10.20.30", "0.1.0"])
def test_accepts_valid_semver(version: str, tmp_path: Path) -> None:
    result = _run(version, tmp_path=tmp_path)
    assert result.returncode == 0


# ── Validation errors ──────────────────────────────────────────────────────────


@pytest.mark.parametrize("bad_version", ["abc", "1.2", "1.2.3.4", "v1.2.3", "1.2.x", ""])
def test_rejects_invalid_semver(bad_version: str, tmp_path: Path) -> None:
    result = _run(bad_version, tmp_path=tmp_path)
    assert result.returncode != 0
    assert "X.Y.Z" in result.stderr or "VERSION" in result.stderr


def test_error_when_no_version_in_toml(tmp_path: Path) -> None:
    result = _run("1.0.0", TOML_NO_VERSION, tmp_path=tmp_path)
    assert result.returncode != 0
    assert "version" in result.stderr.lower()


def test_error_missing_argument(tmp_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(SCRIPT)],
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "usage" in result.stderr.lower() or "VERSION" in result.stderr


# ── Atomic write ───────────────────────────────────────────────────────────────


def test_no_tmp_file_left_after_success(tmp_path: Path) -> None:
    _run("1.2.3", tmp_path=tmp_path)
    tmp_files = list(tmp_path.glob("*.tmp"))
    assert tmp_files == [], f"unexpected .tmp files: {tmp_files}"
