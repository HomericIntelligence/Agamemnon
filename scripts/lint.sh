#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Configure first to get compile_commands.json. If the build directory is
# already configured, cmake will succeed; if not, we want the configure to
# attempt and report failure rather than be silently swallowed. Bucket A.
if ! cmake --preset debug -DAgamemnon_ENABLE_CLANG_TIDY=OFF >/dev/null 2>&1; then
  echo "warn: cmake --preset debug configure failed; proceeding with existing build/debug if present" >&2
fi
if [ ! -f "${ROOT_DIR}/build/debug/compile_commands.json" ]; then
  echo "error: build/debug/compile_commands.json not found; cannot run clang-tidy" >&2
  exit 1
fi

find "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/test" \
  -name "*.cpp" -o -name "*.hpp" | \
  xargs clang-tidy \
    -p "${ROOT_DIR}/build/debug" \
    --config-file="${ROOT_DIR}/.clang-tidy" \
    "$@"
