"""Tests for graceful shutdown — draining in-flight advance_dag calls."""

from __future__ import annotations

import asyncio
from typing import Any
from unittest.mock import patch

import pytest

from agamemnon.orchestration.nats_listener import NATSListener
from agamemnon.orchestration.task_claimer import TaskClaimer

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


async def _noop_get_tasks(team_id: str) -> list[dict[str, Any]]:
    return []


async def _noop_claim_task(team_id: str, task_id: str) -> bool:
    return False


def make_claimer() -> TaskClaimer:
    """Return a TaskClaimer with no-op callbacks for unit tests."""
    return TaskClaimer(get_tasks=_noop_get_tasks, claim_task=_noop_claim_task)


class SlowTaskClaimer(TaskClaimer):
    """TaskClaimer whose advance_dag sleeps for a configurable duration."""

    def __init__(self, delay: float = 0.0) -> None:
        super().__init__(get_tasks=_noop_get_tasks, claim_task=_noop_claim_task)
        self.delay = delay
        self.calls: list[str] = []

    async def advance_dag(self, team_id: str) -> None:
        self.calls.append(team_id)
        if self.delay > 0:
            await asyncio.sleep(self.delay)


# ---------------------------------------------------------------------------
# TaskClaimer.drain — no in-flight tasks
# ---------------------------------------------------------------------------


class TestDrainEmpty:
    async def test_drain_returns_true_immediately_when_no_inflight(self) -> None:
        claimer = make_claimer()
        result = await claimer.drain(timeout=5.0)
        assert result is True

    async def test_inflight_count_zero_initially(self) -> None:
        claimer = make_claimer()
        assert claimer.in_flight_count == 0


# ---------------------------------------------------------------------------
# TaskClaimer — tracking in-flight tasks
# ---------------------------------------------------------------------------


class TestInflightTracking:
    async def test_inflight_count_increases_when_task_dispatched(self) -> None:
        claimer = SlowTaskClaimer(delay=0.1)
        task = claimer.advance_dag_tracked("team-1")
        assert claimer.in_flight_count == 1
        await task

    async def test_inflight_count_decreases_after_task_completes(self) -> None:
        claimer = SlowTaskClaimer(delay=0.0)
        task = claimer.advance_dag_tracked("team-1")
        await task
        # Give the done callback a chance to fire.
        await asyncio.sleep(0)
        assert claimer.in_flight_count == 0

    async def test_multiple_inflight_tracked(self) -> None:
        claimer = SlowTaskClaimer(delay=0.1)
        t1 = claimer.advance_dag_tracked("team-1")
        t2 = claimer.advance_dag_tracked("team-2")
        assert claimer.in_flight_count == 2
        await asyncio.gather(t1, t2)

    async def test_inflight_removed_after_exception(self) -> None:
        claimer = make_claimer()

        async def failing_advance_dag(team_id: str) -> None:
            raise RuntimeError("simulated failure")

        claimer.advance_dag = failing_advance_dag  # type: ignore[method-assign]
        task = claimer.advance_dag_tracked("team-x")
        with pytest.raises(RuntimeError):
            await task
        await asyncio.sleep(0)
        assert claimer.in_flight_count == 0


# ---------------------------------------------------------------------------
# TaskClaimer.drain — in-flight tasks complete before timeout
# ---------------------------------------------------------------------------


class TestDrainCompletes:
    async def test_inflight_advance_dag_completes_before_drain_returns(self) -> None:
        """Core success criterion: drain() waits for in-flight work to finish."""
        claimer = SlowTaskClaimer(delay=0.05)
        claimer.advance_dag_tracked("team-1")
        assert claimer.in_flight_count == 1

        result = await claimer.drain(timeout=5.0)

        assert result is True
        assert claimer.in_flight_count == 0

    async def test_multiple_inflight_all_complete_before_drain_returns(self) -> None:
        claimer = SlowTaskClaimer(delay=0.05)
        for team_id in ("team-1", "team-2", "team-3"):
            claimer.advance_dag_tracked(team_id)
        assert claimer.in_flight_count == 3

        result = await claimer.drain(timeout=5.0)

        assert result is True
        assert claimer.in_flight_count == 0

    async def test_advance_dag_called_with_correct_team_id(self) -> None:
        claimer = SlowTaskClaimer(delay=0.0)
        claimer.advance_dag_tracked("team-42")
        await claimer.drain(timeout=5.0)
        assert "team-42" in claimer.calls


# ---------------------------------------------------------------------------
# TaskClaimer.drain — timeout
# ---------------------------------------------------------------------------


