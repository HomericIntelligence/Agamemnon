"""Tests for NATSListener: ID validation in _on_task_event and start() error handling."""
from __future__ import annotations

import json

import pytest
from unittest.mock import AsyncMock, MagicMock

import nats.errors
import nats.js.errors

from agamemnon.orchestration.nats_listener import NATSListener


def _make_listener() -> tuple[NATSListener, MagicMock]:
    claimer = MagicMock()
    claimer.advance_dag_tracked = MagicMock()
    listener = NATSListener(claimer)
    return listener, claimer


class TestOnTaskEventValidIds:
    async def test_valid_ids_dispatches(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event("hi.tasks.team-1.task-42.completed", "team-1", "task-42")
        claimer.advance_dag_tracked.assert_called_once_with("team-1")

    async def test_valid_uuid_ids_dispatches(self) -> None:
        listener, claimer = _make_listener()
        team = "550e8400-e29b-41d4-a716-446655440000"
        task = "6ba7b810-9dad-11d1-80b4-00c04fd430c8"
        await listener._on_task_event(f"hi.tasks.{team}.{task}.completed", team, task)
        claimer.advance_dag_tracked.assert_called_once_with(team)


class TestOnTaskEventInvalidIds:
    async def test_empty_team_id_dropped(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event("hi.tasks...task-1.completed", "", "task-1")
        claimer.advance_dag_tracked.assert_not_called()

    async def test_slash_in_team_id_dropped(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event("hi.tasks.../admin.task-1.completed", "../admin", "task-1")
        claimer.advance_dag_tracked.assert_not_called()

    async def test_empty_task_id_dropped(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event("hi.tasks.team-1..completed", "team-1", "")
        claimer.advance_dag_tracked.assert_not_called()

    async def test_path_traversal_task_id_dropped(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event("hi.tasks.team-1.../etc.completed", "team-1", "../etc")
        claimer.advance_dag_tracked.assert_not_called()

    async def test_overlong_team_id_dropped(self) -> None:
        listener, claimer = _make_listener()
        bad_team = "a" * 257
        await listener._on_task_event("hi.tasks.AAA.task-1.completed", bad_team, "task-1")
        claimer.advance_dag_tracked.assert_not_called()


class TestParseSubject:
    def test_valid_subject_extracts_team_and_task(self) -> None:
        result = NATSListener._parse_subject("hi.tasks.team-1.task-2.completed")
        assert result == ("team-1", "task-2")

    def test_short_subject_raises_value_error(self) -> None:
        with pytest.raises(ValueError):
            NATSListener._parse_subject("hi.tasks.team-1.task-2")

    def test_long_subject_raises_value_error(self) -> None:
        with pytest.raises(ValueError):
            NATSListener._parse_subject("hi.tasks.team-1.task-2.completed.extra")

    def test_empty_subject_raises(self) -> None:
        with pytest.raises(ValueError):
            NATSListener._parse_subject("")


class TestOnTaskEventShutdown:
    async def test_event_dropped_during_shutdown(self) -> None:
        listener, claimer = _make_listener()
        listener.begin_shutdown()
        await listener._on_task_event("hi.tasks.team-1.task-1.completed", "team-1", "task-1")
        claimer.advance_dag_tracked.assert_not_called()

    async def test_shutdown_checked_before_validation(self) -> None:
        listener, claimer = _make_listener()
        listener.begin_shutdown()
        # Even with invalid IDs, no error raised — shutdown takes priority
        await listener._on_task_event("bad", "", "")
        claimer.advance_dag_tracked.assert_not_called()


class TestOnTaskEventRawPayload:
    @pytest.mark.asyncio
    async def test_valid_json_payload_dispatches(self) -> None:
        listener, claimer = _make_listener()
        payload = json.dumps({"status": "completed"}).encode()
        await listener._on_task_event(
            "hi.tasks.team-1.task-42.completed", "team-1", "task-42", raw_payload=payload
        )
        claimer.advance_dag_tracked.assert_called_once_with("team-1")

    @pytest.mark.asyncio
    async def test_invalid_json_payload_drops_event(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event(
            "hi.tasks.team-1.task-42.completed",
            "team-1",
            "task-42",
            raw_payload=b"not-valid-json",
        )
        claimer.advance_dag_tracked.assert_not_called()

    @pytest.mark.asyncio
    async def test_payload_missing_status_still_dispatches(self) -> None:
        listener, claimer = _make_listener()
        payload = json.dumps({}).encode()
        await listener._on_task_event(
            "hi.tasks.team-1.task-42.completed", "team-1", "task-42", raw_payload=payload
        )
        claimer.advance_dag_tracked.assert_called_once_with("team-1")

    @pytest.mark.asyncio
    async def test_nested_data_status_resolves_effective_status(self) -> None:
        listener, claimer = _make_listener()
        payload = json.dumps({"data": {"status": "completed"}}).encode()
        await listener._on_task_event(
            "hi.tasks.team-1.task-42.completed", "team-1", "task-42", raw_payload=payload
        )
        claimer.advance_dag_tracked.assert_called_once_with("team-1")

    @pytest.mark.asyncio
    async def test_none_payload_dispatches_without_parsing(self) -> None:
        listener, claimer = _make_listener()
        await listener._on_task_event(
            "hi.tasks.team-1.task-42.completed", "team-1", "task-42", raw_payload=None
        )
        claimer.advance_dag_tracked.assert_called_once_with("team-1")


# ---------------------------------------------------------------------------
# Helpers for start() tests
# ---------------------------------------------------------------------------

def _make_mock_nc(subscribe_side_effect: object = None) -> MagicMock:
    """Return a mock NATS client whose jetstream().subscribe() behaves as specified."""
    nc = MagicMock()
    js = MagicMock()
    nc.jetstream.return_value = js
    if subscribe_side_effect is None:
        js.subscribe = AsyncMock(return_value=MagicMock())
    else:
        js.subscribe = AsyncMock(side_effect=subscribe_side_effect)
    return nc


class TestStartSubscribeSuccess:
    async def test_subscribe_success_returns(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc()
        await listener.start(nc, "hi.tasks.>", stream="homeric-tasks")
        nc.jetstream().subscribe.assert_awaited_once()

    async def test_subscribe_success_uses_subject(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc()
        await listener.start(nc, "hi.tasks.team-1.>", stream="homeric-tasks")
        nc.jetstream().subscribe.assert_awaited_once_with("hi.tasks.team-1.>")


class TestStartStreamNotFound:
    async def test_stream_not_found_raises_runtime_error(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc(subscribe_side_effect=nats.js.errors.NotFoundError())
        with pytest.raises(RuntimeError, match="homeric-tasks"):
            await listener.start(
                nc, "hi.tasks.>", stream="homeric-tasks", max_retries=1, retry_base_delay=0.0
            )

    async def test_stream_not_found_retries_up_to_max(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc(subscribe_side_effect=nats.js.errors.NotFoundError())
        with pytest.raises(RuntimeError):
            await listener.start(nc, "hi.tasks.>", max_retries=3, retry_base_delay=0.0)
        assert nc.jetstream().subscribe.await_count == 3

    async def test_stream_not_found_succeeds_on_retry(self) -> None:
        listener, _ = _make_listener()
        nc = MagicMock()
        js = MagicMock()
        nc.jetstream.return_value = js
        js.subscribe = AsyncMock(
            side_effect=[
                nats.js.errors.NotFoundError(),
                nats.js.errors.NotFoundError(),
                MagicMock(),
            ]
        )
        await listener.start(nc, "hi.tasks.>", max_retries=3, retry_base_delay=0.0)
        assert js.subscribe.await_count == 3

    async def test_error_message_includes_stream_name(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc(subscribe_side_effect=nats.js.errors.NotFoundError())
        with pytest.raises(RuntimeError, match="my-custom-stream"):
            await listener.start(
                nc, "hi.tasks.>", stream="my-custom-stream", max_retries=1, retry_base_delay=0.0
            )


class TestStartConnectionErrors:
    async def test_no_servers_raises_connection_error(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc(subscribe_side_effect=nats.errors.NoServersError())
        with pytest.raises(ConnectionError, match="unreachable"):
            await listener.start(nc, "hi.tasks.>")

    async def test_auth_error_raises_connection_error(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc(subscribe_side_effect=nats.errors.AuthorizationError())
        with pytest.raises(ConnectionError, match="authentication"):
            await listener.start(nc, "hi.tasks.>")

    async def test_jetstream_unavailable_raises_runtime_error(self) -> None:
        listener, _ = _make_listener()
        nc = _make_mock_nc(
            subscribe_side_effect=nats.js.errors.ServiceUnavailableError()
        )
        with pytest.raises(RuntimeError, match="JetStream"):
            await listener.start(nc, "hi.tasks.>")
