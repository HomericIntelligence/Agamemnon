"""Regression test: all pypi dependencies in pixi.toml must have bounded version specs."""

from pathlib import Path

import pytest
import tomllib

PIXI_TOML = Path(__file__).resolve().parents[2] / "pixi.toml"


def _load_pypi_deps() -> list[tuple[str, str, str]]:
    """Yield (section, package, spec) for all pypi-dependencies."""
    with open(PIXI_TOML, "rb") as f:
        data = tomllib.load(f)

    results: list[tuple[str, str, str]] = []

    for pkg, spec in data.get("pypi-dependencies", {}).items():
        if isinstance(spec, dict):
            continue
        results.append(("pypi-dependencies", pkg, spec))

    dev_deps = data.get("feature", {}).get("dev", {}).get("pypi-dependencies", {})
    for pkg, spec in dev_deps.items():
        if isinstance(spec, dict):
            continue
        results.append(("feature.dev.pypi-dependencies", pkg, spec))

    return results


ALL_DEPS = _load_pypi_deps()


@pytest.mark.parametrize(
    "section,pkg,spec",
    ALL_DEPS,
    ids=[f"{s}::{p}" for s, p, _ in ALL_DEPS],
)
def test_has_upper_bound(section: str, pkg: str, spec: str) -> None:
    """Every pypi dependency must declare a major-version upper bound."""
    assert "<" in spec, (
        f'{section} :: {pkg} = "{spec}" is missing an upper bound (<). '
        "All pypi dependencies must have a major-version upper bound."
    )


@pytest.mark.parametrize(
    "section,pkg,spec",
    ALL_DEPS,
    ids=[f"{s}::{p}" for s, p, _ in ALL_DEPS],
)
def test_no_wildcard(section: str, pkg: str, spec: str) -> None:
    """Wildcard version specifiers are not allowed for pypi dependencies."""
    assert spec.strip() != "*", (
        f'{section} :: {pkg} = "*" — wildcard specifiers are not allowed.'
    )
