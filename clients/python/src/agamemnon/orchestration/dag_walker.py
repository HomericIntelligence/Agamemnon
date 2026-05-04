from __future__ import annotations

import asyncio
import logging
from typing import Any, Optional

from agamemnon.orchestration.models import Agent, Task, TERMINAL_STATUSES

logger = logging.getLogger(__name__)


class DAGWalker:
    """Walks a task DAG and assigns ready tasks to available agents."""

    def __init__(
        self,
        tasks: list[Task],
        agents: list[Agent],
        client: Optional[Any] = None,
        scan_interval: float = 60.0,
    ) -> None:
        self.tasks = tasks
        self.agents = agents
        self.client = client
        self.scan_interval = scan_interval
        self._scan_task: Optional[asyncio.Task[None]] = None

    def get_available_agents(self) -> list[Agent]:
        """Return agents that are active, online, and not currently assigned a task."""
        return [
            a
            for a in self.agents
            if a.status == "active"
            and a.session_status == "online"
            and a.current_task_id is None
        ]

    def get_ready_tasks(self) -> list[Task]:
        """Return tasks whose dependencies are all in terminal status."""
        completed_ids = {
            t.id for t in self.tasks if t.status in TERMINAL_STATUSES
        }
        return [
            t
            for t in self.tasks
            if t.status == "pending"
            and t.assigned_agent_id is None
            and all(dep in completed_ids for dep in t.dependencies)
        ]

    def _find_cycle_path(self) -> list[str]:
        """Return the IDs of tasks forming a cycle, or an empty list if the DAG is acyclic.

        Uses an iterative three-color DFS (WHITE/GRAY/BLACK) with parent tracking to
        reconstruct the cycle path when a back-edge is found.  An unknown dependency ID
        (one not present in ``self.tasks``) raises :exc:`ValueError` immediately.
        """
        task_ids = {t.id for t in self.tasks}

        # Validate all dependency IDs before traversal (Issue #233).
        for t in self.tasks:
            for dep_id in t.dependencies:
                if dep_id not in task_ids:
                    raise ValueError(
                        f"Unknown dependency {dep_id!r} referenced by task {t.id!r}"
                    )

        adj: dict[str, list[str]] = {t.id: list(t.dependencies) for t in self.tasks}

        WHITE, GRAY, BLACK = 0, 1, 2
        color: dict[str, int] = {t.id: WHITE for t in self.tasks}
        # parent[node] = the node that first pushed *node* onto the DFS stack
        parent: dict[str, str | None] = {t.id: None for t in self.tasks}

        for start in task_ids:
            if color[start] != WHITE:
                continue

            # Stack holds (node_id, processed) tuples.
            # processed=False: first visit — mark GRAY, push neighbors.
            # processed=True:  all neighbors done — mark BLACK.
            stack: list[tuple[str, bool]] = [(start, False)]

            while stack:
                node, processed = stack.pop()

                if processed:
                    color[node] = BLACK
                    continue

                if color[node] == GRAY:
                    # Back-edge detected; reconstruct cycle starting from *node*.
                    cycle: list[str] = [node]
                    cur: str | None = parent[node]
                    while cur is not None and cur != node:
                        cycle.append(cur)
                        cur = parent[cur]
                    cycle.append(node)
                    cycle.reverse()
                    return cycle

                if color[node] == BLACK:
                    continue

                color[node] = GRAY
                stack.append((node, True))
                for neighbor in adj[node]:
                    if color[neighbor] == GRAY:
                        # Immediate back-edge to an already-gray neighbor.
                        cycle = [neighbor]
                        cur2: str | None = node
                        while cur2 is not None and cur2 != neighbor:
                            cycle.append(cur2)
                            cur2 = parent[cur2]
                        cycle.append(neighbor)
                        cycle.reverse()
                        return cycle
                    if color[neighbor] == WHITE:
                        parent[neighbor] = node
                        stack.append((neighbor, False))

        return []

    def validate_no_cycles(self) -> bool:
        """Return True if the task DAG is acyclic, False if a cycle exists.

        Delegates to :meth:`_find_cycle_path`.  Unknown dependency IDs cause a
        :exc:`ValueError` to be raised (see :meth:`_find_cycle_path`).
        """
        return len(self._find_cycle_path()) == 0

    async def advance_dag(self) -> list[tuple[Task, Agent]]:
        """Assign ready tasks to available agents, returning the assignments made.

        When a ``client`` is configured and it exposes a ``get_agents()`` coroutine,
        the method fetches a fresh agent list before evaluating availability (issue
        #196).  This prevents double-assignment races caused by a stale cached list.

        Raises :exc:`ValueError` if the task DAG contains a cycle or if any task
        references an unknown dependency ID.  Agents are marked busy immediately upon
        selection so a single call cannot double-assign the same agent even if the
        local list is stale.
        """
        cycle_path = self._find_cycle_path()
        if cycle_path:
            raise ValueError(f"Cycle detected in task DAG: {cycle_path}")

        # Issue #196: refresh the agent list from the client before assigning so
        # that stale in-memory state doesn't cause double-assignment.
        if self.client is not None and hasattr(self.client, "get_agents"):
            try:
                fresh_agents = await self.client.get_agents()
                if fresh_agents is not None:
                    self.agents = fresh_agents
            except Exception:  # noqa: BLE001
                logger.warning(
                    "advance_dag: get_agents() failed — falling back to cached list",
                    exc_info=True,
                )

        assignments: list[tuple[Task, Agent]] = []
        available = self.get_available_agents()

        for task in self.get_ready_tasks():
            if not available:
                break

            agent = available.pop(0)
            # Mark agent busy immediately — guards against double-assignment within
            # this call if the list was stale when we entered.
            agent.current_task_id = task.id
            task.assigned_agent_id = agent.id

            if self.client is not None:
                await self.client.assign_task(task.id, agent.id)

            assignments.append((task, agent))

        return assignments

    async def _background_scan_loop(self, stop_event: asyncio.Event) -> None:
        """Periodically call :meth:`advance_dag` as a safety net (issue #98).

        Uses ``asyncio.wait_for`` on *stop_event* so the loop wakes up
        immediately on shutdown rather than sleeping for the full interval.
        """
        logger.info(
            "DAGWalker background scan started (interval=%.1fs)", self.scan_interval
        )
        while not stop_event.is_set():
            try:
                await asyncio.wait_for(
                    stop_event.wait(), timeout=self.scan_interval
                )
                # stop_event was set — exit cleanly.
                break
            except asyncio.TimeoutError:
                pass
            try:
                await self.advance_dag()
            except Exception:  # noqa: BLE001
                logger.exception("DAGWalker background scan failed — continuing")
        logger.info("DAGWalker background scan stopped")

    def start_background_scan(
        self, stop_event: asyncio.Event
    ) -> "asyncio.Task[None]":
        """Schedule a background scan loop and return the created task (issue #98).

        The caller is responsible for cancelling or awaiting the returned task on
        shutdown.  Passing *stop_event* allows a graceful, prompt exit.
        """
        self._scan_task = asyncio.create_task(
            self._background_scan_loop(stop_event), name="dag-background-scan"
        )
        return self._scan_task
