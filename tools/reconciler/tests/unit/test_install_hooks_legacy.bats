#!/usr/bin/env bats
# tests/unit/test_install_hooks_legacy.bats
#
# Issue #421: test coverage for the install-hooks-legacy justfile recipe.
#
# Verifies that install-hooks-legacy copies hooks/pre-commit to .git/hooks/pre-commit
# without invoking `pixi run pre-commit install`.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")/../.." && pwd)"

TMP_REPO=""
PIXI_STUB=""

setup() {
    TMP_REPO="${SCRIPT_DIR}/_install_hooks_legacy_test_$$_${RANDOM}"
    mkdir -p "$TMP_REPO"

    git -C "$TMP_REPO" init -q
    git -C "$TMP_REPO" config user.email "test@example.com"
    git -C "$TMP_REPO" config user.name "Test"
    git -C "$TMP_REPO" config commit.gpgsign false

    mkdir -p "${TMP_REPO}/hooks"
    mkdir -p "${TMP_REPO}/.git/hooks"
    cp "${SCRIPT_DIR}/hooks/pre-commit" "${TMP_REPO}/hooks/pre-commit"

    touch "${TMP_REPO}/.gitkeep"
    git -C "$TMP_REPO" add .gitkeep
    git -C "$TMP_REPO" commit -q --no-verify -m "init"

    # Stub that records any invocation of pixi
    PIXI_STUB="$(mktemp -d)"
    cat > "${PIXI_STUB}/pixi" <<'SH'
#!/usr/bin/env bash
echo "called" >> "${0%/*}/pixi.log"
exit 0
SH
    chmod +x "${PIXI_STUB}/pixi"
}

teardown() {
    [[ -n "$TMP_REPO" && -d "$TMP_REPO" ]] && rm -rf "$TMP_REPO"
    [[ -n "$PIXI_STUB" && -d "$PIXI_STUB" ]] && rm -rf "$PIXI_STUB"
}

@test "install-hooks-legacy: copies pre-commit hook to .git/hooks/pre-commit" {
    run just --justfile "${SCRIPT_DIR}/justfile" \
        --working-directory "${TMP_REPO}" \
        install-hooks-legacy
    [ "$status" -eq 0 ]
    [ -f "${TMP_REPO}/.git/hooks/pre-commit" ]
}

@test "install-hooks-legacy: installed hook is executable" {
    run just --justfile "${SCRIPT_DIR}/justfile" \
        --working-directory "${TMP_REPO}" \
        install-hooks-legacy
    [ "$status" -eq 0 ]
    [ -x "${TMP_REPO}/.git/hooks/pre-commit" ]
}

@test "install-hooks-legacy: installed hook content matches hooks/pre-commit" {
    run just --justfile "${SCRIPT_DIR}/justfile" \
        --working-directory "${TMP_REPO}" \
        install-hooks-legacy
    [ "$status" -eq 0 ]
    run diff "${TMP_REPO}/hooks/pre-commit" "${TMP_REPO}/.git/hooks/pre-commit"
    [ "$status" -eq 0 ]
}

@test "install-hooks-legacy: does not invoke pixi run pre-commit install" {
    run env PATH="${PIXI_STUB}:${PATH}" \
        just --justfile "${SCRIPT_DIR}/justfile" \
        --working-directory "${TMP_REPO}" \
        install-hooks-legacy
    [ "$status" -eq 0 ]
    [ ! -f "${PIXI_STUB}/pixi.log" ]
}
