"""Tests for scripts/bump-version.py.

The tests in this file come in two flavors:

* **Subprocess tests** (the original) exercise the script end-to-end via a
  patched copy in ``tmp_path``. These verify CLI behavior but cannot be
  measured by ``coverage.py`` because the subprocess copy lives outside the
  configured ``[tool.coverage.run] source`` list.
* **Direct-call tests** (added for #104) import the script as a module via
  :func:`importlib.util.spec_from_file_location` and exercise
  ``bump_version`` / ``main`` in-process so the file appears in coverage
  reports when ``--cov`` is pointed at ``scripts/bump-version.py``.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "bump-version.py"


def _load_bump_version_module() -> Any:
    """Import scripts/bump-version.py as a module despite its hyphenated name."""
    spec = importlib.util.spec_from_file_location("_bump_version_under_test", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bump_version_module = _load_bump_version_module()

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
    toml2 = tmp_path / "agamemnon_pyproject.toml"
    content = toml_content if toml_content is not None else MINIMAL_TOML
    toml.write_text(content, encoding="utf-8")
    toml2.write_text(content, encoding="utf-8")

    # Patch the script so it targets our tmp pyproject.toml(s)
    script_src = SCRIPT.read_text(encoding="utf-8")
    patched_src = script_src.replace(
        'repo_root / "clients" / "python" / "pyproject.toml",\n'
        '        repo_root / "agamemnon" / "pyproject.toml",',
        f'Path(r"{toml}"),\n        Path(r"{toml2}"),',
    )
    if patched_src == script_src:
        # Fallback for older single-file layout (kept for diff robustness).
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


# ── Direct-call tests (for coverage measurement, see #104) ────────────────────
#
# These tests import scripts/bump-version.py as a module so coverage.py can
# trace it. They complement the subprocess tests above which exercise CLI
# behavior end-to-end.


def test_direct_bump_version_replaces_version(tmp_path: Path) -> None:
    toml = tmp_path / "pyproject.toml"
    toml.write_text(MINIMAL_TOML, encoding="utf-8")

    bump_version_module.bump_version(toml, "2.3.4")

    content = toml.read_text(encoding="utf-8")
    assert 'version = "2.3.4"' in content


def test_direct_bump_version_only_replaces_project_section(tmp_path: Path) -> None:
    toml = tmp_path / "pyproject.toml"
    toml.write_text(TOML_MULTI_VERSION, encoding="utf-8")

    bump_version_module.bump_version(toml, "9.9.9")

    content = toml.read_text(encoding="utf-8")
    assert 'version = "9.9.9"' in content
    assert 'requires = ["hatchling>=1.0.0"]' in content


def test_direct_bump_version_exits_when_no_version(tmp_path: Path) -> None:
    toml = tmp_path / "pyproject.toml"
    toml.write_text(TOML_NO_VERSION, encoding="utf-8")

    with pytest.raises(SystemExit) as exc_info:
        bump_version_module.bump_version(toml, "1.0.0")
    assert exc_info.value.code == 1


def test_direct_bump_version_removes_tmp_file(tmp_path: Path) -> None:
    toml = tmp_path / "pyproject.toml"
    toml.write_text(MINIMAL_TOML, encoding="utf-8")

    bump_version_module.bump_version(toml, "1.2.3")

    assert list(tmp_path.glob("*.tmp")) == []


def test_direct_main_missing_argument(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "argv", ["bump-version.py"])
    with pytest.raises(SystemExit) as exc_info:
        bump_version_module.main()
    assert exc_info.value.code == 1


@pytest.mark.parametrize("bad_version", ["abc", "1.2", "1.2.3.4", "v1.2.3", ""])
def test_direct_main_rejects_invalid_semver(
    bad_version: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(sys, "argv", ["bump-version.py", bad_version])
    with pytest.raises(SystemExit) as exc_info:
        bump_version_module.main()
    assert exc_info.value.code == 1


def test_direct_main_missing_toml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # Redirect the module's __file__ so repo_root resolves into tmp_path
    # (which has no clients/python/pyproject.toml).
    fake_script = tmp_path / "scripts" / "bump-version.py"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("# placeholder\n", encoding="utf-8")

    monkeypatch.setattr(bump_version_module, "__file__", str(fake_script))
    monkeypatch.setattr(sys, "argv", ["bump-version.py", "1.2.3"])

    with pytest.raises(SystemExit) as exc_info:
        bump_version_module.main()
    assert exc_info.value.code == 1


def test_direct_main_happy_path(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    # Build a fake repo layout: <tmp_path>/scripts/bump-version.py,
    # <tmp_path>/clients/python/pyproject.toml, and
    # <tmp_path>/agamemnon/pyproject.toml. Redirect the module's
    # ``__file__`` so ``repo_root`` resolves into <tmp_path>.
    fake_script = tmp_path / "scripts" / "bump-version.py"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("# placeholder\n", encoding="utf-8")

    toml = tmp_path / "clients" / "python" / "pyproject.toml"
    toml.parent.mkdir(parents=True)
    toml.write_text(MINIMAL_TOML, encoding="utf-8")

    toml_orch = tmp_path / "agamemnon" / "pyproject.toml"
    toml_orch.parent.mkdir(parents=True)
    toml_orch.write_text(MINIMAL_TOML, encoding="utf-8")

    monkeypatch.setattr(bump_version_module, "__file__", str(fake_script))
    monkeypatch.setattr(sys, "argv", ["bump-version.py", "5.6.7"])

    bump_version_module.main()

    assert 'version = "5.6.7"' in toml.read_text(encoding="utf-8")
    assert 'version = "5.6.7"' in toml_orch.read_text(encoding="utf-8")
    captured = capsys.readouterr().out
    assert captured.count("bumped version to 5.6.7") == 2


def test_direct_main_errors_when_orchestration_toml_missing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """main() must fail if agamemnon/pyproject.toml is missing (dual-file lockstep)."""
    fake_script = tmp_path / "scripts" / "bump-version.py"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("# placeholder\n", encoding="utf-8")

    toml = tmp_path / "clients" / "python" / "pyproject.toml"
    toml.parent.mkdir(parents=True)
    toml.write_text(MINIMAL_TOML, encoding="utf-8")
    # Intentionally omit agamemnon/pyproject.toml.

    monkeypatch.setattr(bump_version_module, "__file__", str(fake_script))
    monkeypatch.setattr(sys, "argv", ["bump-version.py", "5.6.7"])

    with pytest.raises(SystemExit) as exc_info:
        bump_version_module.main()
    assert exc_info.value.code == 1


def test_direct_main_bumps_both_pyproject_files(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Regression guard: main() must update BOTH client and orchestration files."""
    fake_script = tmp_path / "scripts" / "bump-version.py"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("# placeholder\n", encoding="utf-8")

    client_toml = tmp_path / "clients" / "python" / "pyproject.toml"
    client_toml.parent.mkdir(parents=True)
    client_toml.write_text(MINIMAL_TOML, encoding="utf-8")

    orch_toml = tmp_path / "agamemnon" / "pyproject.toml"
    orch_toml.parent.mkdir(parents=True)
    orch_toml.write_text(MINIMAL_TOML, encoding="utf-8")

    monkeypatch.setattr(bump_version_module, "__file__", str(fake_script))
    monkeypatch.setattr(sys, "argv", ["bump-version.py", "9.8.7"])

    bump_version_module.main()

    assert 'version = "9.8.7"' in client_toml.read_text(encoding="utf-8")
    assert 'version = "9.8.7"' in orch_toml.read_text(encoding="utf-8")


