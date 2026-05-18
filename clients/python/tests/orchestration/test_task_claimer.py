"""Tests for TaskClaimer concurrency guard."""

from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock

import pytest
from agamemnon.orchestration.task_claimer import TaskClaimer


def _make_claimer(
    tasks_by_team: dict[str, list[dict]] | None = None,
    claim_result: bool = True,
) -> tuple[TaskClaimer, AsyncMock, AsyncMock]:
    tasks_by_team = tasks_by_team or {}

    async def get_tasks(team_id: str) -> list[dict]:
        return tasks_by_team.get(team_id, [])

    get_tasks_mock = AsyncMock(side_effect=get_tasks)
    claim_mock = AsyncMock(return_value=claim_result)
    claimer = TaskClaimer(get_tasks=get_tasks_mock, claim_task=claim_mock)
    return claimer, get_tasks_mock, claim_mock


async def test_advance_dag_claims_ready_tasks() -> None:
    tasks = [{"id": "t1"}, {"id": "t2"}]
    claimer, get_tasks_mock, claim_mock = _make_claimer({"team-A": tasks})

    result = await claimer.advance_dag("team-A")

    assert result == ["t1", "t2"]
    get_tasks_mock.assert_awaited_once_with("team-A")
    assert claim_mock.await_count == 2
    claim_mock.assert_any_await("team-A", "t1")
    claim_mock.assert_any_await("team-A", "t2")


async def test_advance_dag_empty_team_returns_empty() -> None:
    claimer, get_tasks_mock, claim_mock = _make_claimer()

    result = await claimer.advance_dag("team-empty")

    assert result == []
    get_tasks_mock.assert_awaited_once_with("team-empty")
    claim_mock.assert_not_awaited()


async def test_advance_dag_partial_claim_failures() -> None:
    tasks = [{"id": "t1"}, {"id": "t2"}]

    async def get_tasks(team_id: str) -> list[dict]:
        return tasks

    get_tasks_mock = AsyncMock(side_effect=get_tasks)
    # First claim succeeds, second fails
    claim_mock = AsyncMock(side_effect=[True, False])
    claimer = TaskClaimer(get_tasks=get_tasks_mock, claim_task=claim_mock)

    result = await claimer.advance_dag("team-A")

    assert result == ["t1"]


async def test_concurrent_advance_dag_coalesced() -> None:
    """Second advance_dag for same team is skipped while first is in-flight."""
    first_call_started = asyncio.Event()
    first_call_proceed = asyncio.Event()

    async def slow_get_tasks(team_id: str) -> list[dict]:
        first_call_started.set()
        await first_call_proceed.wait()
        return [{"id": "t1"}]

    get_tasks_mock = AsyncMock(side_effect=slow_get_tasks)
    claim_mock = AsyncMock(return_value=True)
    claimer = TaskClaimer(get_tasks=get_tasks_mock, claim_task=claim_mock)

    async def first_call() -> list[str]:
        return await claimer.advance_dag("team-X")

    async def second_call() -> list[str]:
        # Wait until first call is inside get_tasks (in-flight), then try
        await first_call_started.wait()
        result = await claimer.advance_dag("team-X")
        # Now ungate the first call so the gather can complete
        first_call_proceed.set()
        return result

    result1, result2 = await asyncio.gather(first_call(), second_call())

    # First call completes normally; second was coalesced
    assert result1 == ["t1"]
    assert result2 == []
    # get_tasks called only once - second call was skipped before calling it
    assert get_tasks_mock.await_count == 1


async def test_concurrent_advance_dag_different_teams_parallel() -> None:
    """Concurrent calls for different teams both proceed independently."""
    tasks = {"team-A": [{"id": "a1"}], "team-B": [{"id": "b1"}]}
    claimer, get_tasks_mock, claim_mock = _make_claimer(tasks)

    result_a, result_b = await asyncio.gather(
        claimer.advance_dag("team-A"),
        claimer.advance_dag("team-B"),
    )

    assert sorted(result_a) == ["a1"]
    assert sorted(result_b) == ["b1"]
    assert get_tasks_mock.await_count == 2


