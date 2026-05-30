#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

mkdir -p "${ROOT_DIR}/build/coverage-report"

# --gcov-ignore-parse-errors=negative_hits.warn_once_per_file:
# The ported work-stealing scheduler (src/concurrency/work_stealing_scheduler.cpp)
# triggers a known gcov tool bug (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68080)
# that emits a malformed "branch N taken -1" with a negative hit count. Without
# this flag gcovr aborts (exit 64) before writing any report. Per gcovr's own
# guidance we downgrade the parse error to a once-per-file warning so the real
# coverage of every other line is still measured. This does NOT suppress or
# exclude any file from the coverage numbers.
gcovr \
  --root "${ROOT_DIR}" \
  --filter "${ROOT_DIR}/include" \
  --filter "${ROOT_DIR}/src" \
  --exclude "${ROOT_DIR}/src/server_main.cpp" \
  --exclude "${ROOT_DIR}/src/nats_client.cpp" \
  --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
  --html-details "${ROOT_DIR}/build/coverage-report/index.html" \
  --xml "${ROOT_DIR}/build/coverage-report/coverage.xml" \
  --print-summary \
  "${ROOT_DIR}/build/coverage"
