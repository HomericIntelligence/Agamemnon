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

    @property
    def in_flight_count(self) -> int:
        """Number of advance_dag_tracked tasks currently executing."""
        return len(self._in_flight)

    def _get_team_lock(self, team_id: str) -> asyncio.Lock:
        """Return the per-team lock, creating it lazily if needed."""
        if team_id not in self._team_locks:
            self._team_locks[team_id] = asyncio.Lock()
        return self._team_locks[team_id]

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
                "advance_dag already in-flight for team %s — skipping", team_id
            )
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
