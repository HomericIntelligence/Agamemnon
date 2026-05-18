"""Pydantic v2 models for the Agamemnon REST API."""

from __future__ import annotations

import os
from typing import Any

from pydantic import BaseModel, Field

# ── Configuration ─────────────────────────────────────────────────────────────


def _default_api_key() -> str | None:
    """Return the AGAMEMNON_API_KEY env var, or None if unset/empty.

    An explicitly empty value is treated as unset so that the server-side
    "development escape hatch" semantics line up: empty env var → no auth header
    is sent.
    """
    value = os.environ.get("AGAMEMNON_API_KEY")
    if value is None or value == "":
        return None
    return value


class AgamemnonConfig(BaseModel):
    """Configuration for the AgamemnonClient.

    When ``api_key`` is set (either explicitly or via the ``AGAMEMNON_API_KEY``
    environment variable), the client will send an
    ``Authorization: Bearer <api_key>`` header on every request. If
    ``api_key`` is ``None`` (the default when the env var is unset/empty),
    no auth header is added — appropriate for talking to a server that was
    itself launched without ``AGAMEMNON_API_KEY``.
    """

    host: str = "localhost"
    port: int = 8080
    timeout: float = 30.0
    api_key: str | None = Field(default_factory=_default_api_key)


# ── Health / Version ──────────────────────────────────────────────────────────


class HealthResponse(BaseModel):
    """Response from GET /v1/health."""

    status: str


class VersionResponse(BaseModel):
    """Response from GET /v1/version."""

    version: str
    name: str


# ── Agents ────────────────────────────────────────────────────────────────────


class Agent(BaseModel):
    """An agent managed by Agamemnon."""

    id: str
    name: str
    label: str = ""
    program: str = ""
    workingDirectory: str = ""
    programArgs: list[str] = Field(default_factory=list)
    taskDescription: str = ""
    tags: list[str] = Field(default_factory=list)
    owner: str = ""
    role: str = "worker"
    host: str = "local"
    status: str = "offline"
    createdAt: str = ""


class AgentCreate(BaseModel):
    """Request body for POST /v1/agents."""

    name: str
    label: str = ""
    program: str = ""
    workingDirectory: str = ""
    programArgs: list[str] = Field(default_factory=list)
    taskDescription: str = ""
    tags: list[str] = Field(default_factory=list)
    owner: str = ""
    role: str = "worker"
    host: str = "local"


class AgentDockerCreate(BaseModel):
    """Request body for POST /v1/agents/docker."""

    name: str
    label: str = ""
    program: str = ""
    workingDirectory: str = ""
    programArgs: list[str] = Field(default_factory=list)
    taskDescription: str = ""
    tags: list[str] = Field(default_factory=list)
    owner: str = ""
    role: str = "worker"
    host: str = "docker"
    image: str = ""
    hostId: str = ""


class AgentUpdate(BaseModel):
    """Request body for PATCH /v1/agents/{id}."""

    model_config = {"extra": "allow"}

    name: str | None = None
    label: str | None = None
    program: str | None = None
    workingDirectory: str | None = None
    programArgs: list[str] | None = None
    taskDescription: str | None = None
    tags: list[str] | None = None
    owner: str | None = None
    role: str | None = None
    host: str | None = None
    status: str | None = None


# ── Teams ─────────────────────────────────────────────────────────────────────


class Team(BaseModel):
    """A team managed by Agamemnon."""

    id: str
    name: str
    agentIds: list[str] = Field(default_factory=list)
    createdAt: str = ""


class TeamCreate(BaseModel):
    """Request body for POST /v1/teams."""

    name: str
    agentIds: list[str] = Field(default_factory=list)


class TeamUpdate(BaseModel):
    """Request body for PUT /v1/teams/{id}."""

    name: str
    agentIds: list[str] = Field(default_factory=list)


# ── Tasks ─────────────────────────────────────────────────────────────────────


class Task(BaseModel):
    """A task managed by Agamemnon."""

    id: str
    teamId: str
    subject: str
    description: str = ""
    assigneeAgentId: str = ""
    blockedBy: list[str] = Field(default_factory=list)
    type: str = "general"
    status: str = "pending"
    createdAt: str = ""
    completedAt: str | None = None


class TaskCreate(BaseModel):
    """Request body for POST /v1/teams/{team_id}/tasks."""

    subject: str
    description: str = ""
    assigneeAgentId: str = ""
    blockedBy: list[str] = Field(default_factory=list)
    type: str = "general"


class TaskUpdate(BaseModel):
    """Request body for PUT or PATCH /v1/teams/{team_id}/tasks/{task_id}."""

    model_config = {"extra": "allow"}

    subject: str | None = None
    description: str | None = None
    assigneeAgentId: str | None = None
    blockedBy: list[str] | None = None
    type: str | None = None
    status: str | None = None


# ── Chaos ─────────────────────────────────────────────────────────────────────


class FailureSpec(BaseModel):
    """Specification for a chaos fault injection."""

    type: str
    parameters: dict[str, Any] = Field(default_factory=dict)


class InjectionResult(BaseModel):
    """A chaos fault entry returned by the API."""

    id: str
    type: str
    active: bool = True
    createdAt: str = ""


class ChaosEntry(BaseModel):
    """Alias for InjectionResult — a chaos fault entry."""

    id: str
    type: str
    active: bool = True
    createdAt: str = ""
