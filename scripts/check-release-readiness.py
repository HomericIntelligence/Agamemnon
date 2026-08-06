#!/usr/bin/env python3
"""Release dry-run validation (release job).

Checks version consistency across CMakeLists.txt, conanfile.py, and
include/agamemnon/version.hpp, plus a CHANGELOG Unreleased section. Does NOT
publish anything.
"""

import re
import sys


def read(path):
    with open(path) as f:
        return f.read()


def main() -> int:
    errors = []

    cmake = read("CMakeLists.txt")
    m = re.search(r"project\s*\([^)]*VERSION\s+(\S+)", cmake)
    cmake_ver = m.group(1) if m else None
    if not cmake_ver:
        errors.append("could not find VERSION in CMakeLists.txt")

    conan = read("conanfile.py")
    m = re.search(r'version\s*=\s*["\']([^"\']+)["\']', conan)
    conan_ver = m.group(1) if m else None
    if not conan_ver:
        errors.append("could not find version in conanfile.py")

    header = read("include/agamemnon/version.hpp")
    m = re.search(r'kVersion\{"([^"]+)"\}', header)
    hdr_ver = m.group(1) if m else None
    if not hdr_ver:
        errors.append("could not find kVersion in version.hpp")

    versions = {
        "CMakeLists.txt": cmake_ver,
        "conanfile.py": conan_ver,
        "version.hpp": hdr_ver,
    }
    print("Release version sources:")
    for k, v in versions.items():
        print(f"  {k:18} {v}")

    uniq = {v for v in versions.values() if v}
    if len(uniq) > 1:
        errors.append(f"version mismatch across release sources: {versions}")

    if cmake_ver and not re.fullmatch(r"\d+\.\d+\.\d+", cmake_ver):
        errors.append(f"version '{cmake_ver}' is not semver MAJOR.MINOR.PATCH")

    changelog = read("CHANGELOG.md")
    if "## [Unreleased]" not in changelog:
        errors.append("CHANGELOG.md has no '## [Unreleased]' section")

    if errors:
        for e in errors:
            print(f"::error::{e}", file=sys.stderr)
        return 1

    print(f"\nOK: release dry-run validated version {cmake_ver}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
