"""Tests for AgamemnonClient using respx to mock httpx."""

from __future__ import annotations

import httpx
import pytest
import respx

from agamemnon_client.client import AgamemnonClient
from agamemnon_client.errors import AgamemnonAPIError, AgamemnonConnectionError
from agamemnon_client.models import (
    AgamemnonConfig,
    AgentCreate,
    AgentDockerCreate,
    AgentUpdate,
    FailureSpec,
    TaskCreate,
    TaskUpdate,
    TeamCreate,
    TeamUpdate,
)

BASE_URL = "http://localhost:8080"

AGENT_PAYLOAD = {
    "id": "agent-1",
    "name": "worker-1",
    "label": "",
    "program": "/bin/bot",
    "workingDirectory": "/tmp",
    "programArgs": [],
    "taskDescription": "",
    "tags": [],
    "owner": "team-alpha",
    "role": "worker",
    "host": "local",
    "status": "offline",
    "createdAt": "2026-01-01T00:00:00Z",
}

TEAM_PAYLOAD = {
    "id": "team-1",
    "name": "alpha",
    "agentIds": [],
    "createdAt": "2026-01-01T00:00:00Z",
}

TASK_PAYLOAD: dict[str, object] = {
    "id": "task-1",
    "teamId": "team-1",
    "subject": "Do the thing",
    "description": "",
    "assigneeAgentId": "",
    "blockedBy": [],
    "type": "general",
    "status": "pending",
    "createdAt": "2026-01-01T00:00:00Z",
    "completedAt": None,
}

FAULT_PAYLOAD = {
    "id": "fault-1",
    "type": "network-drop",
    "active": True,
    "createdAt": "2026-01-01T00:00:00Z",
}


@pytest.fixture
def client() -> AgamemnonClient:
    return AgamemnonClient(AgamemnonConfig(host="localhost", port=8080))


# ── Context manager ────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_context_manager() -> None:
    async with AgamemnonClient() as c:
        assert c is not None


# ── Health ─────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_health_ok(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/health").mock(
        return_value=httpx.Response(200, json={"status": "ok"})
    )
    h = await client.health()
    assert h is not None
    assert h.status == "ok"


@pytest.mark.asyncio
@respx.mock
async def test_health_returns_none_on_connection_error(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/health").mock(side_effect=httpx.ConnectError("refused"))
    h = await client.health()
    assert h is None


@pytest.mark.asyncio
@respx.mock
async def test_health_returns_none_on_500(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/health").mock(
        return_value=httpx.Response(500, json={"error": "crash"})
    )
    h = await client.health()
    assert h is None


# ── Version ────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_version(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/version").mock(
        return_value=httpx.Response(200, json={"version": "0.1.0", "name": "ProjectAgamemnon"})
    )
    v = await client.version()
    assert v.version == "0.1.0"
    assert v.name == "ProjectAgamemnon"


# ── _request error mapping ─────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_request_raises_connection_error(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents").mock(side_effect=httpx.ConnectError("refused"))
    with pytest.raises(AgamemnonConnectionError):
        await client.list_agents()


@pytest.mark.asyncio
@respx.mock
async def test_request_raises_connection_error_on_timeout(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents").mock(side_effect=httpx.TimeoutException("timeout"))
    with pytest.raises(AgamemnonConnectionError):
        await client.list_agents()


@pytest.mark.asyncio
@respx.mock
async def test_request_raises_api_error_on_404(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents/nonexistent").mock(
        return_value=httpx.Response(404, json={"error": "agent not found"})
    )
    with pytest.raises(AgamemnonAPIError) as exc_info:
        await client.get_agent("nonexistent")
    assert exc_info.value.status_code == 404
    assert "agent not found" in exc_info.value.message


@pytest.mark.asyncio
@respx.mock
async def test_request_raises_api_error_on_400(client: AgamemnonClient) -> None:
    respx.post(f"{BASE_URL}/v1/agents").mock(
        return_value=httpx.Response(400, json={"error": "invalid JSON: syntax error"})
    )
    with pytest.raises(AgamemnonAPIError) as exc_info:
        await client.create_agent(AgentCreate(name="bad"))
    assert exc_info.value.status_code == 400


@pytest.mark.asyncio
@respx.mock
async def test_request_api_error_falls_back_to_text_on_non_json(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents/x").mock(
        return_value=httpx.Response(503, text="Service Unavailable")
    )
    with pytest.raises(AgamemnonAPIError) as exc_info:
        await client.get_agent("x")
    assert exc_info.value.status_code == 503


# ── Agents ─────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_list_agents(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents").mock(
        return_value=httpx.Response(200, json=[AGENT_PAYLOAD])
    )
    agents = await client.list_agents()
    assert len(agents) == 1
    assert agents[0].id == "agent-1"


