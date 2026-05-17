#!/usr/bin/env bash
# scripts/lib/git-safety.sh — pre-commit safety guards
#
# Provides helpers to detect no-op git commit scenarios (empty staging area)
# and report the current HEAD SHA clearly, preventing false-positive "commit
# succeeded" reports when nothing was actually staged.
#
# Background: the 29-auto-impl branch exhibited a case where `git commit`
# printed an already-existing SHA because nothing was staged (see issue #142).
# These helpers make that scenario detectable and diagnosable before commit.
#
# Usage:
#   source "${SCRIPT_DIR}/lib/git-safety.sh"
#   assert_staged_nonempty || exit 1
#   git commit -m "..."
#   report_commit_sha

set -euo pipefail

# assert_staged_nonempty — exit non-zero with a clear diagnostic if the
# staging area is empty, so callers can abort before running `git commit`
# (which would silently succeed and report the existing HEAD SHA).
#
# Returns: 0 if something is staged; 1 (with diagnostic) if nothing is staged.
assert_staged_nonempty() {
    if git diff --staged --quiet 2>/dev/null; then
        echo "ERROR: Nothing is staged for commit." >&2
        echo "  git commit would produce a no-op and report the existing HEAD SHA." >&2
        echo "  Current HEAD: $(git log --oneline -1 2>/dev/null || echo '(no commits yet)')" >&2
        echo "  Staged files: (none)" >&2
        echo "  Run 'git status' to see what is modified but unstaged." >&2
        return 1
    fi
    return 0
}

# report_commit_sha — print the SHA of the most recent commit with a clear
# label, so callers can distinguish a new commit from an already-existing one.
#
# Usage: call this AFTER `git commit` to log the resulting SHA.
report_commit_sha() {
    local sha
    sha="$(git rev-parse HEAD 2>/dev/null || echo 'unknown')"
    local oneline
    oneline="$(git log --oneline -1 2>/dev/null || echo 'unknown')"
    echo "Committed: ${sha} — ${oneline}"
}

# is_head_already_pushed — return 0 if the current HEAD has already been
# pushed to the given remote branch, 1 otherwise.
#
# Usage: is_head_already_pushed origin my-branch
is_head_already_pushed() {
    local remote="${1:-origin}"
    local branch="${2:-$(git rev-parse --abbrev-ref HEAD 2>/dev/null)}"
    local local_sha remote_sha
    local_sha="$(git rev-parse HEAD 2>/dev/null || echo '')"
    remote_sha="$(git ls-remote "${remote}" "refs/heads/${branch}" 2>/dev/null | awk '{print $1}')"
    if [[ -n "${remote_sha}" && "${local_sha}" == "${remote_sha}" ]]; then
        return 0
    fi
    return 1
}
