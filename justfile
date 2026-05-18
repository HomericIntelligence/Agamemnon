set shell := ["bash", "-c"]

default:
  @just --list

# Install Conan dependencies (cpp-httplib, nlohmann_json, gtest)
deps:
  pixi run -- conan install . --output-folder=build/debug --profile=conan/profiles/debug --build=missing

# Install Conan dependencies for release
deps-release:
  pixi run -- conan install . --output-folder=build/release --profile=conan/profiles/default --build=missing

# Install Conan dependencies for the coverage build (separate output folder)
deps-coverage:
  pixi run -- conan install . --output-folder=build/coverage --profile=conan/profiles/debug --build=missing

build: deps
  pixi run -- cmake --preset debug && pixi run -- cmake --build --preset debug

test:
  pixi run -- ctest --preset debug --output-on-failure

lint:
  ./scripts/lint.sh

format:
  ./scripts/format.sh

format-check:
  ./scripts/format.sh --check

actionlint:
  actionlint

agamemnon-test:
  cd agamemnon && pixi run test

agamemnon-lint:
  cd agamemnon && pixi run lint

agamemnon-typecheck:
  cd agamemnon && pixi run typecheck

coverage: deps-coverage
  pixi run -- cmake --preset coverage && pixi run -- cmake --build --preset coverage && ./scripts/coverage.sh

clean:
  rm -rf build install

docs-validate:
  ./scripts/validate-openapi.sh

ci:
  pixi run -- cmake --preset ci && pixi run -- cmake --build --preset ci && pixi run -- ctest --preset ci

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
