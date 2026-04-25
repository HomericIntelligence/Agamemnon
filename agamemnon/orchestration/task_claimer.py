"""Per-team concurrency guard for advance_dag task claiming."""

from __future__ import annotations

import asyncio
import logging
from typing import Any, Callable, Coroutine

logger = logging.getLogger(__name__)


class TaskClaimer:
    """Claims ready tasks for a team by advancing its DAG.

    Uses a per-team asyncio.Lock to prevent concurrent advance_dag calls for
    the same team from racing each other. If a call arrives while one is already
    in-flight for the same team, the new call is coalesced (skipped) rather than
    queued, since the in-flight call will already observe the latest state.
    """

    def __init__(
        self,
        get_tasks: Callable[[str], Coroutine[Any, Any, list[dict[str, Any]]]],
        claim_task: Callable[[str, str], Coroutine[Any, Any, bool]],
    ) -> None:
        """Initialize TaskClaimer.

        Args:
            get_tasks: Async callable that fetches tasks for a team_id.
            claim_task: Async callable that attempts to claim a task by team_id
                and task_id. Returns True if the claim succeeded.
        """
        self._get_tasks = get_tasks
        self._claim_task = claim_task
        self._team_locks: dict[str, asyncio.Lock] = {}
        self._advancing: set[str] = set()
        self._in_flight: set[asyncio.Task[Any]] = set()
        # Issue #295: track coalesced (skipped) advance_dag calls per team
        self._coalesced_count: dict[str, int] = {}

    @property
    def in_flight_count(self) -> int:
        """Number of advance_dag_tracked tasks currently executing."""
        return len(self._in_flight)

    def get_coalesced_count(self, team_id: str) -> int:
        """Return the number of coalesced (skipped) advance_dag calls for a team.

        A call is coalesced when advance_dag is invoked while another call for
        the same team is already in-flight.

        Args:
            team_id: The team whose coalesced count to retrieve.

        Returns:
            Number of coalesced calls recorded for this team (0 if none).
        """
        return self._coalesced_count.get(team_id, 0)

    def get_metrics(self) -> dict[str, Any]:
        """Return a snapshot of coalesced-call metrics for all teams.

        Returns:
            A dict mapping team_id to its coalesced call count.
        """
        return dict(self._coalesced_count)

    def _get_team_lock(self, team_id: str) -> asyncio.Lock:
        """Return the per-team lock, creating it lazily if needed."""
        if team_id not in self._team_locks:
            self._team_locks[team_id] = asyncio.Lock()
        return self._team_locks[team_id]

    def cleanup_idle_teams(self) -> list[str]:
        """Remove per-team locks for teams that are not currently advancing.

        Teams with an active advance_dag call in progress are kept. All other
        teams have their lock entry removed from ``_team_locks``, preventing
        unbounded growth as new team IDs are seen over time.

        Returns:
            List of team IDs whose locks were removed.
        """
        idle_teams = [t for t in self._team_locks if t not in self._advancing]
        for team_id in idle_teams:
            del self._team_locks[team_id]
        if idle_teams:
            logger.debug("cleanup_idle_teams: removed %d team locks", len(idle_teams))
        return idle_teams

    async def advance_dag(self, team_id: str) -> list[str]:
        """Fetch ready tasks for team_id and claim them.

        At most one advance_dag call executes per team at any time. If a call
        arrives while another is already in-flight for the same team, the new
        call is skipped (coalesced) and returns an empty list.

        Args:
            team_id: The team whose DAG should be advanced.

        Returns:
            List of task IDs that were successfully claimed, or an empty list
            if this call was coalesced.
        """
        if team_id in self._advancing:
            logger.info(
                "advance_dag already in-flight for team %s -- skipping", team_id
            )
            self._coalesced_count[team_id] = self._coalesced_count.get(team_id, 0) + 1
            return []

        self._advancing.add(team_id)
        try:
            async with self._get_team_lock(team_id):
                tasks = await self._get_tasks(team_id)
                claimed: list[str] = []
                for task in tasks:
                    task_id: str = task["id"]
                    if await self._claim_task(team_id, task_id):
                        claimed.append(task_id)
                return claimed
        finally:
            self._advancing.discard(team_id)

    def advance_dag_tracked(self, team_id: str) -> asyncio.Task[Any]:
        """Schedule advance_dag(team_id) as a tracked asyncio Task.

        The returned Task is registered in _in_flight and removed when done.
        Use drain() to wait for all in-flight tasks to complete.

        Args:
            team_id: The team whose DAG should be advanced.

        Returns:
            The asyncio.Task wrapping the advance_dag coroutine.
        """
        task: asyncio.Task[Any] = asyncio.get_event_loop().create_task(
            self.advance_dag(team_id)
        )
        self._in_flight.add(task)
        task.add_done_callback(self._in_flight.discard)
        return task

    async def drain(self, timeout: float) -> bool:
        """Wait for all in-flight advance_dag_tracked tasks to complete.

        Args:
            timeout: Maximum seconds to wait before giving up.

        Returns:
            True if all tasks completed within the timeout, False otherwise.
        """
        if not self._in_flight:
            return True
        try:
            await asyncio.wait_for(
                asyncio.gather(*self._in_flight, return_exceptions=True),
                timeout=timeout,
            )
            return True
        except asyncio.TimeoutError:
            logger.warning("drain_timeout", extra={"timeout": timeout})
            return False

    async def startup_scan(self, team_ids: list[str]) -> list[asyncio.Task[Any]]:
        """Dispatch advance_dag_tracked for each team in team_ids.

        This method is intended to be called at daemon startup to prime the
        claim pipeline for all known teams. It schedules one tracked task per
        team and returns the list of tasks so the caller can optionally await
        them or pass them to drain().

        Args:
            team_ids: List of team IDs to scan on startup.

        Returns:
            List of asyncio.Task objects, one per team_id.
        """
        tasks: list[asyncio.Task[Any]] = []
        for team_id in team_ids:
            tasks.append(self.advance_dag_tracked(team_id))
        return tasks