@pytest.mark.asyncio
@respx.mock
async def test_list_agents_wrapped(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents").mock(
        return_value=httpx.Response(200, json={"agents": [AGENT_PAYLOAD]})
    )
    agents = await client.list_agents()
    assert len(agents) == 1


@pytest.mark.asyncio
@respx.mock
async def test_create_agent(client: AgamemnonClient) -> None:
    respx.post(f"{BASE_URL}/v1/agents").mock(
        return_value=httpx.Response(201, json={"agent": AGENT_PAYLOAD})
    )
    agent = await client.create_agent(AgentCreate(name="worker-1"))
    assert agent.id == "agent-1"
    assert agent.name == "worker-1"


@pytest.mark.asyncio
@respx.mock
async def test_create_docker_agent(client: AgamemnonClient) -> None:
    docker_payload = {**AGENT_PAYLOAD, "host": "docker"}
    respx.post(f"{BASE_URL}/v1/agents/docker").mock(
        return_value=httpx.Response(201, json={"agent": docker_payload})
    )
    agent = await client.create_docker_agent(AgentDockerCreate(name="docker-bot"))
    assert agent.host == "docker"


@pytest.mark.asyncio
@respx.mock
async def test_get_agent(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents/agent-1").mock(
        return_value=httpx.Response(200, json={"agent": AGENT_PAYLOAD})
    )
    agent = await client.get_agent("agent-1")
    assert agent.id == "agent-1"


@pytest.mark.asyncio
@respx.mock
async def test_get_agent_by_name(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/agents/by-name/worker-1").mock(
        return_value=httpx.Response(200, json={"agent": AGENT_PAYLOAD})
    )
    agent = await client.get_agent_by_name("worker-1")
    assert agent.name == "worker-1"


@pytest.mark.asyncio
@respx.mock
async def test_update_agent(client: AgamemnonClient) -> None:
    updated = {**AGENT_PAYLOAD, "status": "online"}
    respx.patch(f"{BASE_URL}/v1/agents/agent-1").mock(
        return_value=httpx.Response(200, json={"agent": updated})
    )
    agent = await client.update_agent("agent-1", AgentUpdate(status="online"))
    assert agent.status == "online"


@pytest.mark.asyncio
@respx.mock
async def test_start_agent(client: AgamemnonClient) -> None:
    started = {**AGENT_PAYLOAD, "status": "online"}
    respx.post(f"{BASE_URL}/v1/agents/agent-1/start").mock(
        return_value=httpx.Response(200, json=started)
    )
    agent = await client.start_agent("agent-1")
    assert agent.status == "online"


@pytest.mark.asyncio
@respx.mock
async def test_stop_agent(client: AgamemnonClient) -> None:
    stopped = {**AGENT_PAYLOAD, "status": "offline"}
    respx.post(f"{BASE_URL}/v1/agents/agent-1/stop").mock(
        return_value=httpx.Response(200, json=stopped)
    )
    agent = await client.stop_agent("agent-1")
    assert agent.status == "offline"


