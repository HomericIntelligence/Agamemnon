set shell := ["bash", "-c"]

default:
  @just --list

# Install Conan dependencies (cpp-httplib, nlohmann_json, gtest)
deps:
  conan install . --output-folder=build/debug --profile=conan/profiles/debug --build=missing

# Install Conan dependencies for release
deps-release:
  conan install . --output-folder=build/release --profile=conan/profiles/default --build=missing

build: deps
  cmake --preset debug && cmake --build --preset debug

test:
  ctest --preset debug --output-on-failure

lint:
  ./scripts/lint.sh

format:
  ./scripts/format.sh

format-check:
  ./scripts/format.sh --check

coverage:
  cmake --preset coverage && cmake --build --preset coverage && ./scripts/coverage.sh

clean:
  rm -rf build install

docs-validate:
  ./scripts/validate-openapi.sh

ci:
  cmake --preset ci && cmake --build --preset ci && ctest --preset ci
