#!/usr/bin/env bash
set -euo pipefail

cache_dir="${HOME:?HOME must be set}/.conan2"
mkdir -p -- "$cache_dir"
test -d "$cache_dir"
