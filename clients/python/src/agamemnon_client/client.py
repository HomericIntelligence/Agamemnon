"""Async HTTP client for the Agamemnon REST API."""

from __future__ import annotations

import asyncio
import math
import re
from types import TracebackType
from typing import Any, cast

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

    def __init__(self, config: AgamemnonConfig | None = None, *, trust_env: bool = True) -> None:
        if config is None:
            config = AgamemnonConfig()
        self._config = config
        self._base_url = f"http://{config.host}:{config.port}"
        self._client = httpx.AsyncClient(
            base_url=self._base_url,
            timeout=config.timeout,
            trust_env=trust_env,
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

    @staticmethod
    def _fleet_path(kind: str, identifier: str | None = None) -> str:
        if kind not in {"pools", "workers", "sessions", "executions", "build-jobs", "commands"}:
            raise ValueError("unknown Fleet resource kind")
        path = f"/v1/fleet/{kind}"
        if identifier is not None:
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", identifier):
                raise ValueError("invalid Fleet identifier")
            path += f"/{identifier}"
        return path

    async def fleet_list(self, kind: str) -> dict[str, Any]:
        """Read Fleet records; assignments do not establish observed activity."""
        return cast("dict[str, Any]", await self._request("GET", self._fleet_path(kind)))

    async def fleet_get(self, kind: str, identifier: str) -> dict[str, Any]:
        """Read a resource, or a durable command with its current claim."""
        return cast(
            "dict[str, Any]", await self._request("GET", self._fleet_path(kind, identifier))
        )

    async def fleet_create(self, kind: str, body: dict[str, Any]) -> dict[str, Any]:
        """Create a GitHub-backed record without executing work."""
        return cast(
            "dict[str, Any]", await self._request("POST", self._fleet_path(kind), json=body)
        )

    async def fleet_command(
        self, kind: str, identifier: str, operation: str, body: dict[str, Any]
    ) -> dict[str, Any]:
        """Submit durable control intent. Private input remains a reference."""
        if operation not in {"start", "input", "respond", "interrupt", "cancel", "resume", "drain"}:
            raise ValueError("unsupported Fleet operation")
        return cast(
            "dict[str, Any]",
            await self._request(
                "POST", f"{self._fleet_path(kind, identifier)}/{operation}", json=body
            ),
        )

    async def fleet_acknowledge(
        self, kind: str, identifier: str, fact: dict[str, Any]
    ) -> dict[str, Any]:
        """Record a worker command receipt; this does not complete an issue."""
        return cast(
            "dict[str, Any]",
            await self._request("POST", f"{self._fleet_path(kind, identifier)}/ack", json=fact),
        )

    async def fleet_observe(self, fact: dict[str, Any]) -> dict[str, Any]:
        """Submit worker activity through the same orchestration owner."""
        return cast("dict[str, Any]", await self._request("POST", "/v1/fleet/events", json=fact))

    async def fleet_resolve(
        self, kind: str, identifier: str, decision: dict[str, Any], resolution_key: str
    ) -> dict[str, Any]:
        """Record an authorized manual decision; this does not verify review evidence."""
        if kind not in {"sessions", "executions", "build-jobs"}:
            raise ValueError("resolution requires an execution resource")
        return cast(
            "dict[str, Any]",
            await self._request(
                "POST",
                f"{self._fleet_path(kind, identifier)}/resolve",
                json=decision,
                headers={"X-Fleet-Resolution-Key": resolution_key},
            ),
        )

    async def fleet_projects(self) -> dict[str, Any]:
        """Read derived board health without changing canonical work state."""
        return cast("dict[str, Any]", await self._request("GET", "/v1/fleet/projects"))

    async def fleet_reconcile_projects(self) -> dict[str, Any]:
        """Rebuild only explicitly configured ProjectV2 fields from durable issues."""
        return cast("dict[str, Any]", await self._request("POST", "/v1/fleet/projects/reconcile"))

    async def fleet_events(self, after: int = 0) -> dict[str, Any]:
        """Read durable control transitions; activity telemetry uses its own source cursor."""
        if after < 0:
            raise ValueError("event cursor must be nonnegative")
        return cast(
            "dict[str, Any]",
            await self._request("GET", "/v1/fleet/events", params={"after": after}),
        )

    def _fleet_build_path(self, build_id: str) -> str:
        if not isinstance(build_id, str):
            raise ValueError("invalid Fleet build identifier")
        return self._fleet_path("build-jobs", build_id)

    async def fleet_build_submit(self, body: dict[str, Any]) -> dict[str, Any]:
        """Request recipe admission; the controller validates the complete body."""
        return cast(
            "dict[str, Any]",
            await self._build_request("POST", "/v1/fleet/build-jobs/submit", json=body),
        )

    async def fleet_build_status(self, build_id: str) -> dict[str, Any]:
        """Read the canonical build record without granting execution."""
        return cast(
            "dict[str, Any]", await self._build_request("GET", self._fleet_build_path(build_id))
        )

    async def fleet_build_cancel(self, build_id: str, body: dict[str, Any]) -> dict[str, Any]:
        """Request durable cancellation; its acknowledgment does not prove cleanup."""
        return cast(
            "dict[str, Any]",
            await self._build_request(
                "POST", f"{self._fleet_build_path(build_id)}/cancel", json=body
            ),
        )

    async def fleet_build_deliver(self, build_id: str, body: dict[str, Any]) -> dict[str, Any]:
        """Ask the controller to deliver a persisted build command."""
        return cast(
            "dict[str, Any]",
            await self._build_request(
                "POST", f"{self._fleet_build_path(build_id)}/deliver", json=body
            ),
        )

    async def fleet_build_claim_run(
        self, build_id: str, claim: dict[str, Any], supervisor_key: str
    ) -> dict[str, Any]:
        """Request a persisted run grant with separate supervisor authentication."""
        return cast(
            "dict[str, Any]",
            await self._build_request(
                "POST",
                f"{self._fleet_build_path(build_id)}/claim-run",
                json=claim,
                headers={"X-Fleet-Build-Key": supervisor_key},
            ),
        )

    async def fleet_build_fact(
        self, build_id: str, fact: dict[str, Any], supervisor_key: str
    ) -> dict[str, Any]:
        """Submit a fenced result; the controller decides whether to accept it."""
        return cast(
            "dict[str, Any]",
            await self._build_request(
                "POST",
                f"{self._fleet_build_path(build_id)}/facts",
                json=fact,
                headers={"X-Fleet-Build-Key": supervisor_key},
            ),
        )

    async def fleet_build_logs(
        self, build_id: str, *, stream: str = "stdout", after: int = 0, limit: int = 65536
    ) -> dict[str, Any]:
        """Read a bounded log page through the controller's configured backend."""
        if stream not in ("stdout", "stderr"):
            raise ValueError("build log stream must be stdout or stderr")
        if type(after) is not int or not 0 <= after <= 2**63 - 1:
            raise ValueError("build log cursor must be an integer from 0 through 2**63 - 1")
        if type(limit) is not int or not 1 <= limit <= 65536:
            raise ValueError("build log limit must be an integer from 1 through 65536")
        return cast(
            "dict[str, Any]",
            await self._build_request(
                "GET",
                f"{self._fleet_build_path(build_id)}/logs",
                params={"stream": stream, "after": after, "limit": limit},
            ),
        )

    # ── Internal request helper ────────────────────────────────────────────────

    async def _build_request(self, method: str, path: str, **kwargs: Any) -> Any:
        """Use the ordinary authenticated client with a finite build response budget."""
        if not math.isfinite(self._config.timeout):
            raise ValueError("build requests require a finite total timeout")
        return await self._request(method, path, _bounded=True, **kwargs)

    async def _bounded_request(self, method: str, path: str, **kwargs: Any) -> Any:
        """Read at most 512 KiB of uncompressed JSON, closing on every outcome."""
        headers = dict(kwargs.pop("headers", {}) or {})
        headers["Accept-Encoding"] = "identity"
        async with self._client.stream(
            method, path, headers=headers, follow_redirects=False, **kwargs
        ) as response:
            if response.headers.get("content-encoding", "identity").strip().lower() not in {
                "",
                "identity",
            }:
                raise AgamemnonConnectionError(
                    "build response uses an unsupported content encoding"
                )
            body = bytearray()
            async for chunk in response.aiter_bytes(chunk_size=65536):
                if len(body) + len(chunk) > 512 * 1024:
                    raise AgamemnonConnectionError("build response exceeds 512 KiB")
                body.extend(chunk)
            completed = httpx.Response(
                response.status_code,
                headers=response.headers,
                content=bytes(body),
                request=response.request,
            )
            return self._response_value(completed, reject_redirects=True)

    async def _request(
        self, method: str, path: str, *, _bounded: bool = False, **kwargs: Any
    ) -> Any:
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
            if _bounded:
                return await asyncio.wait_for(
                    self._bounded_request(method, path, **kwargs), timeout=self._config.timeout
                )
            response = await self._client.request(method, path, **kwargs)
        except httpx.ConnectError as exc:
            raise AgamemnonConnectionError(
                f"Cannot connect to Agamemnon at {self._base_url}: {exc}"
            ) from exc
        except httpx.TimeoutException as exc:
            raise AgamemnonConnectionError(f"Request to Agamemnon timed out: {exc}") from exc
        except asyncio.TimeoutError as exc:
            raise AgamemnonConnectionError("build request exceeded its total deadline") from exc

        return self._response_value(response)

    @staticmethod
    def _response_value(response: httpx.Response, *, reject_redirects: bool = False) -> Any:
        """Preserve the ordinary response/error contract for both transport paths."""
        if response.is_error or (reject_redirects and response.is_redirect):
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
        data = await self._request("POST", "/v1/agents", json=agent.model_dump(exclude_none=True))
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
        data = await self._request("POST", "/v1/teams", json=team.model_dump(exclude_none=True))
        return Team.model_validate(data.get("team", data))

    async def get_team(self, team_id: str) -> Team:
        """Get a team by ID."""
        data = await self._request("GET", f"/v1/teams/{team_id}")
        return Team.model_validate(data.get("team", data))

    async def update_team(self, team_id: str, update: TeamUpdate) -> Team:
        """Fully replace a team (PUT)."""
        data = await self._request("PUT", f"/v1/teams/{team_id}", json=update.model_dump())
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
