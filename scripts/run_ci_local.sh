#!/bin/bash
# Run the Agamemnon CI suite locally inside a container.
#
# Mirrors what GitHub Actions runs, using the same CI container image
# (ci/Containerfile). Supports Podman (rootless, preferred) or Docker.
#
# Usage:
#   ./scripts/run_ci_local.sh                  # Run all CI checks
#   ./scripts/run_ci_local.sh lint             # clang-format + clang-tidy + python lint (lint job)
#   ./scripts/run_ci_local.sh unit             # build + ctest (unit-tests job)
#   ./scripts/run_ci_local.sh integration      # build + integration ctest (integration-tests job)
#   ./scripts/run_ci_local.sh build            # cmake build (build job)
#   ./scripts/run_ci_local.sh security         # pip-audit + trivy + conan SBOM scans
#   ./scripts/run_ci_local.sh secrets          # gitleaks (security-secrets-scan job)
#   ./scripts/run_ci_local.sh schema           # workflow schema validation
#   ./scripts/run_ci_local.sh version-sync     # CMakeLists vs conanfile version check
#   ./scripts/run_ci_local.sh release          # release dry-run validation
#   ./scripts/run_ci_local.sh uv-check         # lockfile sync checks
#   ./scripts/run_ci_local.sh actionlint       # workflow lint
#
# Container engine: auto-detected (podman first, docker fallback).
# Override: CONTAINER_ENGINE=docker ./scripts/run_ci_local.sh
#
# Image: uses 'agamemnon-ci:local'.
# Build locally: just ci-build  (or: podman build -f ci/Containerfile -t agamemnon-ci:local .)

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUBSET="${1:-all}"

LOCAL_IMAGE="agamemnon-ci:local"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[CI]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[CI]${NC} $*"; }
log_error() { echo -e "${RED}[CI]${NC} $*" >&2; }
log_step()  { echo -e "\n${BLUE}==>${NC} $*"; }

# ============================================================================
# Container engine detection
# ============================================================================

detect_engine() {
    if [ -n "${CONTAINER_ENGINE:-}" ]; then
        if ! command -v "${CONTAINER_ENGINE}" &> /dev/null; then
            log_error "CONTAINER_ENGINE=${CONTAINER_ENGINE} not found in PATH"
            exit 1
        fi
        log_info "Container engine: ${CONTAINER_ENGINE} (from env)"
        return
    fi

    if command -v podman &> /dev/null; then
        CONTAINER_ENGINE="podman"
        log_info "Container engine: podman (rootless)"
    elif command -v docker &> /dev/null; then
        CONTAINER_ENGINE="docker"
        log_info "Container engine: docker"
    else
        log_error "No container engine found. Install podman (recommended) or docker."
        exit 1
    fi
    export CONTAINER_ENGINE
}

# ============================================================================
# Image resolution
# ============================================================================

resolve_image() {
    if "${CONTAINER_ENGINE}" image exists "${LOCAL_IMAGE}" 2>/dev/null || \
       "${CONTAINER_ENGINE}" images -q "${LOCAL_IMAGE}" 2>/dev/null | grep -q .; then
        CI_IMAGE="${LOCAL_IMAGE}"
        log_info "Using local CI image: ${CI_IMAGE}"
    else
        log_error "Local image '${LOCAL_IMAGE}' not found."
        log_error "Build it first: just ci-build"
        exit 1
    fi
    export CI_IMAGE
}

# ============================================================================
# Run a command inside the CI container (workspace mounted at /workspace)
# ============================================================================

run_in_container() {
    local cmd=("$@")
    local engine_flags=()

    if [ "${CONTAINER_ENGINE}" = "podman" ]; then
        engine_flags+=(--userns=keep-id:uid=1000,gid=1000)
    fi

    "${CONTAINER_ENGINE}" run --rm \
        "${engine_flags[@]}" \
        --volume "${PROJECT_ROOT}:/workspace:Z" \
        --workdir /workspace \
        "${CI_IMAGE}" \
        "${cmd[@]}"
}

# ============================================================================
# CI steps (mirror .github/workflows/_required.yml)
# ============================================================================

run_lint() {
    log_step "lint: clang-format + clang-tidy + python lint + actionlint"
    run_in_container bash -c '
        set -euo pipefail
        conan profile detect --exist-ok
        ./scripts/format.sh --check
        ruff check clients/python/
        conan install . --build=missing -s build_type=Debug --output-folder build/debug >/dev/null
        cmake --preset debug -DAgamemnon_ENABLE_CLANG_TIDY=ON
        cmake --build --preset debug
        cd clients/python && uv sync --locked && uv run python -m pytest tests/test_ci_workflows.py -v && uv run mypy src/
        cd /workspace
        actionlint
    '
}

run_unit() {
    log_step "unit-tests: build + ctest"
    run_in_container bash -c '
        set -euo pipefail
        conan profile detect --exist-ok
        conan install . --build=missing -s build_type=Release --output-folder build/release >/dev/null
        cmake --preset release
        cmake --build --preset release
        ctest --preset release --output-on-failure --timeout 120
    '
}

