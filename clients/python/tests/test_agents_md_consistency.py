"""Drift-detection tests ensuring AGENTS.md stays consistent with implementation."""

from __future__ import annotations

from pathlib import Path

import pytest

AGENTS_MD = Path(__file__).parent.parent.parent.parent / "AGENTS.md"

FORBIDDEN_PHRASES = [
    "BlazingMQ",
    "Keystone daemon",
    "Tailscale peer discovery",
    "SQLite",
]

REQUIRED_PHRASES = [
    "MaxAckPending=1",
    "hi.tasks.",
    "hi.myrmidon.",
    "GitHub Issues",
    "L0",
    "L1",
    "L2",
    "L3",
]


@pytest.fixture(scope="module")
def agents_md_content() -> str:
    """Read AGENTS.md once for all tests in this module."""
    assert AGENTS_MD.exists(), f"AGENTS.md not found at {AGENTS_MD}"
    return AGENTS_MD.read_text()


@pytest.mark.parametrize("phrase", FORBIDDEN_PHRASES)
def test_forbidden_phrase_absent(agents_md_content: str, phrase: str) -> None:
    """Aspirational or incorrect phrases must not appear in AGENTS.md."""
    assert phrase not in agents_md_content, (
        f"Forbidden phrase {phrase!r} found in AGENTS.md — remove aspirational claims"
    )


@pytest.mark.parametrize("phrase", REQUIRED_PHRASES)
def test_required_phrase_present(agents_md_content: str, phrase: str) -> None:
    """Core coordination concepts must be documented in AGENTS.md."""
    assert phrase in agents_md_content, (
        f"Required phrase {phrase!r} missing from AGENTS.md — documentation is incomplete"
    )
