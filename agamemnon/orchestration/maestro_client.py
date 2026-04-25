from __future__ import annotations

from typing import Any, Optional

from .models import Agent, Task, TERMINAL_STATUSES
from .validation import validate_id


def _agent_from_api(data: dict[str, Any]) -> Agent:
    """Parse an Agent from AI Maestro API response payload."""
    agent = data.get("agent", data)
    session = agent.get("session", {})
    return Agent(
        id=agent["id"],
        name=agent.get("name", ""),
        host=agent.get("hostId", ""),
        status=agent.get("status", "active"),
        session_status=session.get("status", "unknown"),
        task_description=agent.get("taskDescription", ""),
        program=agent.get("program", ""),
        current_task_id=agent.get("currentTaskId"),
    )


def _task_from_api(data: dict[str, Any]) -> Task:
    """Parse a Task from AI Maestro API response payload."""
    return Task(
        id=data["id"],
        title=data.get("title", ""),
        status=data.get("status", "pending"),
        dependencies=data.get("dependencies", []),
        assigned_agent_id=data.get("assignedAgentId"),
    )


class MaestroClient:
    """Async HTTP client for the AI Maestro REST API."""

    def __init__(self, http_client: Any) -> None:
        self._client = http_client

    async def _with_retries(self, fn: Any) -> Any:
        return await fn()

    @staticmethod
    def _extract_key(body: Any, key: str, context: str) -> list[Any]:
        if isinstance(body, list):
            return body
        if isinstance(body, dict) and key in body:
            return body[key]
        raise ValueError(f"Unexpected response shape for {context}: {body!r}")

    async def get_agents(self) -> list[Agent]:
        """Fetch active (non-soft-deleted) agents from the API."""
        resp = await self._with_retries(
            lambda: self._client.get("/api/agents/unified")
        )
        body = resp.json()
        raw_agents = self._extract_key(body, "agents", "GET /api/agents/unified")
        return [
            _agent_from_api(entry)
            for entry in raw_agents
            if entry.get("agent", entry).get("deletedAt") is None
        ]

    async def get_tasks(self) -> list[Task]:
        """Fetch all tasks from the API."""
        resp = await self._with_retries(
            lambda: self._client.get("/api/tasks")
        )
        body = resp.json()
        raw_tasks = self._extract_key(body, "tasks", "GET /api/tasks")
        return [_task_from_api(t) for t in raw_tasks]

    async def assign_task(self, task_id: str, agent_id: str) -> None:
        """Assign a task to an agent via the API."""
        validate_id(task_id, "task_id")
        validate_id(agent_id, "agent_id")
        await self._with_retries(
            lambda: self._client.put(
                f"/api/tasks/{task_id}/assign",
                json={"agentId": agent_id},
            )
        )
