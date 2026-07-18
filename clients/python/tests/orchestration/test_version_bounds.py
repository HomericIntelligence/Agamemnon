"""Regression test: dev dependencies in pyproject.toml must have bounded version specs.

Migrated from the former pixi.toml-based check (ADR-018: uv replaces pixi).

Scope note: the original pixi check scanned the ``pypi-dependencies`` and
``feature.dev.pypi-dependencies`` tables — i.e. the client's declared deps and
its ``dev`` tooling — but *not* the ``feature.lint`` env (pip-audit/pip/msgpack,
which are upstream-unbounded security tools). This port preserves that scope by
checking ``[project.optional-dependencies].dev`` and ``[dependency-groups].dev``,
and deliberately skips the ``lint`` group and the library's own runtime
``[project.dependencies]`` (which the pixi ``pypi-dependencies`` table mirrored
but the check never scanned directly).

Each requirement is parsed as a PEP 508 string; the environment marker
(``; python_version >= '3.10'``) is stripped before checking the version
specifier. Path/URL-sourced packages (e.g. the sibling orchestration package
pulled in via ``[tool.uv.sources]``) carry no version specifier and are exempt.
"""

from pathlib import Path

import pytest
import tomllib

PYPROJECT = Path(__file__).resolve().parents[2] / "pyproject.toml"

# Dependency-group names to enforce bounds on (mirrors the old pixi feature.dev
# scope). The `lint` group holds upstream-unbounded security tooling and is
# intentionally excluded, matching the pre-migration behaviour.
CHECKED_GROUPS = ("dev",)


def _spec_of(requirement: str) -> str:
    """Return the version-specifier portion of a PEP 508 requirement string."""
    base = requirement.split(";", 1)[0].strip()
    for i, ch in enumerate(base):
        if ch in "<>=!~":
            return base[i:].strip()
    return ""


def _bare_name(requirement: str) -> str:
    """Return the bare package name from a PEP 508 requirement string."""
    name = requirement.split(";", 1)[0].strip()
    for i, ch in enumerate(name):
        if ch in "<>=!~[ ":
            return name[:i].strip()
    return name.strip()


def _sourced_packages() -> set[str]:
    """Names declared under [tool.uv.sources] (path/URL deps, version-exempt)."""
    with open(PYPROJECT, "rb") as f:
        data = tomllib.load(f)
    sources = data.get("tool", {}).get("uv", {}).get("sources", {})
    return {name.lower() for name in sources}


def _load_deps() -> list[tuple[str, str, str]]:
    """Yield (section, package, spec) for every in-scope dependency."""
    with open(PYPROJECT, "rb") as f:
        data = tomllib.load(f)

    exempt = _sourced_packages()
    results: list[tuple[str, str, str]] = []

    def _add(section: str, requirements: list[str]) -> None:
        for req in requirements:
            name = _bare_name(req)
            if name.lower() in exempt:
                continue
            results.append((section, name, _spec_of(req)))

    project = data.get("project", {})
    _add(
        "project.optional-dependencies.dev",
        project.get("optional-dependencies", {}).get("dev", []),
    )
    groups = data.get("dependency-groups", {})
    for group in CHECKED_GROUPS:
        _add(f"dependency-groups.{group}", groups.get(group, []))

    return results


ALL_DEPS = _load_deps()


@pytest.mark.parametrize(
    "section,pkg,spec",
    ALL_DEPS,
    ids=[f"{s}::{p}" for s, p, _ in ALL_DEPS],
)
def test_has_upper_bound(section: str, pkg: str, spec: str) -> None:
    """Every versioned dependency must declare a major-version upper bound."""
    # A dependency with an environment marker but no version (e.g. a
    # conditional path/URL source) carries no specifier and is exempt.
    if spec == "":
        pytest.skip(f"{section} :: {pkg} has no version specifier (path/URL source)")
    assert "<" in spec, (
        f'{section} :: {pkg} = "{spec}" is missing an upper bound (<). '
        "All dependencies must have a major-version upper bound."
    )


@pytest.mark.parametrize(
    "section,pkg,spec",
    ALL_DEPS,
    ids=[f"{s}::{p}" for s, p, _ in ALL_DEPS],
)
def test_no_wildcard(section: str, pkg: str, spec: str) -> None:
    """Wildcard version specifiers are not allowed for dependencies."""
    assert spec.strip() != "*", (
        f'{section} :: {pkg} = "*" — wildcard specifiers are not allowed.'
    )
