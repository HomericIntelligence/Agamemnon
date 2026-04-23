from __future__ import annotations

from typing import Optional

from .models import Agent, Task, TERMINAL_STATUSES
from .maestro_client import MaestroClient


class DAGWalker:
    """Walks a task DAG and assigns ready tasks to available agents."""

    def __init__(
        self,
        tasks: list[Task],
        agents: list[Agent],
        client: Optional[MaestroClient] = None,
    ) -> None:
        self.tasks = tasks
        self.agents = agents
        self.client = client

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

    def validate_no_cycles(self) -> bool:
        """Return True if the task DAG is acyclic, False if a cycle exists.

        Uses an iterative three-color DFS (WHITE/GRAY/BLACK) to avoid
        RecursionError on deep dependency chains. A GRAY node encountered
        during traversal indicates a back-edge and therefore a cycle.
        """
        # Build adjacency: task_id -> list of dependency ids present in the graph
        task_ids = {t.id for t in self.tasks}
        adj: dict[str, list[str]] = {
            t.id: [dep for dep in t.dependencies if dep in task_ids]
            for t in self.tasks
        }

        WHITE, GRAY, BLACK = 0, 1, 2
        color: dict[str, int] = {t.id: WHITE for t in self.tasks}

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
                    # Back-edge: cycle detected
                    return False

                if color[node] == BLACK:
                    continue

                color[node] = GRAY
                # Push post-process sentinel before neighbors
                stack.append((node, True))
                for neighbor in adj[node]:
                    if color[neighbor] == GRAY:
                        return False
                    if color[neighbor] == WHITE:
                        stack.append((neighbor, False))

        return True

    async def advance_dag(self) -> list[tuple[Task, Agent]]:
        """Assign ready tasks to available agents, returning the assignments made.

        Agents are marked busy immediately upon selection so a single call cannot
        double-assign the same agent even if the local list is stale.
        """
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
