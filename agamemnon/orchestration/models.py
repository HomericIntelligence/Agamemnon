from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from pydantic import BaseModel, model_validator


TERMINAL_STATUSES: frozenset[str] = frozenset({"completed", "failed", "error", "cancelled"})


@dataclass
class Task:
    id: str
    title: str
    status: str
    dependencies: list[str] = field(default_factory=list)
    assigned_agent_id: Optional[str] = None


@dataclass
class Agent:
    id: str
    name: str
    host: str = ""
    status: str = "active"
    session_status: str = "online"
    task_description: str = ""
    program: str = ""
    current_task_id: Optional[str] = None


class TaskEvent(BaseModel):
    """Validated NATS task event payload.

    Normalizes status from three possible locations:
    1. Top-level ``status`` field
    2. Nested ``data.status`` (Hermes format per issue #107)
    3. ``newStatus`` field

    All fields are optional — NATS payloads can be sparse or use different
    schemas depending on the emitting component.
    """

    status: str | None = None
    newStatus: str | None = None  # noqa: N815 — matches camelCase JSON payload
    data: dict | None = None
    taskId: str | None = None  # noqa: N815
    teamId: str | None = None  # noqa: N815
    effective_status: str | None = None

    @model_validator(mode="after")
    def _resolve_effective_status(self) -> TaskEvent:
        resolved = self.status
        if not resolved and self.data:
            resolved = self.data.get("status")
        if not resolved:
            resolved = self.newStatus
        self.effective_status = resolved
        return self
