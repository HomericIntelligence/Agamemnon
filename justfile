set shell := ["bash", "-c"]

default:
  @just --list

# Install Conan dependencies (cpp-httplib, nlohmann_json, gtest)
deps:
  uv run -- conan install . --output-folder=build/debug --profile=conan/profiles/debug --build=missing

# Install Conan dependencies for release
deps-release:
  uv run -- conan install . --output-folder=build/release --profile=conan/profiles/default --build=missing

# Install Conan dependencies for the coverage build (separate output folder)
deps-coverage:
  uv run -- conan install . --output-folder=build/coverage --profile=conan/profiles/debug --build=missing

build: deps
  uv run -- cmake --preset debug -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/c++ && uv run -- cmake --build --preset debug

test:
  uv run -- ctest --preset debug --output-on-failure

check-version:
  ./scripts/check-version-consistency.sh

lint: check-version
  ./scripts/lint.sh

format:
  ./scripts/format.sh

format-check:
  ./scripts/format.sh --check

actionlint:
  actionlint

agamemnon-test:
  cd agamemnon && uv run --group dev pytest tests/ -v

agamemnon-lint:
  cd agamemnon && uv run --group dev ruff check src/ tests/

agamemnon-typecheck:
  cd agamemnon && uv run --group dev mypy src/agamemnon/

coverage: deps-coverage
  uv run -- cmake --preset coverage && uv run -- cmake --build --preset coverage && ./scripts/coverage.sh

clean:
  rm -rf build install

docs-validate:
  ./scripts/validate-openapi.sh

ci:
  uv run -- cmake --preset ci && uv run -- cmake --build --preset ci && uv run -- ctest --preset ci

# Cut a release: bump version, commit, tag, and push
release VERSION push='true':
  #!/usr/bin/env bash
  set -euo pipefail
  if ! [[ "{{VERSION}}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: VERSION must be X.Y.Z (got '{{VERSION}}')" >&2
    exit 1
  fi
  ./scripts/check-release-readiness.sh "{{VERSION}}"
  python3 scripts/bump-version.py "{{VERSION}}"
  git add clients/python/pyproject.toml
  git commit -S -m "chore: bump version to v{{VERSION}}"
  git tag -s "v{{VERSION}}" -m "v{{VERSION}}"
  if [ "{{push}}" = "true" ]; then
    git push --follow-tags
  fi

# =============================================================================
# Containerized CI (podman-by-default)
# =============================================================================
# These recipes run the same checks as the GitHub Actions workflows, but inside
# the CI container image (ci/Containerfile) instead of on the native host.
# Engine: podman (rootless, preferred) or docker — auto-detected by
# scripts/run_ci_local.sh. Override with CONTAINER_ENGINE=docker.

# Build the CI container image (ci/Containerfile)
ci-build:
    podman build -f ci/Containerfile -t agamemnon-ci:local .

# Run the full required-check suite in the container
ci-check:
    ./scripts/run_ci_local.sh all

# Run lint (clang-format + clang-tidy + python lint + actionlint) in the container
ci-lint:
    ./scripts/run_ci_local.sh lint

# Run unit tests in the container
ci-unit:
    ./scripts/run_ci_local.sh unit

# Run integration tests in the container
ci-integration:
    ./scripts/run_ci_local.sh integration

# Run the cmake build in the container
ci-build-cpp:
    ./scripts/run_ci_local.sh build

# Run security scans (pip-audit + trivy + conan SBOM) in the container
ci-security:
    ./scripts/run_ci_local.sh security

# Run the gitleaks secrets scan in the container
ci-secrets:
    ./scripts/run_ci_local.sh secrets

# Run workflow schema validation in the container
ci-schema:
    ./scripts/run_ci_local.sh schema

# Run deps/version-sync in the container
ci-version-sync:
    ./scripts/run_ci_local.sh version-sync

# Run the release dry-run in the container
ci-release:
    ./scripts/run_ci_local.sh release

# Run uv lockfile checks in the container
ci-uv-check:
    ./scripts/run_ci_local.sh uv-check

# Run actionlint in the container
ci-actionlint:
    ./scripts/run_ci_local.sh actionlint
