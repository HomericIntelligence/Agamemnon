from __future__ import annotations

from agamemnon.orchestration.models import Agent, Task


def make_agent(
    id: str = "agent-1",
    name: str = "Agent 1",
    host: str = "host-1",
    status: str = "active",
    session_status: str = "online",
    task_description: str = "",
    program: str = "",
    current_task_id: str | None = None,
) -> Agent:
    """Create an Agent test fixture with sensible defaults."""
    return Agent(
        id=id,
        name=name,
        host=host,
        status=status,
        session_status=session_status,
        task_description=task_description,
        program=program,
        current_task_id=current_task_id,
    )


def make_task(
    id: str = "task-1",
    title: str = "Task 1",
    status: str = "pending",
    dependencies: list[str] | None = None,
    assigned_agent_id: str | None = None,
) -> Task:
    """Create a Task test fixture with sensible defaults."""
    return Task(
        id=id,
        title=title,
        status=status,
        dependencies=dependencies or [],
        assigned_agent_id=assigned_agent_id,
    )
