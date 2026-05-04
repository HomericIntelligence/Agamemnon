#!/usr/bin/env python3
"""Rewrite the version field in clients/python/pyproject.toml."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


def bump_version(toml_path: Path, new_version: str) -> None:
    """Replace the version line under [project] in *toml_path* with *new_version*.

    Raises SystemExit if the version line is not found.
    """
    text = toml_path.read_text(encoding="utf-8")

    in_project = False
    lines = text.splitlines(keepends=True)
    new_lines: list[str] = []
    replaced = False

    for line in lines:
        if re.match(r"^\[project\]", line):
            in_project = True
        elif re.match(r"^\[", line):
            in_project = False

        if in_project and not replaced and re.match(r'^version\s*=\s*"[^"]*"', line):
            new_lines.append(f'version = "{new_version}"\n')
            replaced = True
        else:
            new_lines.append(line)

    if not replaced:
        print(
            f"error: version line not found under [project] in {toml_path}",
            file=sys.stderr,
        )
        sys.exit(1)

    tmp = toml_path.with_suffix(".toml.tmp")
    tmp.write_text("".join(new_lines), encoding="utf-8")
    os.replace(tmp, toml_path)
    print(f"bumped version to {new_version}")


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} VERSION", file=sys.stderr)
        sys.exit(1)

    new_version = sys.argv[1]
    if not re.fullmatch(r"\d+\.\d+\.\d+", new_version):
        print(
            f"error: VERSION must be X.Y.Z (got '{new_version}')",
            file=sys.stderr,
        )
        sys.exit(1)

    repo_root = Path(__file__).resolve().parent.parent
    toml_path = repo_root / "clients" / "python" / "pyproject.toml"

    if not toml_path.exists():
        print(f"error: {toml_path} does not exist", file=sys.stderr)
        sys.exit(1)

    bump_version(toml_path, new_version)


if __name__ == "__main__":
    main()
