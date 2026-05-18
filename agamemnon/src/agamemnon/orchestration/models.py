from __future__ import annotations

from dataclasses import dataclass, field

from pydantic import BaseModel, ConfigDict, model_validator



TERMINAL_STATUSES: frozenset[str] = frozenset({"completed", "failed", "error", "cancelled"})


def resolve_event_status(
    status: str | None,
    data: dict[str, object] | None,
    new_status: str | None,
) -> str | None:
    """Resolve effective status from three possible sources in priority order.

    Priority: status > data["status"] > new_status
    """
    resolved: str | None = status
    if not resolved and data:
        candidate = data.get("status")
        resolved = candidate if isinstance(candidate, str) else None
    if not resolved:
        resolved = new_status
    return resolved


@dataclass
class Task:
    id: str
    title: str
    status: str
    dependencies: list[str] = field(default_factory=list)
    assigned_agent_id: str | None = None


@dataclass
class Agent:
    id: str
    name: str
    host: str = ""
    status: str = "active"
    session_status: str = "online"
    task_description: str = ""
    program: str = ""
    current_task_id: str | None = None


class TaskEvent(BaseModel):
    """Validated NATS task event payload.

    Normalizes status from three possible locations:
    1. Top-level ``status`` field
    2. Nested ``data.status`` (Hermes format per issue #107)
    3. ``newStatus`` field

    All fields are optional — NATS payloads can be sparse or use different
    schemas depending on the emitting component.
    """

    model_config = ConfigDict(extra="forbid")

    status: str | None = None
    newStatus: str | None = None  # noqa: N815 — matches camelCase JSON payload
    data: dict[str, object] | None = None
    taskId: str | None = None  # noqa: N815
    teamId: str | None = None  # noqa: N815
    effective_status: str | None = None

    @model_validator(mode="after")
    def _resolve_effective_status(self) -> TaskEvent:
        self.effective_status = resolve_event_status(self.status, self.data, self.newStatus)
        return self
