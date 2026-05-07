"""Tests for agamemnon_client.config — Settings and load_settings."""

from __future__ import annotations

import warnings

import pytest

from agamemnon_client.config import Settings, load_settings


class TestLoadSettingsDefaults:
    def test_defaults(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("AGAMEMNON_LOG_LEVEL", raising=False)
        monkeypatch.delenv("AGAMEMNON_POLL_INTERVAL", raising=False)
        monkeypatch.delenv("AGAMEMNON_SHUTDOWN_TIMEOUT", raising=False)
        monkeypatch.delenv("KEYSTONE_LOG_LEVEL", raising=False)
        monkeypatch.delenv("KEYSTONE_POLL_INTERVAL", raising=False)
        monkeypatch.delenv("KEYSTONE_SHUTDOWN_TIMEOUT", raising=False)

        s = load_settings()

        assert s.log_level == "INFO"
        assert s.poll_interval == 1.0
        assert s.shutdown_timeout == 30.0

    def test_return_type(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("AGAMEMNON_LOG_LEVEL", raising=False)
        monkeypatch.delenv("AGAMEMNON_POLL_INTERVAL", raising=False)
        monkeypatch.delenv("AGAMEMNON_SHUTDOWN_TIMEOUT", raising=False)
        monkeypatch.delenv("KEYSTONE_LOG_LEVEL", raising=False)
        monkeypatch.delenv("KEYSTONE_POLL_INTERVAL", raising=False)
        monkeypatch.delenv("KEYSTONE_SHUTDOWN_TIMEOUT", raising=False)

        s = load_settings()

        assert isinstance(s, Settings)
        assert isinstance(s.poll_interval, float)
        assert isinstance(s.shutdown_timeout, float)


class TestLoadSettingsNewNames:
    def test_agamemnon_log_level(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", "DEBUG")
        monkeypatch.delenv("KEYSTONE_LOG_LEVEL", raising=False)

        s = load_settings()

        assert s.log_level == "DEBUG"

    def test_agamemnon_poll_interval(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("AGAMEMNON_POLL_INTERVAL", "2.5")
        monkeypatch.delenv("KEYSTONE_POLL_INTERVAL", raising=False)

        s = load_settings()

        assert s.poll_interval == 2.5

    def test_agamemnon_shutdown_timeout(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("AGAMEMNON_SHUTDOWN_TIMEOUT", "60.0")
        monkeypatch.delenv("KEYSTONE_SHUTDOWN_TIMEOUT", raising=False)

        s = load_settings()

        assert s.shutdown_timeout == 60.0

    def test_all_agamemnon_vars_no_warning(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", "WARNING")
        monkeypatch.setenv("AGAMEMNON_POLL_INTERVAL", "0.5")
        monkeypatch.setenv("AGAMEMNON_SHUTDOWN_TIMEOUT", "10.0")
        monkeypatch.delenv("KEYSTONE_LOG_LEVEL", raising=False)
        monkeypatch.delenv("KEYSTONE_POLL_INTERVAL", raising=False)
        monkeypatch.delenv("KEYSTONE_SHUTDOWN_TIMEOUT", raising=False)

        with warnings.catch_warnings():
            warnings.simplefilter("error", DeprecationWarning)
            s = load_settings()

        assert s.log_level == "WARNING"
        assert s.poll_interval == 0.5
        assert s.shutdown_timeout == 10.0


class TestLoadSettingsOldNames:
    def test_keystone_log_level_warns(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("AGAMEMNON_LOG_LEVEL", raising=False)
        monkeypatch.setenv("KEYSTONE_LOG_LEVEL", "ERROR")

        with pytest.warns(DeprecationWarning, match="KEYSTONE_LOG_LEVEL is deprecated"):
            s = load_settings()

        assert s.log_level == "ERROR"

    def test_keystone_poll_interval_warns(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("AGAMEMNON_POLL_INTERVAL", raising=False)
        monkeypatch.setenv("KEYSTONE_POLL_INTERVAL", "5.0")

        with pytest.warns(DeprecationWarning, match="KEYSTONE_POLL_INTERVAL is deprecated"):
            s = load_settings()

        assert s.poll_interval == 5.0

    def test_keystone_shutdown_timeout_warns(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.delenv("AGAMEMNON_SHUTDOWN_TIMEOUT", raising=False)
        monkeypatch.setenv("KEYSTONE_SHUTDOWN_TIMEOUT", "45.0")

        with pytest.warns(DeprecationWarning, match="KEYSTONE_SHUTDOWN_TIMEOUT is deprecated"):
            s = load_settings()

        assert s.shutdown_timeout == 45.0

    def test_deprecation_warning_message_includes_new_name(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.delenv("AGAMEMNON_LOG_LEVEL", raising=False)
        monkeypatch.setenv("KEYSTONE_LOG_LEVEL", "DEBUG")

        with pytest.warns(DeprecationWarning, match="AGAMEMNON_LOG_LEVEL"):
            load_settings()


class TestLoadSettingsPrecedence:
    def test_agamemnon_takes_precedence_over_keystone(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setenv("AGAMEMNON_LOG_LEVEL", "CRITICAL")
        monkeypatch.setenv("KEYSTONE_LOG_LEVEL", "DEBUG")

        with warnings.catch_warnings():
            warnings.simplefilter("error", DeprecationWarning)
            s = load_settings()

        assert s.log_level == "CRITICAL"

    def test_agamemnon_poll_interval_precedence(self, monkeypatch: pytest.MonkeyPatch) -> None:
        monkeypatch.setenv("AGAMEMNON_POLL_INTERVAL", "3.0")
        monkeypatch.setenv("KEYSTONE_POLL_INTERVAL", "99.0")

        with warnings.catch_warnings():
            warnings.simplefilter("error", DeprecationWarning)
            s = load_settings()

        assert s.poll_interval == 3.0

    def test_agamemnon_shutdown_timeout_precedence(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        monkeypatch.setenv("AGAMEMNON_SHUTDOWN_TIMEOUT", "15.0")
        monkeypatch.setenv("KEYSTONE_SHUTDOWN_TIMEOUT", "99.0")

        with warnings.catch_warnings():
            warnings.simplefilter("error", DeprecationWarning)
            s = load_settings()

        assert s.shutdown_timeout == 15.0
