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

# Focused native Fleet/API tests; CMAKE_PREFIX_PATH can point at cached dependencies.
fleet-test:
  cmake -S test/fleet -B build/fleet
  cmake --build build/fleet --parallel 2
  ctest --test-dir build/fleet --output-on-failure

fleet-client-test python='python3':
  PYTHONPATH="clients/python/src${PYTHONPATH:+:$PYTHONPATH}" {{python}} -m unittest discover -s clients/python/tests -p 'test_fleet*.py' -v

# Exports only actual FleetService-produced envelopes from the bounded contract test.
fleet-export output:
  FLEET_CONTRACT_OUTPUT='{{output}}' build/fleet/fleet_contract_tests --gtest_filter=FleetRoutes.ExportLifecycleEnvelopes

# Export actual subordinate-build admission, durable grant and cancellation responses.
fleet-build-export output:
  FLEET_BUILD_CONTRACT_OUTPUT='{{output}}' build/fleet/fleet_contract_tests --gtest_filter=FleetBuildExportRoutes.ExportBuildContract

# Check independently captured bridge facts against the actual native controller.
fleet-import input:
  build/fleet/fleet_fact_import '{{input}}'

# Link the real server using an existing nats.c build, without dependency fetching.
fleet-native-build nats_prefix:
  cmake -S test/fleet -B build/fleet -DFLEET_NATIVE_NATS_PREFIX='{{nats_prefix}}'
  cmake --build build/fleet --parallel 2
  ctest --test-dir build/fleet --output-on-failure

fleet-nats-test:
  cmake -S test/fleet -B build/fleet
  cmake --build build/fleet --target fleet_nats_endpoint_tests --parallel 2
  ctest --test-dir build/fleet -R fleet_nats_endpoint --output-on-failure

# Launches a fresh loopback-only broker with private ephemeral storage.
fleet-jetstream-test nats_server='nats-server' python='python3':
  {{python}} test/fleet/private_jetstream.py '{{nats_server}}' build/fleet/fleet_jetstream_tests

fleet-startup-test evidence_root python='python3':
  {{python}} test/fleet/startup_preflight.py --binary build/fleet/fleet_native_server --root '{{evidence_root}}'

fleet-format clang_format='clang-format':
  {{clang_format}} -i include/agamemnon/fleet.hpp src/fleet.cpp test/src/test_fleet.cpp include/agamemnon/projects.hpp src/projects.cpp test/src/test_projects.cpp test/fleet/import_bridge_facts.cpp

fleet-format-check clang_format='clang-format':
  {{clang_format}} --dry-run --Werror include/agamemnon/fleet.hpp src/fleet.cpp test/src/test_fleet.cpp include/agamemnon/projects.hpp src/projects.cpp test/src/test_projects.cpp test/fleet/import_bridge_facts.cpp

fleet-client-lint python='python3':
  {{python}} -m ruff check clients/python/src/agamemnon_client/client.py clients/python/tests/test_fleet_client.py clients/python/tests/test_fleet_build_transport.py
  {{python}} -m ruff format --check clients/python/src/agamemnon_client/client.py clients/python/tests/test_fleet_client.py clients/python/tests/test_fleet_build_transport.py

fleet-client-format python='python3':
  {{python}} -m ruff format clients/python/src/agamemnon_client/client.py clients/python/tests/test_fleet_client.py clients/python/tests/test_fleet_build_transport.py

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

# Controlled installer contracts; no image, network or dependency installation.
ci-tools-test python='python3':
    PYTHONDONTWRITEBYTECODE=1 {{python}} -m unittest discover -s test/ci -p test_ci_tools.py -v

# Actual pinned uv with private local wheels; no network, container or scanner.
ci-audit-test python='python3' uv='uv':
    PYTHONDONTWRITEBYTECODE=1 UV_AUDIT_TEST_BINARY='{{uv}}' {{python}} -m unittest discover -s test/ci -p test_ci_audit.py -v

# Check report ignore boundaries with private Git repositories; no scanners run.
ci-reports-test python='python3':
    PYTHONDONTWRITEBYTECODE=1 {{python}} -m unittest discover -s test/ci -p test_ci_reports.py -v

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

# Bounded attachment tests in an already provisioned Python 3.11+ environment.
fleet-attachment-test python='python3':
  PYTHONPATH="agamemnon/src${PYTHONPATH:+:$PYTHONPATH}" {{python}} -m pytest agamemnon/tests/test_fleetd_attachment.py agamemnon/tests/test_fleetd_observations.py -q

# Preserve argument boundaries; the adapter only consumes previously admitted work.
[positional-arguments]
fleet-attachment-run *args:
  PYTHONPATH="agamemnon/src${PYTHONPATH:+:$PYTHONPATH}" "${FLEET_ATTACHMENT_PYTHON:-python3}" -m agamemnon.fleetd "$@"

# Builds the fixture-only receiver for the explicit Telemachy cross-repo contract.
fleet-epic-import-build:
  cmake -S test/fleet -B build/fleet
  cmake --build build/fleet --target fleet_epic_import --parallel 2
