"""Tests for AGAMEMNON_LOG_LEVEL validation in Settings.

See issue #132 — misconfigured log levels should fail loudly at startup
rather than silently falling back to WARNING via logging.basicConfig.
"""

from __future__ import annotations

import pytest

from agamemnon.orchestration.config import (
    VALID_LOG_LEVELS,
    Settings,
    load_settings,
)


class TestLogLevelValidation:
    """Validate that Settings rejects bad log levels and normalizes good ones."""

    @pytest.mark.parametrize("level", list(VALID_LOG_LEVELS))
    def test_accepts_canonical_levels(
        self, monkeypatch: pytest.MonkeyPatch, level: str
    ) -> None:
        """All documented log levels are accepted in canonical form."""
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", level)
        s = load_settings()
        assert s.log_level == level

    @pytest.mark.parametrize(
        ("supplied", "expected"),
        [
            ("debug", "DEBUG"),
            ("Info", "INFO"),
            ("  warning  ", "WARNING"),
            ("ERROR", "ERROR"),
            ("critical", "CRITICAL"),
        ],
    )
    def test_accepts_case_insensitively_and_normalizes(
        self,
        monkeypatch: pytest.MonkeyPatch,
        supplied: str,
        expected: str,
    ) -> None:
        """Mixed-case and whitespace input is normalized to canonical uppercase."""
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", supplied)
        s = load_settings()
        assert s.log_level == expected

    def test_default_is_info_when_env_unset(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Without AGAMEMNON_LOG_LEVEL set, default is INFO."""
        monkeypatch.delenv("AGAMEMNON_LOG_LEVEL", raising=False)
        s = load_settings()
        assert s.log_level == "INFO"

    @pytest.mark.parametrize("bad_value", ["TYPO", "verbose", "", "NOTSET", "warn"])
    def test_rejects_invalid_values(
        self, monkeypatch: pytest.MonkeyPatch, bad_value: str
    ) -> None:
        """Unknown log levels raise ValueError listing accepted values."""
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", bad_value)
        with pytest.raises(ValueError) as excinfo:
            load_settings()
        msg = str(excinfo.value)
        for valid in VALID_LOG_LEVELS:
            assert valid in msg

    def test_setattr_validates_assignment(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Assigning an invalid log_level after construction also raises."""
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", "INFO")
        s = load_settings()
        with pytest.raises(ValueError, match="Invalid log level"):
            s.log_level = "TYPO"

    def test_setattr_normalizes_assignment(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Assigning a lowercase log_level normalizes it."""
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", "INFO")
        s = Settings()
        s.log_level = "debug"
        assert s.log_level == "DEBUG"
