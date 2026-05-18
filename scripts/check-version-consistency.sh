#!/usr/bin/env bash
# Verify that the version string in include/projectagamemnon/version.hpp
# matches the VERSION argument in CMakeLists.txt's project() call.
# Exits 0 on match, 1 on mismatch, 2 on parse failure.
set -euo pipefail

HEADER_PATH="${HEADER_PATH:-include/projectagamemnon/version.hpp}"
CMAKE_PATH="${CMAKE_PATH:-CMakeLists.txt}"

if [[ ! -f "$HEADER_PATH" ]]; then
  echo "ERROR: $HEADER_PATH not found" >&2
  exit 2
fi
if [[ ! -f "$CMAKE_PATH" ]]; then
  echo "ERROR: $CMAKE_PATH not found" >&2
  exit 2
fi

# Extract from version.hpp: matches `kVersion{"X.Y.Z"}` or `kVersion = "X.Y.Z"`
HEADER_VER=$(grep -oE 'kVersion[[:space:]]*[={][[:space:]]*"[0-9]+\.[0-9]+\.[0-9]+"' "$HEADER_PATH" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)

# Extract from CMakeLists.txt: matches `VERSION X.Y.Z` inside a project() block
# Use perl for multiline matching since project() may span multiple lines.
CMAKE_VER=$(perl -0777 -nE 'say $1 if /project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)/s' "$CMAKE_PATH" | head -1)

if [[ -z "$HEADER_VER" ]]; then
  echo "ERROR: could not extract version from $HEADER_PATH (looked for kVersion = \"X.Y.Z\")" >&2
  exit 2
fi
if [[ -z "$CMAKE_VER" ]]; then
  echo "ERROR: could not extract version from $CMAKE_PATH (looked for project(... VERSION X.Y.Z))" >&2
  exit 2
fi

if [[ "$HEADER_VER" != "$CMAKE_VER" ]]; then
  echo "ERROR: version mismatch -- $HEADER_PATH=$HEADER_VER vs $CMAKE_PATH=$CMAKE_VER" >&2
  exit 1
fi

echo "OK: version $HEADER_VER consistent across $HEADER_PATH and $CMAKE_PATH"
