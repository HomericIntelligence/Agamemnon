"""Unit tests for TaskEvent Pydantic model validation and status normalization."""

from __future__ import annotations

import pytest
from pydantic import ValidationError

from agamemnon.orchestration.models import TaskEvent, resolve_event_status


class TestTaskEventFlatStatus:
    """TaskEvent resolves effective_status from top-level status field."""

    def test_flat_status_resolved(self) -> None:
        event = TaskEvent.model_validate({"status": "completed"})
        assert event.effective_status == "completed"

    def test_flat_status_preserves_value(self) -> None:
        event = TaskEvent.model_validate({"status": "failed"})
        assert event.status == "failed"
        assert event.effective_status == "failed"

    @pytest.mark.parametrize("status", ["completed", "failed", "error", "cancelled", "running"])
    def test_various_flat_statuses(self, status: str) -> None:
        event = TaskEvent.model_validate({"status": status})
        assert event.effective_status == status


class TestTaskEventNestedStatus:
    """TaskEvent resolves effective_status from data.status (Hermes format, issue #107)."""

    def test_nested_data_status_resolved(self) -> None:
        event = TaskEvent.model_validate({"data": {"status": "completed"}})
        assert event.effective_status == "completed"

    def test_nested_status_when_top_level_absent(self) -> None:
        event = TaskEvent.model_validate({"data": {"status": "running", "extra": "value"}})
        assert event.effective_status == "running"

    def test_top_level_status_takes_precedence_over_nested(self) -> None:
        event = TaskEvent.model_validate({"status": "completed", "data": {"status": "running"}})
        assert event.effective_status == "completed"

    def test_data_without_status_key(self) -> None:
        event = TaskEvent.model_validate({"data": {"other": "value"}})
        assert event.effective_status is None

    def test_data_none_skipped(self) -> None:
        event = TaskEvent.model_validate({"data": None})
        assert event.effective_status is None


class TestTaskEventNewStatus:
    """TaskEvent resolves effective_status from newStatus field as fallback."""

    def test_new_status_resolved_when_status_absent(self) -> None:
        event = TaskEvent.model_validate({"newStatus": "completed"})
        assert event.effective_status == "completed"

    def test_status_takes_precedence_over_new_status(self) -> None:
        event = TaskEvent.model_validate({"status": "failed", "newStatus": "completed"})
        assert event.effective_status == "failed"

    def test_nested_status_takes_precedence_over_new_status(self) -> None:
        event = TaskEvent.model_validate({"data": {"status": "running"}, "newStatus": "completed"})
        assert event.effective_status == "running"

    def test_new_status_fallback_when_flat_and_nested_absent(self) -> None:
        event = TaskEvent.model_validate({"newStatus": "cancelled", "taskId": "t-1"})
        assert event.effective_status == "cancelled"


class TestTaskEventMissingFields:
    """TaskEvent handles sparse payloads gracefully — all fields optional."""

    def test_empty_payload_is_valid(self) -> None:
        event = TaskEvent.model_validate({})
        assert event.effective_status is None
        assert event.status is None
        assert event.newStatus is None
        assert event.data is None
        assert event.taskId is None
        assert event.teamId is None

    def test_unknown_fields_ignored(self) -> None:
        with pytest.raises(ValidationError, match="Extra inputs are not permitted"):
            TaskEvent.model_validate({"status": "done", "unknownField": "ignored"})

    def test_task_id_and_team_id_populated(self) -> None:
        event = TaskEvent.model_validate({"taskId": "t-42", "teamId": "team-1", "status": "completed"})
        assert event.taskId == "t-42"
        assert event.teamId == "team-1"
        assert event.effective_status == "completed"


class TestTaskEventResolutionPriority:
    """Verify the three-way resolution priority: status > data.status > newStatus."""

    def test_all_three_present_top_level_wins(self) -> None:
        event = TaskEvent.model_validate({
            "status": "completed",
            "data": {"status": "running"},
            "newStatus": "failed",
        })
        assert event.effective_status == "completed"

    def test_nested_beats_new_status(self) -> None:
        event = TaskEvent.model_validate({
            "data": {"status": "running"},
            "newStatus": "failed",
        })
        assert event.effective_status == "running"

    def test_empty_status_string_falls_through_to_nested(self) -> None:
        event = TaskEvent.model_validate({"status": "", "data": {"status": "running"}})
        assert event.effective_status == "running"

    def test_empty_status_string_falls_through_to_new_status(self) -> None:
        event = TaskEvent.model_validate({"status": "", "newStatus": "completed"})
        assert event.effective_status == "completed"

    def test_none_status_falls_through(self) -> None:
        event = TaskEvent.model_validate({"status": None, "newStatus": "completed"})
        assert event.effective_status == "completed"


class TestResolveEventStatus:
    """Unit tests for the standalone resolve_event_status utility function."""

    def test_status_returned_when_set(self) -> None:
        assert resolve_event_status("done", None, None) == "done"

    def test_data_status_used_when_status_absent(self) -> None:
        assert resolve_event_status(None, {"status": "done"}, None) == "done"

    def test_new_status_used_as_final_fallback(self) -> None:
        assert resolve_event_status(None, None, "done") == "done"

    def test_status_takes_priority_over_data_and_new_status(self) -> None:
        assert resolve_event_status("top", {"status": "nested"}, "new") == "top"

    def test_all_none_returns_none(self) -> None:
        assert resolve_event_status(None, None, None) is None