run_integration() {
    log_step "integration-tests: build + integration ctest"
    run_in_container bash -c '
        set -euo pipefail
        conan profile detect --exist-ok
        conan install . --build=missing -s build_type=Release --output-folder build/release >/dev/null
        cmake --preset release
        cmake --build --preset release
        cd build/release
        if ctest -L integration --output-on-failure --dry-run 2>&1 | grep -q "No tests were found"; then
            echo "No integration label found — running full test suite"
            ctest --output-on-failure
        else
            ctest -L integration --output-on-failure
        fi
    '
}

run_build() {
    log_step "build: cmake configure + build"
    run_in_container bash -c '
        set -euo pipefail
        conan profile detect --exist-ok
        conan install . --build=missing -s build_type=Release --output-folder build/release >/dev/null
        cmake --preset release
        cmake --build --preset release
    '
}

run_security() {
    log_step "security/dependency-scan: pip-audit + trivy + conan SBOM"
    run_in_container bash -c '
        set -euo pipefail
        cd clients/python && uv run --only-group lint pip-audit --skip-editable
        cd /workspace
        trivy fs --exit-code 1 --severity HIGH,CRITICAL --scanners vuln .
        conan profile detect --exist-ok
        conan lock create conanfile.py --lockfile-out=conan.lock --build=missing >/dev/null
        syft . --override-default-catalogers conan -o cyclonedx-json=conan-sbom.cdx.json
        grype sbom:conan-sbom.cdx.json --fail-on high --output table
        grype sbom:.github/cpp-fetchcontent-deps.cdx.json --fail-on high --output table
    '
}

run_secrets() {
    log_step "security/secrets-scan: gitleaks"
    run_in_container bash -c '
        set -euo pipefail
        if [ -f .gitleaks.toml ]; then
            gitleaks detect --source . --config .gitleaks.toml --report-format sarif --report-path gitleaks.sarif --exit-code 1
        else
            gitleaks detect --source . --report-format sarif --report-path gitleaks.sarif --exit-code 1
        fi
    '
}

run_schema() {
    log_step "schema-validation: workflow schemas"
    run_in_container bash -c '
        set -euo pipefail
        find .github/workflows -name "*.yml" | sort | \
            xargs check-jsonschema --schemafile .github/schemas/github-workflow.json
    '
}

run_version_sync() {
    log_step "deps/version-sync: CMakeLists vs conanfile version check"
    run_in_container python3 scripts/check-version-sync.py
}

run_release() {
    log_step "release: dry-run validation"
    run_in_container python3 scripts/check-release-readiness.py
}

run_uv_check() {
    log_step "uv-check: lockfile sync"
    run_in_container bash -c '
        set -euo pipefail
        uv lock --check
        cd agamemnon && uv lock --check
        cd ../clients/python && uv lock --check
    '
}

run_actionlint() {
    log_step "actionlint: workflow lint"
    run_in_container actionlint
}

# ============================================================================
# Main
# ============================================================================

FAILED=()

run_step() {
    local name="$1"
    local fn="$2"
    if ! "${fn}"; then
        FAILED+=("${name}")
        log_error "${name} FAILED"
    fi
}

detect_engine
resolve_image

log_info "CI subset: ${SUBSET}"
log_info "Project root: ${PROJECT_ROOT}"

case "${SUBSET}" in
    lint)
        run_step "lint" run_lint
        ;;
    unit)
        run_step "unit-tests" run_unit
        ;;
    integration)
        run_step "integration-tests" run_integration
        ;;
    build)
        run_step "build" run_build
        ;;
    security)
        run_step "security/dependency-scan" run_security
        ;;
    secrets)
        run_step "security/secrets-scan" run_secrets
        ;;
    schema)
        run_step "schema-validation" run_schema
        ;;
    version-sync)
        run_step "deps/version-sync" run_version_sync
        ;;
    release)
        run_step "release" run_release
        ;;
    uv-check)
        run_step "uv-check" run_uv_check
        ;;
    actionlint)
        run_step "actionlint" run_actionlint
        ;;
    all)
        run_step "lint" run_lint
        run_step "uv-check" run_uv_check
        run_step "build" run_build
        run_step "unit-tests" run_unit
        run_step "integration-tests" run_integration
        run_step "security/dependency-scan" run_security
        run_step "security/secrets-scan" run_secrets
        run_step "schema-validation" run_schema
        run_step "deps/version-sync" run_version_sync
        run_step "release" run_release
        ;;
    *)
        log_error "Unknown subset: ${SUBSET}"
        log_error "Valid values: all, lint, unit, integration, build, security, secrets, schema, version-sync, release, uv-check, actionlint"
        exit 1
        ;;
esac

echo ""
if [ "${#FAILED[@]}" -eq 0 ]; then
    log_info "All CI checks passed."
else
    log_error "Failed: ${FAILED[*]}"
    exit 1
fi