async def test_advance_dag_lock_released_on_error() -> None:
    """Lock and _advancing set are cleaned up even when get_tasks raises."""
    call_count = 0

    async def flaky_get_tasks(team_id: str) -> list[dict]:
        nonlocal call_count
        call_count += 1
        if call_count == 1:
            raise RuntimeError("transient failure")
        return [{"id": "t1"}]

    get_tasks_mock = AsyncMock(side_effect=flaky_get_tasks)
    claim_mock = AsyncMock(return_value=True)
    claimer = TaskClaimer(get_tasks=get_tasks_mock, claim_task=claim_mock)

    with pytest.raises(RuntimeError, match="transient failure"):
        await claimer.advance_dag("team-Z")

    # Second call must not deadlock and must succeed
    result = await claimer.advance_dag("team-Z")
    assert result == ["t1"]
    assert get_tasks_mock.await_count == 2


async def test_sequential_calls_same_team_both_execute() -> None:
    """Sequential (non-concurrent) calls for the same team both run normally."""
    tasks = [{"id": "t1"}]
    claimer, get_tasks_mock, claim_mock = _make_claimer({"team-A": tasks})

    result1 = await claimer.advance_dag("team-A")
    result2 = await claimer.advance_dag("team-A")

    assert result1 == ["t1"]
    assert result2 == ["t1"]
    assert get_tasks_mock.await_count == 2


# ---------------------------------------------------------------------------
# Issue #295 — coalesced count metrics
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_coalesced_count_increments_when_skipped() -> None:
    """get_coalesced_count increments each time an advance_dag call is skipped."""
    first_call_started = asyncio.Event()
    first_call_proceed = asyncio.Event()

    async def slow_get_tasks(team_id: str) -> list[dict]:
        first_call_started.set()
        await first_call_proceed.wait()
        return []

    get_tasks_mock = AsyncMock(side_effect=slow_get_tasks)
    claim_mock = AsyncMock(return_value=True)
    claimer = TaskClaimer(get_tasks=get_tasks_mock, claim_task=claim_mock)

    async def first_call() -> list[str]:
        return await claimer.advance_dag("team-X")

    async def second_call() -> list[str]:
        await first_call_started.wait()
        result = await claimer.advance_dag("team-X")
        first_call_proceed.set()
        return result

    await asyncio.gather(first_call(), second_call())

    assert claimer.get_coalesced_count("team-X") == 1


@pytest.mark.asyncio
async def test_coalesced_count_zero_when_not_skipped() -> None:
    """get_coalesced_count is 0 for a team that was never coalesced."""
    claimer, _, _ = _make_claimer()
    assert claimer.get_coalesced_count("team-never-seen") == 0


@pytest.mark.asyncio
async def test_get_metrics_returns_all_coalesced_teams() -> None:
    """get_metrics returns a dict with entries for every coalesced team."""
    first_started: dict[str, asyncio.Event] = {
        "team-A": asyncio.Event(),
        "team-B": asyncio.Event(),
    }
    proceed: dict[str, asyncio.Event] = {
        "team-A": asyncio.Event(),
        "team-B": asyncio.Event(),
    }

    async def slow_get_tasks(team_id: str) -> list[dict]:
        first_started[team_id].set()
        await proceed[team_id].wait()
        return []

    get_tasks_mock = AsyncMock(side_effect=slow_get_tasks)
    claim_mock = AsyncMock(return_value=True)
    claimer = TaskClaimer(get_tasks=get_tasks_mock, claim_task=claim_mock)

    async def do_coalesce(team_id: str) -> None:
        async def first() -> None:
            await claimer.advance_dag(team_id)

        async def second() -> None:
            await first_started[team_id].wait()
            await claimer.advance_dag(team_id)
            proceed[team_id].set()

        await asyncio.gather(first(), second())

    await asyncio.gather(do_coalesce("team-A"), do_coalesce("team-B"))

    metrics = claimer.get_metrics()
    assert metrics.get("team-A", 0) == 1
    assert metrics.get("team-B", 0) == 1


