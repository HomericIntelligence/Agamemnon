#!/usr/bin/env bash
# Verify all prerequisites are in place before pushing a v* tag.
# Exits non-zero if any check fails.
set -euo pipefail

REPO="HomericIntelligence/ProjectAgamemnon"
WORKFLOW="python-client-release.yml"
ENV_NAME="pypi"
TAG_PATTERN="v*"
PYPI_PACKAGE="HomericIntelligence-Agamemnon"

ok()   { printf '\033[32m[OK]\033[0m %s\n' "$1"; }
fail() { printf '\033[31m[FAIL]\033[0m %s\n' "$1"; FAILED=1; }
warn() { printf '\033[33m[WARN]\033[0m %s\n' "$1"; }

FAILED=0

# 1. Workflow file exists on main
if git show "origin/main:.github/workflows/${WORKFLOW}" &>/dev/null; then
    ok "Workflow '${WORKFLOW}' is present on main"
else
    fail "Workflow '${WORKFLOW}' is NOT present on main — merge the Python client PR first"
fi

# 2. GitHub pypi environment exists with tag policy
ENV_JSON=$(gh api "repos/${REPO}/environments/${ENV_NAME}" 2>/dev/null || true)
if [[ -z "$ENV_JSON" ]]; then
    fail "GitHub environment '${ENV_NAME}' does not exist — create it under repo Settings → Environments"
else
    ok "GitHub environment '${ENV_NAME}' exists"
    POLICY_JSON=$(gh api "repos/${REPO}/environments/${ENV_NAME}/deployment-branch-policies" 2>/dev/null || true)
    TAG_POLICY=$(echo "$POLICY_JSON" | python3 -c "
import sys, json
d = json.load(sys.stdin)
tags = [p for p in d.get('branch_policies', []) if p.get('type') == 'tag']
print(tags[0]['name'] if tags else '')
" 2>/dev/null || true)
    if [[ "$TAG_POLICY" == "$TAG_PATTERN" ]]; then
        ok "  Tag deployment policy '${TAG_PATTERN}' is set"
    else
        fail "  No '${TAG_PATTERN}' tag policy on '${ENV_NAME}' environment — add it under repo Settings → Environments"
    fi
fi

# 3. No existing v0.1.0 tag
if git ls-remote --tags origin "refs/tags/v0.1.0" | grep -q v0.1.0; then
    warn "Tag v0.1.0 already exists on origin — was the release already pushed?"
else
    ok "Tag v0.1.0 does not yet exist (ready to publish)"
fi

# 4. PyPI package not yet published (pending publisher required for first push)
if pip index versions "${PYPI_PACKAGE}" 2>/dev/null | grep -q "0.1.0"; then
    warn "${PYPI_PACKAGE}==0.1.0 is already on PyPI — release may already be complete"
else
    ok "${PYPI_PACKAGE}==0.1.0 not yet on PyPI (publish pending)"
fi

# 5. Local main is up-to-date
BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [[ "$BRANCH" == "main" ]]; then
    LOCAL=$(git rev-parse HEAD)
    REMOTE=$(git rev-parse origin/main)
    if [[ "$LOCAL" == "$REMOTE" ]]; then
        ok "Local main is up-to-date with origin/main"
    else
        fail "Local main is behind origin/main — run 'git pull' before tagging"
    fi
else
    warn "Not on main (current branch: ${BRANCH}) — switch to main before tagging"
fi

if [[ "$FAILED" -ne 0 ]]; then
    echo ""
    echo "One or more checks failed. Resolve the issues above before pushing the tag."
    exit 1
fi

echo ""
echo "All automated checks passed."
echo ""
echo "MANUAL CHECK REQUIRED:"
echo "  Verify the PyPI pending publisher is registered at:"
echo "  https://pypi.org/manage/account/publishing/"
echo "  (cannot be verified programmatically)"
echo ""
echo "When ready, run:"
echo "  git tag -s v0.1.0 -m 'release: v0.1.0'"
echo "  git push origin v0.1.0"
