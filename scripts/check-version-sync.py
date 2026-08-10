#!/usr/bin/env python3
"""Check CMakeLists.txt vs conanfile.py version consistency (deps/version-sync)."""

import re
import sys


def main() -> int:
    cmake_text = open("CMakeLists.txt").read()
    cmake_match = re.search(r"project\s*\([^)]*VERSION\s+(\S+)", cmake_text)
    if not cmake_match:
        print("ERROR: could not find VERSION in CMakeLists.txt", file=sys.stderr)
        return 1
    cmake_ver = cmake_match.group(1)
    print(f"CMake version:  {cmake_ver}")

    conan_text = open("conanfile.py").read()
    conan_match = re.search(r'version\s*=\s*["\']([^"\']+)["\']', conan_text)
    if not conan_match:
        print("ERROR: could not find version in conanfile.py", file=sys.stderr)
        return 1
    conan_ver = conan_match.group(1)
    print(f"Conan version:  {conan_ver}")

    if cmake_ver != conan_ver:
        print(
            f"ERROR: version mismatch — CMakeLists.txt={cmake_ver}, conanfile.py={conan_ver}",
            file=sys.stderr,
        )
        return 1

    print("OK: versions match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