@pytest.mark.asyncio
@respx.mock
async def test_delete_agent(client: AgamemnonClient) -> None:
    respx.delete(f"{BASE_URL}/v1/agents/agent-1").mock(
        return_value=httpx.Response(200, json={"deleted": "agent-1"})
    )
    deleted_id = await client.delete_agent("agent-1")
    assert deleted_id == "agent-1"


# ── Teams ──────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_list_teams(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/teams").mock(
        return_value=httpx.Response(200, json=[TEAM_PAYLOAD])
    )
    teams = await client.list_teams()
    assert len(teams) == 1
    assert teams[0].id == "team-1"


@pytest.mark.asyncio
@respx.mock
async def test_create_team(client: AgamemnonClient) -> None:
    respx.post(f"{BASE_URL}/v1/teams").mock(
        return_value=httpx.Response(201, json={"team": TEAM_PAYLOAD})
    )
    team = await client.create_team(TeamCreate(name="alpha"))
    assert team.id == "team-1"


@pytest.mark.asyncio
@respx.mock
async def test_get_team(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/teams/team-1").mock(
        return_value=httpx.Response(200, json={"team": TEAM_PAYLOAD})
    )
    team = await client.get_team("team-1")
    assert team.name == "alpha"


@pytest.mark.asyncio
@respx.mock
async def test_update_team(client: AgamemnonClient) -> None:
    updated = {**TEAM_PAYLOAD, "name": "beta"}
    respx.put(f"{BASE_URL}/v1/teams/team-1").mock(
        return_value=httpx.Response(200, json={"team": updated})
    )
    team = await client.update_team("team-1", TeamUpdate(name="beta"))
    assert team.name == "beta"


@pytest.mark.asyncio
@respx.mock
async def test_delete_team(client: AgamemnonClient) -> None:
    respx.delete(f"{BASE_URL}/v1/teams/team-1").mock(
        return_value=httpx.Response(200, json={"deleted": "team-1"})
    )
    deleted_id = await client.delete_team("team-1")
    assert deleted_id == "team-1"


# ── Tasks ──────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_list_tasks(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/tasks").mock(
        return_value=httpx.Response(200, json=[TASK_PAYLOAD])
    )
    tasks = await client.list_tasks()
    assert len(tasks) == 1
    assert tasks[0].id == "task-1"


@pytest.mark.asyncio
@respx.mock
async def test_list_team_tasks(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/teams/team-1/tasks").mock(
        return_value=httpx.Response(200, json=[TASK_PAYLOAD])
    )
    tasks = await client.list_team_tasks("team-1")
    assert len(tasks) == 1


@pytest.mark.asyncio
@respx.mock
async def test_create_task(client: AgamemnonClient) -> None:
    respx.post(f"{BASE_URL}/v1/teams/team-1/tasks").mock(
        return_value=httpx.Response(201, json={"task": TASK_PAYLOAD})
    )
    task = await client.create_task("team-1", TaskCreate(subject="Do the thing"))
    assert task.id == "task-1"
    assert task.subject == "Do the thing"


@pytest.mark.asyncio
@respx.mock
async def test_get_task(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/teams/team-1/tasks/task-1").mock(
        return_value=httpx.Response(200, json={"task": TASK_PAYLOAD})
    )
    task = await client.get_task("team-1", "task-1")
    assert task.status == "pending"


@pytest.mark.asyncio
@respx.mock
async def test_update_task_put(client: AgamemnonClient) -> None:
    completed = {**TASK_PAYLOAD, "status": "completed", "completedAt": "2026-01-02T00:00:00Z"}
    respx.put(f"{BASE_URL}/v1/teams/team-1/tasks/task-1").mock(
        return_value=httpx.Response(200, json={"task": completed})
    )
    task = await client.update_task(
        "team-1", "task-1", TaskUpdate(status="completed"), partial=False
    )
    assert task.status == "completed"