class TestDrainTimeout:
    @pytest.mark.timeout(30)
    async def test_drain_timeout_returns_false(self) -> None:
        """drain() returns False when in-flight tasks outlast the timeout."""
        claimer = SlowTaskClaimer(delay=10.0)
        task = claimer.advance_dag_tracked("team-slow")

        result = await claimer.drain(timeout=0.05)

        assert result is False
        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass

    @pytest.mark.timeout(30)
    async def test_drain_timeout_logs_warning(self) -> None:
        claimer = SlowTaskClaimer(delay=10.0)
        task = claimer.advance_dag_tracked("team-slow")

        with patch.object(claimer, "_in_flight", claimer._in_flight):
            with patch("agamemnon.orchestration.task_claimer.logger") as mock_logger:
                result = await claimer.drain(timeout=0.05)
                assert result is False
                mock_logger.warning.assert_called_once()
                call_kwargs = mock_logger.warning.call_args
                assert call_kwargs[0][0] == "drain_timeout"

        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass


# ---------------------------------------------------------------------------
# NATSListener — shutting_down flag
# ---------------------------------------------------------------------------


class TestNATSListenerShutdown:
    def test_shutting_down_false_initially(self) -> None:
        claimer = make_claimer()
        listener = NATSListener(claimer)
        assert listener.shutting_down is False

    def test_begin_shutdown_sets_flag(self) -> None:
        claimer = make_claimer()
        listener = NATSListener(claimer)
        listener.begin_shutdown()
        assert listener.shutting_down is True

    async def test_new_events_dropped_after_begin_shutdown(self) -> None:
        """After begin_shutdown(), _on_task_event must not call advance_dag."""
        claimer = SlowTaskClaimer(delay=0.0)
        listener = NATSListener(claimer)
        listener.begin_shutdown()

        await listener._on_task_event("hi.tasks.team-1.t1.assigned", "team-1", "t1")

        assert claimer.in_flight_count == 0
        assert claimer.calls == []

    async def test_events_dispatched_before_shutdown(self) -> None:
        claimer = SlowTaskClaimer(delay=0.0)
        listener = NATSListener(claimer)

        await listener._on_task_event("hi.tasks.team-1.t1.assigned", "team-1", "t1")
        await asyncio.sleep(0.01)

        assert "team-1" in claimer.calls

    async def test_event_dropped_during_shutdown_logs_info(self) -> None:
        claimer = make_claimer()
        listener = NATSListener(claimer)
        listener.begin_shutdown()

        with patch("agamemnon.orchestration.nats_listener.logger") as mock_logger:
            await listener._on_task_event(
                "hi.tasks.team-dropped.t1.assigned", "team-dropped", "t1"
            )
            mock_logger.info.assert_called_once()
            call_args = mock_logger.info.call_args
            assert call_args[0][0] == "nats_event_dropped_during_shutdown"

    async def test_inflight_before_shutdown_completes(self) -> None:
        """Events in-flight at shutdown time must still complete."""
        claimer = SlowTaskClaimer(delay=0.05)
        listener = NATSListener(claimer)

        # Dispatch an event before shutdown.
        await listener._on_task_event("hi.tasks.team-1.t1.assigned", "team-1", "t1")
        assert claimer.in_flight_count == 1

        # Now signal shutdown.
        listener.begin_shutdown()

        # drain() must wait for the already-in-flight task.
        result = await claimer.drain(timeout=5.0)
        assert result is True
        assert claimer.in_flight_count == 0


# ---------------------------------------------------------------------------
# Integration: begin_shutdown → drain → stop sequence
# ---------------------------------------------------------------------------


class TestShutdownSequence:
    async def test_full_shutdown_sequence_completes_inflight(self) -> None:
        """Full sequence: begin_shutdown → drain → stop, all in-flight complete."""
        claimer = SlowTaskClaimer(delay=0.05)
        listener = NATSListener(claimer)

        # Simulate events arriving before shutdown.
        await listener._on_task_event("hi.tasks.team-a.t1.assigned", "team-a", "t1")
        await listener._on_task_event("hi.tasks.team-b.t1.assigned", "team-b", "t1")

        # Shutdown starts.
        listener.begin_shutdown()

        # New events are dropped.
        await listener._on_task_event("hi.tasks.team-c.t1.assigned", "team-c", "t1")

        # Drain in-flight ops.
        drained = await claimer.drain(timeout=5.0)
        assert drained is True

        # NATS drain.
        await listener.stop()

        assert claimer.in_flight_count == 0
        assert "team-c" not in claimer.calls

    @pytest.mark.timeout(30)
    async def test_stop_called_even_when_drain_times_out(self) -> None:
        """listener.stop() must be called even if drain times out."""
        claimer = SlowTaskClaimer(delay=10.0)
        listener = NATSListener(claimer)
        stop_called = False

        async def _stop() -> None:
            nonlocal stop_called
            stop_called = True

        listener.stop = _stop  # type: ignore[method-assign]

        task = claimer.advance_dag_tracked("team-slow")
        listener.begin_shutdown()

        drained = await claimer.drain(timeout=0.05)
        assert drained is False

        await listener.stop()
        assert stop_called is True

        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass
