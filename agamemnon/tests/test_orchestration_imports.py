"""Smoke tests verifying the orchestration package is importable under its new namespace."""

from __future__ import annotations

import pytest


def test_package_import() -> None:
    """Orchestration package is importable."""
    import agamemnon.orchestration  # noqa: F401


def test_config_module() -> None:
    """Config module exports Settings and load_settings."""
    from agamemnon.orchestration.config import Settings, load_settings

    settings = load_settings()
    assert isinstance(settings, Settings)
    assert settings.log_level in ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")
    assert settings.poll_interval > 0
    assert settings.shutdown_timeout > 0


def test_models_module() -> None:
    """Models module exports Task, Agent, TaskEvent, and resolve_event_status."""
    from agamemnon.orchestration.models import (
        Agent,
        Task,
        TaskEvent,
        resolve_event_status,
    )

    task = Task(id="t1", title="Do work", status="pending")
    assert task.id == "t1"
    assert task.dependencies == []

    agent = Agent(id="a1", name="Worker")
    assert agent.status == "active"
    assert agent.current_task_id is None

    event = TaskEvent(taskId="t1", teamId="team1", status="completed")
    assert event.effective_status == "completed"

    assert resolve_event_status("active", None, None) == "active"
    assert resolve_event_status(None, {"status": "pending"}, None) == "pending"
    assert resolve_event_status(None, None, "failed") == "failed"


def test_validation_module() -> None:
    """Validation module rejects unsafe ID strings."""
    from agamemnon.orchestration.validation import validate_id

    assert validate_id("valid-id-123", "task_id") == "valid-id-123"

    with pytest.raises(ValueError, match="empty"):
        validate_id("", "task_id")

    with pytest.raises(ValueError, match="path separator"):
        validate_id("a/b", "task_id")

    with pytest.raises(ValueError, match="path traversal"):
        validate_id("a..b", "task_id")

    with pytest.raises(ValueError, match="whitespace"):
        validate_id("  ", "task_id")


def test_logging_module() -> None:
    """Logging module exports get_logger and KeystoneLogger."""
    from agamemnon.orchestration.logging import KeystoneLogger, get_logger

    logger = get_logger(component="test")
    assert isinstance(logger, KeystoneLogger)


def test_task_event_rejects_unknown_fields() -> None:
    """TaskEvent with extra='forbid' raises on unknown fields."""
    from pydantic import ValidationError

    from agamemnon.orchestration.models import TaskEvent

    with pytest.raises(ValidationError):
        TaskEvent(unknownField="x")  # type: ignore[call-arg]


@pytest.mark.parametrize(
    ("status", "data", "new_status", "expected"),
    [
        ("active", None, None, "active"),
        (None, {"status": "pending"}, None, "pending"),
        (None, None, "failed", "failed"),
        ("active", {"status": "pending"}, "failed", "active"),
        (None, {"status": "pending"}, "failed", "pending"),
        (None, None, None, None),
    ],
)
def test_resolve_event_status_priority(
    status: str | None,
    data: dict | None,
    new_status: str | None,
    expected: str | None,
) -> None:
    """resolve_event_status applies priority: status > data.status > new_status."""
    from agamemnon.orchestration.models import resolve_event_status

    assert resolve_event_status(status, data, new_status) == expected