@pytest.mark.asyncio
@respx.mock
async def test_update_task_patch(client: AgamemnonClient) -> None:
    completed = {**TASK_PAYLOAD, "status": "completed"}
    respx.patch(f"{BASE_URL}/v1/teams/team-1/tasks/task-1").mock(
        return_value=httpx.Response(200, json={"task": completed})
    )
    task = await client.update_task(
        "team-1", "task-1", TaskUpdate(status="completed"), partial=True
    )
    assert task.status == "completed"


# ── Chaos ──────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_list_chaos(client: AgamemnonClient) -> None:
    respx.get(f"{BASE_URL}/v1/chaos").mock(
        return_value=httpx.Response(200, json=[FAULT_PAYLOAD])
    )
    faults = await client.list_chaos()
    assert len(faults) == 1
    assert faults[0].id == "fault-1"


@pytest.mark.asyncio
@respx.mock
async def test_inject_chaos_no_spec(client: AgamemnonClient) -> None:
    respx.post(f"{BASE_URL}/v1/chaos/network-drop").mock(
        return_value=httpx.Response(201, json={"fault": FAULT_PAYLOAD})
    )
    result = await client.inject_chaos("network-drop")
    assert result.id == "fault-1"
    assert result.type == "network-drop"


@pytest.mark.asyncio
@respx.mock
async def test_inject_chaos_with_spec(client: AgamemnonClient) -> None:
    respx.post(f"{BASE_URL}/v1/chaos/latency").mock(
        return_value=httpx.Response(
            201,
            json={"fault": {**FAULT_PAYLOAD, "type": "latency"}},
        )
    )
    spec = FailureSpec(type="latency", parameters={"delay_ms": 200})
    result = await client.inject_chaos("latency", spec)
    assert result.type == "latency"


@pytest.mark.asyncio
@respx.mock
async def test_delete_chaos(client: AgamemnonClient) -> None:
    respx.delete(f"{BASE_URL}/v1/chaos/fault-1").mock(
        return_value=httpx.Response(200, json={"deleted": "fault-1"})
    )
    deleted_id = await client.delete_chaos("fault-1")
    assert deleted_id == "fault-1"


# ── 204 / empty body ──────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_request_returns_none_on_204(client: AgamemnonClient) -> None:
    # health() absorbs any exception, so use version() as a proxy but override
    # the response to 204 by calling _request directly
    respx.get(f"{BASE_URL}/v1/version").mock(return_value=httpx.Response(204))
    result = await client._request("GET", "/v1/version")
    assert result is None


# ── aclose ─────────────────────────────────────────────────────────────────────


@pytest.mark.asyncio
async def test_aclose() -> None:
    c = AgamemnonClient()
    await c.aclose()  # Should not raise


# ── Authentication ─────────────────────────────────────────────────────────────


@pytest.mark.asyncio
@respx.mock
async def test_request_sends_bearer_when_api_key_configured() -> None:
    """When AgamemnonConfig.api_key is set, every request carries Authorization: Bearer."""
    config = AgamemnonConfig(host="localhost", port=8080, api_key="s3cret-key")
    authed = AgamemnonClient(config)
    route = respx.get(f"{BASE_URL}/v1/version").mock(
        return_value=httpx.Response(
            200, json={"version": "0.1.0", "name": "ProjectAgamemnon"}
        )
    )
    try:
        await authed.version()
    finally:
        await authed.aclose()

    assert route.called
    sent = route.calls.last.request
    assert sent.headers.get("Authorization") == "Bearer s3cret-key"


@pytest.mark.asyncio
@respx.mock
async def test_request_omits_authorization_when_api_key_unset() -> None:
    """With no api_key, no Authorization header is added (dev escape hatch)."""
    config = AgamemnonConfig(host="localhost", port=8080, api_key=None)
    unauthed = AgamemnonClient(config)
    route = respx.get(f"{BASE_URL}/v1/version").mock(
        return_value=httpx.Response(
            200, json={"version": "0.1.0", "name": "ProjectAgamemnon"}
        )
    )
    try:
        await unauthed.version()
    finally:
        await unauthed.aclose()

    assert route.called
    sent = route.calls.last.request
    assert "Authorization" not in sent.headers
