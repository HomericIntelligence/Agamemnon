"""Async HTTP client for the Agamemnon REST API."""

from __future__ import annotations

from types import TracebackType
from typing import Any

import httpx

from agamemnon_client.errors import (
    AgamemnonAPIError,
    AgamemnonConnectionError,
)
from agamemnon_client.models import (
    AgamemnonConfig,
    Agent,
    AgentCreate,
    AgentDockerCreate,
    AgentUpdate,
    ChaosEntry,
    FailureSpec,
    HealthResponse,
    InjectionResult,
    Task,
    TaskCreate,
    TaskUpdate,
    Team,
    TeamCreate,
    TeamUpdate,
    VersionResponse,
)


class AgamemnonClient:
    """Async HTTP client for Agamemnon's REST API.

    Usage::

        async with AgamemnonClient(AgamemnonConfig(host="localhost", port=8080)) as client:
            health = await client.health()
    """

    def __init__(self, config: AgamemnonConfig | None = None) -> None:
        if config is None:
            config = AgamemnonConfig()
        self._config = config
        self._base_url = f"http://{config.host}:{config.port}"
        self._client = httpx.AsyncClient(
            base_url=self._base_url,
            timeout=config.timeout,
        )

    async def __aenter__(self) -> AgamemnonClient:
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        await self._client.aclose()

    async def aclose(self) -> None:
        """Close the underlying HTTP client."""
        await self._client.aclose()

    # ── Internal request helper ────────────────────────────────────────────────

    async def _request(self, method: str, path: str, **kwargs: Any) -> Any:
        """Send an HTTP request and return the parsed JSON response.

        If ``self._config.api_key`` is set, an
        ``Authorization: Bearer <key>`` header is added to every request.
        Caller-supplied ``Authorization`` headers (passed via ``headers=...``)
        are preserved and take precedence.

        Raises:
            AgamemnonConnectionError: If the server cannot be reached.
            AgamemnonAPIError: If the server returns a non-2xx status.
        """
        if self._config.api_key:
            headers = dict(kwargs.pop("headers", {}) or {})
            headers.setdefault("Authorization", f"Bearer {self._config.api_key}")
            kwargs["headers"] = headers
        try:
            response = await self._client.request(method, path, **kwargs)
        except httpx.ConnectError as exc:
            raise AgamemnonConnectionError(
                f"Cannot connect to Agamemnon at {self._base_url}: {exc}"
            ) from exc
        except httpx.TimeoutException as exc:
            raise AgamemnonConnectionError(
                f"Request to Agamemnon timed out: {exc}"
            ) from exc

        if response.is_error:
            try:
                detail = response.json().get("error", response.text)
            except Exception:
                detail = response.text
            raise AgamemnonAPIError(response.status_code, detail)

        if response.status_code == 204 or not response.content:
            return None

        return response.json()

    # ── Health / Version ──────────────────────────────────────────────────────

    async def health(self) -> HealthResponse | None:
        """Return service health status, or None if the service is unreachable.

        This method never raises — it returns None on any error.
        """
        try:
            data = await self._request("GET", "/v1/health")
            return HealthResponse.model_validate(data)
        except Exception:
            return None

    async def version(self) -> VersionResponse:
        """Return the service version."""
        data = await self._request("GET", "/v1/version")
        return VersionResponse.model_validate(data)

    # ── Agents ────────────────────────────────────────────────────────────────

    async def list_agents(self) -> list[Agent]:
        """List all agents."""
        data = await self._request("GET", "/v1/agents")
        agents = data if isinstance(data, list) else data.get("agents", [])
        return [Agent.model_validate(a) for a in agents]

    async def create_agent(self, agent: AgentCreate) -> Agent:
        """Create a new agent. Returns the created agent."""
        data = await self._request(
            "POST", "/v1/agents", json=agent.model_dump(exclude_none=True)
        )
        return Agent.model_validate(data.get("agent", data))

    async def create_docker_agent(self, agent: AgentDockerCreate) -> Agent:
        """Create a new Docker agent. Returns the created agent."""
        data = await self._request(
            "POST", "/v1/agents/docker", json=agent.model_dump(exclude_none=True)
        )
        return Agent.model_validate(data.get("agent", data))

    async def get_agent(self, agent_id: str) -> Agent:
        """Get an agent by ID."""
        data = await self._request("GET", f"/v1/agents/{agent_id}")
        return Agent.model_validate(data.get("agent", data))

    async def get_agent_by_name(self, name: str) -> Agent:
        """Get an agent by name."""
        data = await self._request("GET", f"/v1/agents/by-name/{name}")
        return Agent.model_validate(data.get("agent", data))

    async def update_agent(self, agent_id: str, update: AgentUpdate) -> Agent:
        """Partially update an agent (PATCH)."""
        data = await self._request(
            "PATCH",
            f"/v1/agents/{agent_id}",
            json=update.model_dump(exclude_none=True),
        )
        return Agent.model_validate(data.get("agent", data))

    async def start_agent(self, agent_id: str) -> Agent:
        """Start an agent."""
        data = await self._request("POST", f"/v1/agents/{agent_id}/start")
        return Agent.model_validate(data.get("agent", data))

    async def stop_agent(self, agent_id: str) -> Agent:
        """Stop an agent."""
        data = await self._request("POST", f"/v1/agents/{agent_id}/stop")
        return Agent.model_validate(data.get("agent", data))

    async def delete_agent(self, agent_id: str) -> str:
        """Delete an agent. Returns the deleted agent's ID."""
        data = await self._request("DELETE", f"/v1/agents/{agent_id}")
        return str(data.get("deleted", agent_id))

    # ── Teams ─────────────────────────────────────────────────────────────────

    async def list_teams(self) -> list[Team]:
        """List all teams."""
        data = await self._request("GET", "/v1/teams")
        teams = data if isinstance(data, list) else data.get("teams", [])
        return [Team.model_validate(t) for t in teams]

    async def create_team(self, team: TeamCreate) -> Team:
        """Create a new team. Returns the created team."""
        data = await self._request(
            "POST", "/v1/teams", json=team.model_dump(exclude_none=True)
        )
        return Team.model_validate(data.get("team", data))

    async def get_team(self, team_id: str) -> Team:
        """Get a team by ID."""
        data = await self._request("GET", f"/v1/teams/{team_id}")
        return Team.model_validate(data.get("team", data))

    async def update_team(self, team_id: str, update: TeamUpdate) -> Team:
        """Fully replace a team (PUT)."""
        data = await self._request(
            "PUT", f"/v1/teams/{team_id}", json=update.model_dump()
        )
        return Team.model_validate(data.get("team", data))

    async def delete_team(self, team_id: str) -> str:
        """Delete a team. Returns the deleted team's ID."""
        data = await self._request("DELETE", f"/v1/teams/{team_id}")
        return str(data.get("deleted", team_id))

    # ── Tasks ─────────────────────────────────────────────────────────────────

    async def list_tasks(self) -> list[Task]:
        """List all tasks across all teams."""
        data = await self._request("GET", "/v1/tasks")
        tasks = data if isinstance(data, list) else data.get("tasks", [])
        return [Task.model_validate(t) for t in tasks]

    async def list_team_tasks(self, team_id: str) -> list[Task]:
        """List all tasks for a specific team."""
        data = await self._request("GET", f"/v1/teams/{team_id}/tasks")
        tasks = data if isinstance(data, list) else data.get("tasks", [])
        return [Task.model_validate(t) for t in tasks]

    async def create_task(self, team_id: str, task: TaskCreate) -> Task:
        """Create a task in a team. Returns the created task."""
        data = await self._request(
            "POST",
            f"/v1/teams/{team_id}/tasks",
            json=task.model_dump(exclude_none=True),
        )
        return Task.model_validate(data.get("task", data))

    async def get_task(self, team_id: str, task_id: str) -> Task:
        """Get a specific task."""
        data = await self._request("GET", f"/v1/teams/{team_id}/tasks/{task_id}")
        return Task.model_validate(data.get("task", data))

    async def update_task(
        self, team_id: str, task_id: str, update: TaskUpdate, *, partial: bool = False
    ) -> Task:
        """Update a task.

        Args:
            team_id: The team ID.
            task_id: The task ID.
            update: The update payload.
            partial: If True, uses PATCH (partial update). If False, uses PUT (full replace).
        """
        method = "PATCH" if partial else "PUT"
        payload = update.model_dump(exclude_none=True) if partial else update.model_dump()
        data = await self._request(
            method,
            f"/v1/teams/{team_id}/tasks/{task_id}",
            json=payload,
        )
        return Task.model_validate(data.get("task", data))

    # ── Chaos ─────────────────────────────────────────────────────────────────

    async def list_chaos(self) -> list[ChaosEntry]:
        """List all active chaos faults."""
        data = await self._request("GET", "/v1/chaos")
        faults = data if isinstance(data, list) else data.get("faults", [])
        return [ChaosEntry.model_validate(f) for f in faults]

    async def inject_chaos(
        self, fault_type: str, spec: FailureSpec | None = None
    ) -> InjectionResult:
        """Inject a chaos fault of the given type."""
        body = spec.model_dump() if spec is not None else {}
        data = await self._request("POST", f"/v1/chaos/{fault_type}", json=body)
        return InjectionResult.model_validate(data.get("fault", data))

    async def delete_chaos(self, fault_id: str) -> str:
        """Remove a chaos fault. Returns the deleted fault's ID."""
        data = await self._request("DELETE", f"/v1/chaos/{fault_id}")
        return str(data.get("deleted", fault_id))
