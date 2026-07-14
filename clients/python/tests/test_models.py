"""Tests for agamemnon_client Pydantic models."""

import pytest

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


class TestAgamemnonConfig:
    def test_defaults(self) -> None:
        cfg = AgamemnonConfig()
        assert cfg.host == "localhost"
        assert cfg.port == 8080
        assert cfg.timeout == 30.0

    def test_custom_values(self) -> None:
        cfg = AgamemnonConfig(host="10.0.0.1", port=9090, timeout=5.0)
        assert cfg.host == "10.0.0.1"
        assert cfg.port == 9090
        assert cfg.timeout == 5.0


class TestHealthResponse:
    def test_parse(self) -> None:
        h = HealthResponse.model_validate({"status": "ok"})
        assert h.status == "ok"


class TestVersionResponse:
    def test_parse(self) -> None:
        v = VersionResponse.model_validate({"version": "0.1.0", "name": "Agamemnon"})
        assert v.version == "0.1.0"
        assert v.name == "Agamemnon"


class TestAgent:
    def test_minimal(self) -> None:
        a = Agent.model_validate({"id": "abc", "name": "worker-1"})
        assert a.id == "abc"
        assert a.name == "worker-1"
        assert a.role == "worker"
        assert a.status == "offline"
        assert a.host == "local"
        assert a.programArgs == []
        assert a.tags == []

    def test_full(self) -> None:
        a = Agent.model_validate(
            {
                "id": "abc",
                "name": "worker-1",
                "label": "Label",
                "program": "/bin/python",
                "workingDirectory": "/tmp",
                "programArgs": ["-m", "foo"],
                "taskDescription": "desc",
                "tags": ["t1"],
                "owner": "team-a",
                "role": "lead",
                "host": "docker",
                "status": "online",
                "createdAt": "2026-01-01T00:00:00Z",
            }
        )
        assert a.status == "online"
        assert a.role == "lead"
        assert a.programArgs == ["-m", "foo"]


class TestAgentCreate:
    def test_name_required(self) -> None:
        with pytest.raises(Exception):
            AgentCreate.model_validate({})

    def test_defaults(self) -> None:
        ac = AgentCreate(name="bot")
        assert ac.host == "local"
        assert ac.role == "worker"
        assert ac.programArgs == []


class TestAgentDockerCreate:
    def test_docker_defaults(self) -> None:
        adc = AgentDockerCreate(name="docker-bot")
        assert adc.host == "docker"
        assert adc.image == ""
        assert adc.hostId == ""


class TestAgentUpdate:
    def test_all_optional(self) -> None:
        u = AgentUpdate()
        assert u.name is None
        assert u.status is None

    def test_partial_update(self) -> None:
        u = AgentUpdate(status="online")
        assert u.status == "online"
        assert u.name is None


class TestTeam:
    def test_minimal(self) -> None:
        t = Team.model_validate({"id": "t1", "name": "alpha"})
        assert t.agentIds == []

    def test_with_agents(self) -> None:
        t = Team.model_validate({"id": "t1", "name": "alpha", "agentIds": ["a1", "a2"]})
        assert t.agentIds == ["a1", "a2"]


class TestTeamCreate:
    def test_name_required(self) -> None:
        with pytest.raises(Exception):
            TeamCreate.model_validate({})

    def test_default_agent_ids(self) -> None:
        tc = TeamCreate(name="alpha")
        assert tc.agentIds == []


class TestTeamUpdate:
    def test_full_replace(self) -> None:
        tu = TeamUpdate(name="beta", agentIds=["a1"])
        assert tu.name == "beta"
        assert tu.agentIds == ["a1"]


class TestTask:
    def test_minimal(self) -> None:
        t = Task.model_validate({"id": "t1", "teamId": "team1", "subject": "Do thing"})
        assert t.status == "pending"
        assert t.type == "general"
        assert t.blockedBy == []
        assert t.completedAt is None

    def test_completed_at_can_be_null(self) -> None:
        t = Task.model_validate(
            {"id": "t1", "teamId": "team1", "subject": "X", "completedAt": None}
        )
        assert t.completedAt is None

    def test_completed_at_set(self) -> None:
        t = Task.model_validate(
            {
                "id": "t1",
                "teamId": "team1",
                "subject": "X",
                "status": "completed",
                "completedAt": "2026-01-02T00:00:00Z",
            }
        )
        assert t.completedAt == "2026-01-02T00:00:00Z"


class TestTaskCreate:
    def test_subject_required(self) -> None:
        with pytest.raises(Exception):
            TaskCreate.model_validate({})

    def test_defaults(self) -> None:
        tc = TaskCreate(subject="Fix bug")
        assert tc.type == "general"
        assert tc.blockedBy == []


class TestTaskUpdate:
    def test_all_optional(self) -> None:
        tu = TaskUpdate()
        assert tu.subject is None
        assert tu.status is None


class TestFailureSpec:
    def test_type_required(self) -> None:
        with pytest.raises(Exception):
            FailureSpec.model_validate({})

    def test_defaults(self) -> None:
        fs = FailureSpec(type="network-drop")
        assert fs.parameters == {}

    def test_with_parameters(self) -> None:
        fs = FailureSpec(type="latency", parameters={"delay_ms": 200})
        assert fs.parameters == {"delay_ms": 200}


class TestInjectionResult:
    def test_parse(self) -> None:
        ir = InjectionResult.model_validate(
            {"id": "fault-1", "type": "network-drop", "active": True, "createdAt": "2026-01-01"}
        )
        assert ir.id == "fault-1"
        assert ir.active is True

    def test_defaults(self) -> None:
        ir = InjectionResult.model_validate({"id": "f1", "type": "cpu-spike"})
        assert ir.active is True


class TestChaosEntry:
    def test_parse(self) -> None:
        ce = ChaosEntry.model_validate({"id": "c1", "type": "disk-full"})
        assert ce.id == "c1"
        assert ce.active is True