# ---------------------------------------------------------------------------
# sync_security_md
# ---------------------------------------------------------------------------

SECURITY_MD = """\
# Security Policy

## Supported Versions

| Version | Status  |
| ------- | ------- |
| 0.1.0   | Current |
| 0.0.9   | EOL     |
"""


def test_sync_security_md_replaces_first_row(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    sec = tmp_path / "SECURITY.md"
    sec.write_text(SECURITY_MD, encoding="utf-8")

    bump_version_module.sync_security_md(sec, "1.2.3")

    contents = sec.read_text(encoding="utf-8")
    assert "| 1.2.3   |" in contents
    # Older row must remain untouched (count=1 only replaces first match).
    assert "| 0.0.9   | EOL     |" in contents
    assert "synced SECURITY.md version to 1.2.3" in capsys.readouterr().out


def test_sync_security_md_noop_when_already_current(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    sec = tmp_path / "SECURITY.md"
    sec.write_text(SECURITY_MD, encoding="utf-8")
    mtime_before = sec.stat().st_mtime_ns

    bump_version_module.sync_security_md(sec, "0.1.0")

    # No textual change -> no write -> no confirmation print.
    assert sec.stat().st_mtime_ns == mtime_before
    assert "synced SECURITY.md" not in capsys.readouterr().out


def test_sync_security_md_missing_file_is_noop(tmp_path: Path) -> None:
    missing = tmp_path / "does-not-exist.md"

    # Must not raise; must not create the file.
    bump_version_module.sync_security_md(missing, "9.9.9")

    assert not missing.exists()


def test_direct_main_happy_path_also_syncs_security_md(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake_script = tmp_path / "scripts" / "bump-version.py"
    fake_script.parent.mkdir(parents=True)
    fake_script.write_text("# placeholder\n", encoding="utf-8")

    toml = tmp_path / "clients" / "python" / "pyproject.toml"
    toml.parent.mkdir(parents=True)
    toml.write_text(MINIMAL_TOML, encoding="utf-8")

    sec = tmp_path / "SECURITY.md"
    sec.write_text(SECURITY_MD, encoding="utf-8")

    monkeypatch.setattr(bump_version_module, "__file__", str(fake_script))
    monkeypatch.setattr(sys, "argv", ["bump-version.py", "7.8.9"])

    bump_version_module.main()

    assert 'version = "7.8.9"' in toml.read_text(encoding="utf-8")
    assert "| 7.8.9   |" in sec.read_text(encoding="utf-8")
