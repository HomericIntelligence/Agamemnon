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
