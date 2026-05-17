#!/bin/bash
# Check that commits modifying src/ or agamemnon/ also update CHANGELOG.md
# This is a warning-only hook (exit 0) to preserve developer workflow.

set -e

# Get the list of files changed in the current commit
changed_files=$(git diff --cached --name-only)

# Check if src/ or agamemnon/ was modified
src_or_agamemnon_changed=$(echo "$changed_files" | grep -E '^(src/|agamemnon/)' || true)

# If src/ or agamemnon/ was modified, check if CHANGELOG.md [Unreleased] was updated
if [ -n "$src_or_agamemnon_changed" ]; then
  changelog_changed=$(echo "$changed_files" | grep -E '^CHANGELOG\.md$' || true)

  if [ -z "$changelog_changed" ]; then
    # CHANGELOG.md not modified — check if it would be modified by a staged unstaging
    if ! git diff --cached CHANGELOG.md | grep -q '\[Unreleased\]'; then
      echo "⚠️  WARNING: commit touches src/ or agamemnon/ but does not modify CHANGELOG.md [Unreleased] block" >&2
      echo "   Please consider updating CHANGELOG.md with your changes." >&2
    fi
  fi
fi

exit 0