# ---------------------------------------------------------------------------
# Issue #296 — cleanup_idle_teams
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_cleanup_idle_teams_removes_idle_locks() -> None:
    """cleanup_idle_teams removes team locks that are not currently advancing."""
    claimer, _, _ = _make_claimer()

    # Trigger lazy lock creation for two teams by calling advance_dag
    await claimer.advance_dag("team-A")
    await claimer.advance_dag("team-B")

    assert "team-A" in claimer._team_locks
    assert "team-B" in claimer._team_locks

    removed = claimer.cleanup_idle_teams()

    assert "team-A" not in claimer._team_locks
    assert "team-B" not in claimer._team_locks
    assert set(removed) == {"team-A", "team-B"}


@pytest.mark.asyncio
async def test_cleanup_idle_teams_keeps_active_team() -> None:
    """cleanup_idle_teams does not remove a lock for a team currently advancing."""
    first_started = asyncio.Event()
    proceed = asyncio.Event()

    async def slow_get_tasks(team_id: str) -> list[dict]:
        first_started.set()
        await proceed.wait()
        return []

    claimer = TaskClaimer(
        get_tasks=AsyncMock(side_effect=slow_get_tasks),
        claim_task=AsyncMock(return_value=True),
    )

    advance_task = asyncio.get_event_loop().create_task(claimer.advance_dag("team-X"))
    await first_started.wait()

    # While team-X is actively advancing, cleanup should not remove it
    removed = claimer.cleanup_idle_teams()
    assert "team-X" not in removed
    assert "team-X" in claimer._team_locks

    proceed.set()
    await advance_task


@pytest.mark.asyncio
async def test_cleanup_idle_teams_empty_is_noop() -> None:
    """cleanup_idle_teams on a fresh claimer returns an empty list."""
    claimer, _, _ = _make_claimer()
    removed = claimer.cleanup_idle_teams()
    assert removed == []


# ---------------------------------------------------------------------------
# Issue #300 — startup_scan
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_startup_scan_dispatches_advance_for_each_team() -> None:
    """startup_scan schedules advance_dag_tracked for every supplied team_id."""
    teams = {"team-A": [{"id": "a1"}], "team-B": [{"id": "b1"}]}
    claimer, get_tasks_mock, claim_mock = _make_claimer(teams)

    dispatched_tasks = await claimer.startup_scan(["team-A", "team-B"])

    # All tasks should be asyncio.Task instances
    assert len(dispatched_tasks) == 2
    for t in dispatched_tasks:
        assert isinstance(t, asyncio.Task)

    # Let them all complete
    await asyncio.gather(*dispatched_tasks)

    # Both teams should have been fetched
    assert get_tasks_mock.await_count == 2
    called_teams = {c.args[0] for c in get_tasks_mock.call_args_list}
    assert called_teams == {"team-A", "team-B"}


@pytest.mark.asyncio
async def test_startup_scan_empty_list_returns_no_tasks() -> None:
    """startup_scan with an empty list produces no tasks."""
    claimer, get_tasks_mock, _ = _make_claimer()

    dispatched_tasks = await claimer.startup_scan([])

    assert dispatched_tasks == []
    get_tasks_mock.assert_not_awaited()


@pytest.mark.asyncio
async def test_startup_scan_tasks_are_tracked_in_flight() -> None:
    """Tasks returned by startup_scan appear in the in_flight_count until done."""
    proceed = asyncio.Event()

    async def slow_get_tasks(team_id: str) -> list[dict]:
        await proceed.wait()
        return []

    claimer = TaskClaimer(
        get_tasks=AsyncMock(side_effect=slow_get_tasks),
        claim_task=AsyncMock(return_value=True),
    )

    dispatched_tasks = await claimer.startup_scan(["team-A", "team-B"])
    assert claimer.in_flight_count == 2

    proceed.set()
    await asyncio.gather(*dispatched_tasks)
    assert claimer.in_flight_count == 0
